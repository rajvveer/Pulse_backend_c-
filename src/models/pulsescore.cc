// pulsescore.cc — C++ port of src/models/PulseScore.js.
// See include/pulse/models/pulsescore.hpp.
//
// Preserves the exact schema defaults, tier table, score math (Math.log1p /
// Math.round / Math.min semantics), streak tracking, achievement checks, and the
// getOrCreate / getLeaderboard / getUserRank statics from the Mongoose model.
// Collection name and field names match the JS schema 1:1.
#include "pulse/models/pulsescore.hpp"

#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/oid.hpp>

#include <mongocxx/collection.hpp>
#include <mongocxx/cursor.hpp>
#include <mongocxx/pipeline.hpp>
#include <mongocxx/options/index.hpp>
#include <mongocxx/options/find.hpp>

#include <chrono>
#include <cmath>
#include <ctime>
#include <set>
#include <string>

namespace pulse::models::pulsescore {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;
using bld::make_array;

// =========================================================
//  TIER CONFIGURATION
// =========================================================
const std::vector<TierConfig>& tiers() {
  // Order matches Object.entries(TIERS) in the JS source.
  static const std::vector<TierConfig> kTiers = {
      {kTierNewcomer,   0,   199,  "\xF0\x9F\x8C\xB1", "Newcomer",     "#8BC34A"},
      {kTierRising,     200, 399,  "\xE2\xAD\x90",     "Rising Star",  "#FFC107"},
      {kTierEstablished,400, 599,  "\xF0\x9F\x92\xAB", "Established",   "#FF9800"},
      {kTierInfluencer, 600, 799,  "\xF0\x9F\x94\xA5", "Influencer",   "#F44336"},
      {kTierIcon,       800, 1000, "\xF0\x9F\x91\x91", "Icon",         "#9C27B0"},
  };
  return kTiers;
}

const TierConfig* tierConfig(const std::string& tier) {
  for (const auto& t : tiers()) {
    if (tier == t.key) return &t;
  }
  return nullptr;
}

namespace {

// ---- date helpers (mirror new Date().toISOString().split('T')[0]) ----

// today's date as "YYYY-MM-DD" in UTC.
std::string todayStr() {
  std::time_t tt = std::chrono::system_clock::to_time_t(
      std::chrono::system_clock::now());
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &tt);
#else
  gmtime_r(&tt, &tm);
#endif
  char buf[16];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
  return std::string(buf);
}

// yesterday's date as "YYYY-MM-DD" in UTC (new Date(); setDate(getDate()-1)).
std::string yesterdayStr() {
  auto when = std::chrono::system_clock::now() - std::chrono::hours(24);
  std::time_t tt = std::chrono::system_clock::to_time_t(when);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &tt);
#else
  gmtime_r(&tt, &tm);
#endif
  char buf[16];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
  return std::string(buf);
}

// ---- numeric helpers preserving JS Math.* semantics ----

// JS numbers from a possibly-missing JSON field, default 0.
double num(const Json::Value& v, const char* key) {
  if (!v.isMember(key)) return 0.0;
  const Json::Value& f = v[key];
  if (f.isNumeric()) return f.asDouble();
  if (f.isString()) { try { return std::stod(f.asString()); } catch (...) { return 0.0; } }
  return 0.0;
}

// Math.round: JS rounds half-away-from-zero toward +Infinity for .5
// (Math.round(2.5)==3, Math.round(-2.5)==-2). All values here are non-negative,
// so std::floor(x + 0.5) matches exactly.
long long jsRound(double x) {
  return static_cast<long long>(std::floor(x + 0.5));
}

// ---- nested default scaffolding ----

void ensureNumber(Json::Value& obj, const char* key, double def) {
  if (!obj.isMember(key) || !obj[key].isNumeric()) obj[key] = def;
}

void applyComponentDefaults(Json::Value& doc) {
  if (!doc.isMember("components") || !doc["components"].isObject())
    doc["components"] = Json::Value(Json::objectValue);
  Json::Value& c = doc["components"];
  ensureNumber(c, "engagement", 0);
  ensureNumber(c, "consistency", 0);
  ensureNumber(c, "community", 0);
  ensureNumber(c, "reach", 0);
  ensureNumber(c, "creativity", 0);
}

void applyMetricDefaults(Json::Value& doc) {
  if (!doc.isMember("metrics") || !doc["metrics"].isObject())
    doc["metrics"] = Json::Value(Json::objectValue);
  Json::Value& m = doc["metrics"];
  ensureNumber(m, "totalPosts", 0);
  ensureNumber(m, "totalLikesGiven", 0);
  ensureNumber(m, "totalLikesReceived", 0);
  ensureNumber(m, "totalCommentsGiven", 0);
  ensureNumber(m, "totalCommentsReceived", 0);
  ensureNumber(m, "totalFollowers", 0);
  ensureNumber(m, "totalFollowing", 0);
  ensureNumber(m, "totalShares", 0);
  ensureNumber(m, "totalViews", 0);
  ensureNumber(m, "uniqueVibes", 0);
  ensureNumber(m, "mediaPostsCount", 0);
  ensureNumber(m, "daysActive", 0);
  ensureNumber(m, "currentStreak", 0);
  ensureNumber(m, "longestStreak", 0);
  if (!m.isMember("lastActiveDate") || !m["lastActiveDate"].isString())
    m["lastActiveDate"] = "";
}

// The user-projection fields the JS .populate('user', '...') pulls in.
bsoncxx::document::value userProjection() {
  return make_document(
      kvp("username", 1),
      kvp("profile.displayName", 1),
      kvp("profile.avatar", 1),
      kvp("isVerified", 1));
}

} // namespace

// =========================================================
//  INDEXES
// =========================================================
void ensureIndexes() {
  auto col = pulse::db::collection(kCollection);

  // user: { index: true, unique: true } (field-level on the schema).
  {
    mongocxx::options::index opts{};
    opts.unique(true);
    col.create_index(make_document(kvp("user", 1)), opts);
  }

  // pulseScoreSchema.index({ score: -1 }); // For leaderboards
  col.create_index(make_document(kvp("score", -1)));

  // pulseScoreSchema.index({ tier: 1 });
  col.create_index(make_document(kvp("tier", 1)));

  pulse::log::info("Ensured indexes for collection '{}'", kCollection);
}

// =========================================================
//  DEFAULTS / OUTPUT SHAPING
// =========================================================
Json::Value applyDefaults(Json::Value doc) {
  // score: default 0
  ensureNumber(doc, "score", 0);

  // tier: enum, default 'newcomer'
  if (!doc.isMember("tier") || !doc["tier"].isString() ||
      doc["tier"].asString().empty()) {
    doc["tier"] = kTierNewcomer;
  }

  // components.* and metrics.* defaults
  applyComponentDefaults(doc);
  applyMetricDefaults(doc);

  // history: [] default
  if (!doc.isMember("history") || !doc["history"].isArray())
    doc["history"] = Json::Value(Json::arrayValue);

  // achievements: [] default
  if (!doc.isMember("achievements") || !doc["achievements"].isArray())
    doc["achievements"] = Json::Value(Json::arrayValue);

  // lastComputedAt: default now
  std::string now = pulse::bsonjson::nowIso8601();
  if (!doc.isMember("lastComputedAt") || doc["lastComputedAt"].isNull())
    doc["lastComputedAt"] = now;

  // timestamps: createdAt / updatedAt
  if (!doc.isMember("createdAt")) doc["createdAt"] = now;
  if (!doc.isMember("updatedAt")) doc["updatedAt"] = now;

  return doc;
}

Json::Value sanitizeForOutput(Json::Value doc) {
  // The PulseScore schema declares no select:false / sensitive fields. The
  // default Mongoose toJSON drops the version key.
  doc.removeMember("__v");
  return doc;
}

// =========================================================
//  STATIC METHODS
// =========================================================
Json::Value getOrCreate(const std::string& userId) {
  auto col = pulse::db::collection(kCollection);
  const bsoncxx::oid userOid = pulse::bsonjson::oid(userId);

  // let ps = await this.findOne({ user: userId });
  auto existing = col.find_one(make_document(kvp("user", userOid)));
  if (existing) {
    return sanitizeForOutput(pulse::bsonjson::toJson(existing->view()));
  }

  // ps = new this({ user: userId }); await ps.save();
  Json::Value doc(Json::objectValue);
  doc["user"] = userId;
  doc = applyDefaults(doc);

  // Persist with the user field + timestamps + lastComputedAt as real BSON
  // dates (so they sort/compare correctly), and the rest from the defaulted doc.
  const bsoncxx::types::b_date now{
      std::chrono::milliseconds{pulse::bsonjson::nowMillis()}};

  bld::document insert;
  insert.append(kvp("user", userOid));
  insert.append(kvp("score", static_cast<int64_t>(jsRound(num(doc, "score")))));
  insert.append(kvp("tier", doc["tier"].asString()));

  // components subdocument
  {
    const Json::Value& c = doc["components"];
    insert.append(kvp("components", make_document(
        kvp("engagement",  static_cast<int64_t>(jsRound(c["engagement"].asDouble()))),
        kvp("consistency", static_cast<int64_t>(jsRound(c["consistency"].asDouble()))),
        kvp("community",   static_cast<int64_t>(jsRound(c["community"].asDouble()))),
        kvp("reach",       static_cast<int64_t>(jsRound(c["reach"].asDouble()))),
        kvp("creativity",  static_cast<int64_t>(jsRound(c["creativity"].asDouble()))))));
  }

  // metrics subdocument
  {
    const Json::Value& m = doc["metrics"];
    insert.append(kvp("metrics", make_document(
        kvp("totalPosts",            static_cast<int64_t>(jsRound(m["totalPosts"].asDouble()))),
        kvp("totalLikesGiven",       static_cast<int64_t>(jsRound(m["totalLikesGiven"].asDouble()))),
        kvp("totalLikesReceived",    static_cast<int64_t>(jsRound(m["totalLikesReceived"].asDouble()))),
        kvp("totalCommentsGiven",    static_cast<int64_t>(jsRound(m["totalCommentsGiven"].asDouble()))),
        kvp("totalCommentsReceived", static_cast<int64_t>(jsRound(m["totalCommentsReceived"].asDouble()))),
        kvp("totalFollowers",        static_cast<int64_t>(jsRound(m["totalFollowers"].asDouble()))),
        kvp("totalFollowing",        static_cast<int64_t>(jsRound(m["totalFollowing"].asDouble()))),
        kvp("totalShares",           static_cast<int64_t>(jsRound(m["totalShares"].asDouble()))),
        kvp("totalViews",            static_cast<int64_t>(jsRound(m["totalViews"].asDouble()))),
        kvp("uniqueVibes",           static_cast<int64_t>(jsRound(m["uniqueVibes"].asDouble()))),
        kvp("mediaPostsCount",       static_cast<int64_t>(jsRound(m["mediaPostsCount"].asDouble()))),
        kvp("daysActive",            static_cast<int64_t>(jsRound(m["daysActive"].asDouble()))),
        kvp("currentStreak",         static_cast<int64_t>(jsRound(m["currentStreak"].asDouble()))),
        kvp("longestStreak",         static_cast<int64_t>(jsRound(m["longestStreak"].asDouble()))),
        kvp("lastActiveDate",        m["lastActiveDate"].asString()))));
  }

  insert.append(kvp("history", bld::array{}));
  insert.append(kvp("achievements", bld::array{}));
  insert.append(kvp("lastComputedAt", now));
  insert.append(kvp("createdAt", now));
  insert.append(kvp("updatedAt", now));

  auto result = col.insert_one(insert.extract());

  // Re-read so the returned JSON mirrors the persisted document (incl. _id).
  if (result) {
    auto created = col.find_one(make_document(kvp("user", userOid)));
    if (created) return sanitizeForOutput(pulse::bsonjson::toJson(created->view()));
  }
  return sanitizeForOutput(doc);
}

Json::Value getLeaderboard(long long limit) {
  auto col = pulse::db::collection(kCollection);

  // find({ score: { $gt: 0 } }).sort({ score: -1 }).limit(limit)
  //   .populate('user', 'username profile.displayName profile.avatar isVerified')
  //   .lean()
  // Implemented as an aggregation so the populate is replicated via $lookup.
  mongocxx::pipeline p;
  p.match(make_document(kvp("score", make_document(kvp("$gt", 0)))));
  p.sort(make_document(kvp("score", -1)));
  if (limit > 0) p.limit(limit);
  p.lookup(make_document(
      kvp("from", "users"),
      kvp("localField", "user"),
      kvp("foreignField", "_id"),
      kvp("pipeline", make_array(
          make_document(kvp("$project", userProjection())))),
      kvp("as", "userDoc")));
  p.add_fields(make_document(
      kvp("user", make_document(kvp("$ifNull", make_array(
          make_document(kvp("$arrayElemAt", make_array("$userDoc", 0))),
          "$user"))))));
  p.project(make_document(kvp("userDoc", 0)));

  Json::Value out(Json::arrayValue);
  auto cursor = col.aggregate(p);
  for (auto&& view : cursor) {
    out.append(sanitizeForOutput(pulse::bsonjson::toJson(view)));
  }
  return out;
}

std::optional<Json::Value> getUserRank(const std::string& userId) {
  auto col = pulse::db::collection(kCollection);
  const bsoncxx::oid userOid = pulse::bsonjson::oid(userId);

  // const userScore = await this.findOne({ user: userId });
  auto userScore = col.find_one(make_document(kvp("user", userOid)));
  if (!userScore) return std::nullopt;  // if (!userScore) return null;

  // Extract the numeric score (may be int32/int64/double in BSON).
  long long scoreVal = 0;
  {
    auto el = userScore->view()["score"];
    if (el) {
      if (el.type() == bsoncxx::type::k_int32)       scoreVal = el.get_int32().value;
      else if (el.type() == bsoncxx::type::k_int64)  scoreVal = el.get_int64().value;
      else if (el.type() == bsoncxx::type::k_double) scoreVal = static_cast<long long>(el.get_double().value);
    }
  }

  // const rank = await this.countDocuments({ score: { $gt: userScore.score } }) + 1;
  long long rank = col.count_documents(make_document(
      kvp("score", make_document(kvp("$gt", scoreVal))))) + 1;

  // const total = await this.countDocuments({ score: { $gt: 0 } });
  long long total = col.count_documents(make_document(
      kvp("score", make_document(kvp("$gt", 0)))));

  // percentile: Math.round(((total - rank) / total) * 100)
  // (total is >= 1 here since userScore.score >= 0 means it counts toward total
  // only when > 0; if total is 0 this divides by zero -> NaN in JS, preserved as
  // null here to avoid emitting NaN.)
  Json::Value result(Json::objectValue);
  result["rank"] = static_cast<Json::Int64>(rank);
  result["total"] = static_cast<Json::Int64>(total);
  if (total != 0) {
    double pct = (static_cast<double>(total - rank) / static_cast<double>(total)) * 100.0;
    result["percentile"] = static_cast<Json::Int64>(jsRound(pct));
  } else {
    result["percentile"] = Json::Value::nullSingleton();  // NaN -> null
  }
  return result;
}

// =========================================================
//  INSTANCE METHODS (over the document JSON)
// =========================================================
Json::Value recordAction(Json::Value doc, const std::string& action,
                         double value) {
  // Ensure metrics exist before mutating.
  applyMetricDefaults(doc);
  Json::Value& m = doc["metrics"];

  const std::string today = todayStr();

  // Update raw metrics by action.
  auto add = [&](const char* key) { m[key] = num(m, key) + value; };
  if (action == "post")             add("totalPosts");
  else if (action == "like_given")  add("totalLikesGiven");
  else if (action == "like_received") add("totalLikesReceived");
  else if (action == "comment_given") add("totalCommentsGiven");
  else if (action == "comment_received") add("totalCommentsReceived");
  else if (action == "follower_gained") add("totalFollowers");
  else if (action == "share")       add("totalShares");
  else if (action == "view_received") add("totalViews");
  else if (action == "media_post")  add("mediaPostsCount");

  // Update streak.
  if (m["lastActiveDate"].asString() != today) {
    const std::string yStr = yesterdayStr();

    if (m["lastActiveDate"].asString() == yStr) {
      m["currentStreak"] = num(m, "currentStreak") + 1;
    } else if (m["lastActiveDate"].asString() != today) {
      m["currentStreak"] = 1;
    }

    m["daysActive"] = num(m, "daysActive") + 1;
    m["lastActiveDate"] = today;

    if (num(m, "currentStreak") > num(m, "longestStreak")) {
      m["longestStreak"] = num(m, "currentStreak");
    }
  }

  // Recalculate score.
  return recalculate(std::move(doc));
}

Json::Value recalculate(Json::Value doc) {
  applyComponentDefaults(doc);
  applyMetricDefaults(doc);
  ensureNumber(doc, "score", 0);

  const Json::Value& m = doc["metrics"];
  Json::Value& c = doc["components"];

  const double totalPosts        = num(m, "totalPosts");
  const double totalLikesReceived = num(m, "totalLikesReceived");
  const double currentStreak     = num(m, "currentStreak");
  const double daysActive        = num(m, "daysActive");
  const double totalCommentsGiven = num(m, "totalCommentsGiven");
  const double totalLikesGiven   = num(m, "totalLikesGiven");
  const double totalFollowers    = num(m, "totalFollowers");
  const double totalViews        = num(m, "totalViews");
  const double mediaPostsCount   = num(m, "mediaPostsCount");
  const double uniqueVibes       = num(m, "uniqueVibes");

  // Engagement (0-200): quality ratio of likes received vs posts.
  const double engagementRatio = totalPosts > 0 ? totalLikesReceived / totalPosts : 0.0;
  c["engagement"] = static_cast<Json::Int64>(std::min<long long>(
      200, jsRound(std::log1p(engagementRatio * 10.0) * 40.0)));

  // Consistency (0-200): streaks and daily activity.
  c["consistency"] = static_cast<Json::Int64>(std::min<long long>(
      200, jsRound(
          (std::min(currentStreak, 30.0) / 30.0) * 100.0 +
          (std::min(daysActive, 90.0) / 90.0) * 100.0)));

  // Community (0-200): giving to others (comments, likes given).
  c["community"] = static_cast<Json::Int64>(std::min<long long>(
      200, jsRound(
          std::log1p(totalCommentsGiven * 3.0) * 20.0 +
          std::log1p(totalLikesGiven) * 10.0)));

  // Reach (0-200): follower count and view metrics.
  c["reach"] = static_cast<Json::Int64>(std::min<long long>(
      200, jsRound(
          std::log1p(totalFollowers * 5.0) * 25.0 +
          std::log1p(totalViews) * 5.0)));

  // Creativity (0-200): content diversity.
  const double mediaRatio = totalPosts > 0 ? mediaPostsCount / totalPosts : 0.0;
  c["creativity"] = static_cast<Json::Int64>(std::min<long long>(
      200, jsRound(
          mediaRatio * 100.0 +
          std::min(uniqueVibes, 5.0) * 20.0)));

  // Total score.
  const long long oldScore = static_cast<long long>(num(doc, "score"));
  const long long total =
      c["engagement"].asInt64() + c["consistency"].asInt64() +
      c["community"].asInt64() + c["reach"].asInt64() + c["creativity"].asInt64();
  const long long newScore = std::min<long long>(1000, total);
  doc["score"] = static_cast<Json::Int64>(newScore);

  // Update tier.
  doc = updateTier(std::move(doc));

  // Check achievements.
  doc = checkAchievements(std::move(doc));

  // Record in history (daily, not per action).
  if (!doc.isMember("history") || !doc["history"].isArray())
    doc["history"] = Json::Value(Json::arrayValue);
  Json::Value& history = doc["history"];

  const std::string today = todayStr();

  // lastHistoryDate = last history entry's date (YYYY-MM-DD) or ''.
  std::string lastHistoryDate;
  if (history.size() > 0) {
    const Json::Value& last = history[history.size() - 1];
    if (last.isObject() && last.isMember("date") && last["date"].isString()) {
      // new Date(lastHistory.date).toISOString().split('T')[0]
      const std::string d = last["date"].asString();
      // ISO-8601 string -> take the date portion before 'T'.
      auto tpos = d.find('T');
      lastHistoryDate = (tpos == std::string::npos) ? d : d.substr(0, tpos);
    }
  }

  if (lastHistoryDate != today) {
    Json::Value entry(Json::objectValue);
    entry["date"]  = pulse::bsonjson::nowIso8601();   // new Date()
    entry["score"] = static_cast<Json::Int64>(newScore);
    entry["tier"]  = doc["tier"];
    entry["delta"] = static_cast<Json::Int64>(newScore - oldScore);
    history.append(entry);

    // Keep last 90 days.
    if (history.size() > 90) {
      Json::Value trimmed(Json::arrayValue);
      Json::ArrayIndex start = history.size() - 90;
      for (Json::ArrayIndex i = start; i < history.size(); ++i)
        trimmed.append(history[i]);
      doc["history"] = trimmed;
    }
  }

  doc["lastComputedAt"] = pulse::bsonjson::nowIso8601();  // new Date()
  return doc;
}

Json::Value updateTier(Json::Value doc) {
  const long long score = static_cast<long long>(num(doc, "score"));
  for (const auto& t : tiers()) {
    if (score >= t.min && score <= t.max) {
      doc["tier"] = t.key;
      return doc;
    }
  }
  return doc;
}

Json::Value checkAchievements(Json::Value doc) {
  if (!doc.isMember("achievements") || !doc["achievements"].isArray())
    doc["achievements"] = Json::Value(Json::arrayValue);
  Json::Value& achievements = doc["achievements"];

  // earned = new Set(this.achievements.map(a => a.id))
  std::set<std::string> earned;
  for (const auto& a : achievements) {
    if (a.isObject() && a.isMember("id") && a["id"].isString())
      earned.insert(a["id"].asString());
  }

  applyMetricDefaults(doc);
  ensureNumber(doc, "score", 0);
  const Json::Value& m = doc["metrics"];
  const double score = num(doc, "score");

  // check(id, condition, name, description, emoji)
  auto check = [&](const char* id, bool condition, const char* name,
                   const char* description, const char* emoji) {
    if (earned.find(id) == earned.end() && condition) {
      Json::Value ach(Json::objectValue);
      ach["id"] = id;
      ach["name"] = name;
      ach["description"] = description;
      ach["emoji"] = emoji;
      // unlockedAt: default Date.now
      ach["unlockedAt"] = pulse::bsonjson::nowIso8601();
      achievements.append(ach);
      earned.insert(id);
    }
  };

  check("first_post",   num(m, "totalPosts") >= 1,            "First Post",        "Created your first post", "\xF0\x9F\x93\x9D");
  check("posts_10",     num(m, "totalPosts") >= 10,           "Content Creator",   "10 posts created",        "\xF0\x9F\x93\xB8");
  check("posts_50",     num(m, "totalPosts") >= 50,           "Prolific Poster",   "50 posts created",        "\xF0\x9F\x9A\x80");
  check("streak_3",     num(m, "currentStreak") >= 3,         "On a Roll",         "3-day activity streak",   "\xF0\x9F\x94\xA5");
  check("streak_7",     num(m, "currentStreak") >= 7,         "Week Warrior",      "7-day activity streak",   "\xF0\x9F\x92\xAA");
  check("streak_30",    num(m, "currentStreak") >= 30,        "Month Master",      "30-day activity streak",  "\xF0\x9F\x91\x91");
  check("score_100",    score >= 100,                          "Getting Started",   "Reached 100 Pulse Score", "\xE2\xAD\x90");
  check("score_500",    score >= 500,                          "Half Way",          "Reached 500 Pulse Score", "\xF0\x9F\x92\xAB");
  check("score_800",    score >= 800,                          "Elite Status",      "Reached 800 Pulse Score", "\xF0\x9F\x8F\x86");
  check("likes_100",    num(m, "totalLikesReceived") >= 100,  "Crowd Favorite",    "100 likes received",      "\xE2\x9D\xA4\xEF\xB8\x8F");
  check("community_50", num(m, "totalCommentsGiven") >= 50,   "Community Builder", "50 comments given",       "\xF0\x9F\xA4\x9D");
  check("followers_100",num(m, "totalFollowers") >= 100,      "Rising Influence",  "100 followers gained",    "\xF0\x9F\x93\x88");

  return doc;
}

Json::Value getDisplayData(const Json::Value& doc) {
  const std::string tier = doc.isMember("tier") && doc["tier"].isString()
                               ? doc["tier"].asString() : std::string(kTierNewcomer);
  const TierConfig* cfg = tierConfig(tier);

  const long long score = static_cast<long long>(num(doc, "score"));

  Json::Value out(Json::objectValue);
  out["score"] = static_cast<Json::Int64>(score);
  out["tier"] = tier;
  out["tierLabel"] = cfg ? cfg->label : "";
  out["tierEmoji"] = cfg ? cfg->emoji : "";
  out["tierColor"] = cfg ? cfg->color : "";
  out["components"] = doc.isMember("components") ? doc["components"]
                                                 : Json::Value(Json::objectValue);

  // streak: this.metrics.currentStreak
  long long streak = 0;
  if (doc.isMember("metrics"))
    streak = static_cast<long long>(num(doc["metrics"], "currentStreak"));
  out["streak"] = static_cast<Json::Int64>(streak);

  // achievements: this.achievements.length
  long long achCount = 0;
  if (doc.isMember("achievements") && doc["achievements"].isArray())
    achCount = static_cast<long long>(doc["achievements"].size());
  out["achievements"] = static_cast<Json::Int64>(achCount);

  // nextTierAt: tierConfig.max < 1000 ? tierConfig.max + 1 : null
  if (cfg && cfg->max < 1000) {
    out["nextTierAt"] = cfg->max + 1;
    // progressToNext: Math.round(((score - min) / (max - min)) * 100)
    double pct = (static_cast<double>(score - cfg->min) /
                  static_cast<double>(cfg->max - cfg->min)) * 100.0;
    out["progressToNext"] = static_cast<Json::Int64>(jsRound(pct));
  } else {
    out["nextTierAt"] = Json::Value::nullSingleton();
    out["progressToNext"] = 100;  // : 100
  }

  return out;
}

} // namespace pulse::models::pulsescore
