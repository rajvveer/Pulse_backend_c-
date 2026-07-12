// userbehavior.cc — C++ port of src/models/UserBehavior.js.
// See userbehavior.hpp for the schema -> free-function map.
#include "pulse/models/userbehavior.hpp"

#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/oid.hpp>

#include <mongocxx/collection.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/index.hpp>
#include <mongocxx/options/replace.hpp>
#include <mongocxx/exception/exception.hpp>

#include <chrono>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <ctime>

namespace pulse::models::userbehavior {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;
using bld::make_array;

namespace {

// "now" in epoch milliseconds (matches Date.now() / `new Date()` arithmetic).
long long nowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch()).count();
}

bsoncxx::types::b_date dateFromMillis(long long ms) {
  return bsoncxx::types::b_date{std::chrono::milliseconds{ms}};
}

std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

// A JSON value read as a double, tolerating int/uint/real/null (JS `|| 0`).
double asNumber(const Json::Value& v, double fallback = 0.0) {
  if (v.isNumeric()) return v.asDouble();
  return fallback;
}

// Parse an ISO-8601 timestamp ("YYYY-MM-DDTHH:MM:SS(.fff)(Z)") or "YYYY-MM-DD"
// into epoch millis. Returns nullopt on an unparseable value. Used to compare
// stored seenAt/lastActivityAt strings against time cutoffs.
std::optional<long long> parseDateMillis(const std::string& s) {
  if (s.empty()) return std::nullopt;
  int year = 0, mon = 0, day = 0, hour = 0, min = 0, sec = 0;
  int matched = std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d",
                            &year, &mon, &day, &hour, &min, &sec);
  if (matched < 3) return std::nullopt;
  std::tm tm{};
  tm.tm_year = year - 1900;
  tm.tm_mon  = mon - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min  = min;
  tm.tm_sec  = sec;
#if defined(_WIN32)
  std::time_t t = _mkgmtime(&tm);
#else
  std::time_t t = timegm(&tm);
#endif
  if (t == static_cast<std::time_t>(-1)) return std::nullopt;
  long long ms = static_cast<long long>(t) * 1000;
  auto dot = s.find('.');
  if (dot != std::string::npos && dot + 1 < s.size()) {
    std::string frac;
    for (size_t i = dot + 1; i < s.size() && std::isdigit((unsigned char)s[i]); ++i)
      frac.push_back(s[i]);
    while (frac.size() < 3) frac.push_back('0');
    frac = frac.substr(0, 3);
    ms += std::stoll(frac);
  }
  return ms;
}

// Read a JSON timestamp field as epoch millis (accepts an ISO string or a raw
// numeric epoch-millis value). nullopt when absent/unset.
std::optional<long long> jsonDateMillis(const Json::Value& v) {
  if (v.isNumeric()) return static_cast<long long>(v.asDouble());
  if (v.isString())  return parseDateMillis(v.asString());
  return std::nullopt;
}

// Ensure obj[key] is an object, returning a reference to it.
Json::Value& ensureObject(Json::Value& obj, const char* key) {
  if (!obj.isMember(key) || !obj[key].isObject())
    obj[key] = Json::Value(Json::objectValue);
  return obj[key];
}

// ── BSON persistence ────────────────────────────────────────────────────────
// Mongoose `.save()` writes the full document. We persist the mutated JSON via
// replace_one(upsert), but a handful of fields must round-trip as their native
// BSON types (not JSON strings) so indexes/queries behave: `user` and each
// recentlySeenPosts[].postId as ObjectId, and the Date fields as BSON dates.

// Date-like field names that should serialize as BSON dates when their string
// parses as ISO-8601 (mirrors how Mongoose stores these Date paths).
bool isDateKey(const std::string& key) {
  return key == "seenAt" || key == "startedAt" || key == "lastActivityAt" ||
         key == "lastProfileUpdate" || key == "createdAt" || key == "updatedAt";
}

// Templated over the builder type so the SAME logic serves the top-level
// bld::document and the lambda-callback bld::sub_document / bld::sub_array.
template <typename ArrBuilder>
void appendJsonArray(ArrBuilder& arr, const Json::Value& v);

template <typename DocBuilder>
void appendJsonValue(DocBuilder& doc, const std::string& key, const Json::Value& v) {
  const std::string& k = key;

  // `user` -> ObjectId when it is a 24-hex string.
  if (k == "user" && v.isString()) {
    if (auto o = pulse::bsonjson::tryOid(v.asString())) { doc.append(kvp(key, *o)); return; }
  }
  // recentlySeenPosts[].postId -> ObjectId when it is a 24-hex string.
  if (k == "postId" && v.isString()) {
    if (auto o = pulse::bsonjson::tryOid(v.asString())) { doc.append(kvp(key, *o)); return; }
  }
  // Date paths -> BSON date when the string is ISO-8601.
  if (isDateKey(k)) {
    if (auto ms = jsonDateMillis(v)) { doc.append(kvp(key, dateFromMillis(*ms))); return; }
  }

  switch (v.type()) {
    case Json::nullValue:    doc.append(kvp(key, bsoncxx::types::b_null{})); break;
    case Json::intValue:     doc.append(kvp(key, static_cast<std::int64_t>(v.asInt64()))); break;
    case Json::uintValue:    doc.append(kvp(key, static_cast<std::int64_t>(v.asUInt64()))); break;
    case Json::realValue:    doc.append(kvp(key, v.asDouble())); break;
    case Json::booleanValue: doc.append(kvp(key, v.asBool())); break;
    case Json::stringValue:  doc.append(kvp(key, v.asString())); break;
    case Json::arrayValue:
      doc.append(kvp(key, [&](bld::sub_array sub) { appendJsonArray(sub, v); }));
      break;
    case Json::objectValue:
      doc.append(kvp(key, [&](bld::sub_document sub) {
        for (const auto& m : v.getMemberNames()) appendJsonValue(sub, m, v[m]);
      }));
      break;
  }
}

template <typename ArrBuilder>
void appendJsonArray(ArrBuilder& arr, const Json::Value& v) {
  for (const auto& el : v) {
    switch (el.type()) {
      case Json::nullValue:    arr.append(bsoncxx::types::b_null{}); break;
      case Json::intValue:     arr.append(static_cast<std::int64_t>(el.asInt64())); break;
      case Json::uintValue:    arr.append(static_cast<std::int64_t>(el.asUInt64())); break;
      case Json::realValue:    arr.append(el.asDouble()); break;
      case Json::booleanValue: arr.append(el.asBool()); break;
      case Json::stringValue:  arr.append(el.asString()); break;
      case Json::arrayValue:
        arr.append([&](bld::sub_array sub) { appendJsonArray(sub, el); });
        break;
      case Json::objectValue:
        arr.append([&](bld::sub_document sub) {
          for (const auto& m : el.getMemberNames()) appendJsonValue(sub, m.c_str(), el[m]);
        });
        break;
    }
  }
}

// Build a BSON document from the behavior JSON, minus _id (which is the replace
// filter key, not a settable field).
bsoncxx::document::value buildBehaviorDoc(const Json::Value& behavior) {
  bld::document doc;
  for (const auto& m : behavior.getMemberNames()) {
    if (m == "_id") continue;
    appendJsonValue(doc, m, behavior[m]);
  }
  return doc.extract();
}

// Persist `behavior` to Mongo (full-document save), keyed by `user`.
// updatedAt is refreshed to "now" (Mongoose timestamps on save).
void saveBehavior(Json::Value& behavior) {
  behavior["updatedAt"] = pulse::bsonjson::nowIso8601();
  try {
    auto col = pulse::db::collection(kCollection);
    std::optional<bsoncxx::oid> userOid;
    if (behavior.isMember("user") && behavior["user"].isString())
      userOid = pulse::bsonjson::tryOid(behavior["user"].asString());
    auto bson = buildBehaviorDoc(behavior);
    mongocxx::options::replace ropts{};
    ropts.upsert(true);
    if (userOid) {
      col.replace_one(make_document(kvp("user", *userOid)).view(), bson.view(), ropts);
    } else if (behavior.isMember("user")) {
      col.replace_one(
          make_document(kvp("user", behavior["user"].asString())).view(), bson.view(), ropts);
    }
  } catch (const std::exception& e) {
    pulse::log::error("UserBehavior save failed: {}", e.what());
  }
}

// findOne({ user: userId }) -> JSON (sanitized) or nullopt.
std::optional<Json::Value> findByUser(const std::string& userId) {
  try {
    auto col = pulse::db::collection(kCollection);
    auto o = pulse::bsonjson::tryOid(userId);
    auto res = o ? col.find_one(make_document(kvp("user", *o)).view())
                 : col.find_one(make_document(kvp("user", userId)).view());
    if (!res) return std::nullopt;
    return sanitizeForOutput(pulse::bsonjson::toJson(res->view()));
  } catch (const std::exception& e) {
    pulse::log::error("UserBehavior findByUser failed: {}", e.what());
    return std::nullopt;
  }
}

} // namespace

// ── Index management ────────────────────────────────────────────────────────
void ensureIndexes() {
  try {
    auto col = pulse::db::collection(kCollection);

    // Field-level: user: { unique: true, index: true } -> unique index { user: 1 }.
    {
      mongocxx::options::index opts{};
      opts.unique(true);
      col.create_index(make_document(kvp("user", 1)), opts);
    }

    // userBehaviorSchema.index({ user: 1 })  (duplicate of the field-level index;
    // mongocxx treats it as a no-op against the existing { user: 1 } index).
    col.create_index(make_document(kvp("user", 1)));

    // userBehaviorSchema.index({ 'currentSession.lastActivityAt': 1 })
    col.create_index(make_document(kvp("currentSession.lastActivityAt", 1)));

    pulse::log::info("UserBehavior indexes ensured ({})", kCollection);
  } catch (const std::exception& e) {
    pulse::log::error("UserBehavior ensureIndexes failed: {}", e.what());
  }
}

// ── Insert defaults + output sanitization ───────────────────────────────────
Json::Value applyDefaults(Json::Value doc) {
  if (!doc.isObject()) doc = Json::Value(Json::objectValue);

  // contentAffinities ----------------------------------------------------------
  Json::Value& ca = ensureObject(doc, "contentAffinities");
  // topics: Map default new Map() -> {}
  if (!ca.isMember("topics") || !ca["topics"].isObject())
    ca["topics"] = Json::Value(Json::objectValue);
  // mediaTypes: video/image/text/gif default 0.5
  Json::Value& mt = ensureObject(ca, "mediaTypes");
  if (!mt.isMember("video")) mt["video"] = 0.5;
  if (!mt.isMember("image")) mt["image"] = 0.5;
  if (!mt.isMember("text"))  mt["text"]  = 0.5;
  if (!mt.isMember("gif"))   mt["gif"]   = 0.5;
  // postLengths: short/medium/long default 0.5
  Json::Value& pl = ensureObject(ca, "postLengths");
  if (!pl.isMember("short"))  pl["short"]  = 0.5;
  if (!pl.isMember("medium")) pl["medium"] = 0.5;
  if (!pl.isMember("long"))   pl["long"]   = 0.5;
  // authorCategories: Map default new Map() -> {}
  if (!ca.isMember("authorCategories") || !ca["authorCategories"].isObject())
    ca["authorCategories"] = Json::Value(Json::objectValue);

  // sessionPatterns ------------------------------------------------------------
  Json::Value& sp = ensureObject(doc, "sessionPatterns");
  if (!sp.isMember("avgSessionDurationMs")) sp["avgSessionDurationMs"] = 0;
  if (!sp.isMember("totalSessions"))        sp["totalSessions"]        = 0;
  // peakHours: array of { hour, weight(default 1) } — default empty array.
  if (!sp.isMember("peakHours") || !sp["peakHours"].isArray())
    sp["peakHours"] = Json::Value(Json::arrayValue);
  if (!sp.isMember("avgScrollVelocity"))  sp["avgScrollVelocity"]  = 0;
  if (!sp.isMember("avgDwellTimeMs"))     sp["avgDwellTimeMs"]     = 0;
  if (!sp.isMember("dwellTimeToLikeMs"))  sp["dwellTimeToLikeMs"]  = 2000;
  if (!sp.isMember("postsPerSession"))    sp["postsPerSession"]    = 0;

  // engagementVelocity ---------------------------------------------------------
  Json::Value& ev = ensureObject(doc, "engagementVelocity");
  if (!ev.isMember("avgTimeToLikeMs"))    ev["avgTimeToLikeMs"]    = 0;
  if (!ev.isMember("avgTimeToCommentMs")) ev["avgTimeToCommentMs"] = 0;
  if (!ev.isMember("likeRate"))           ev["likeRate"]           = 0;
  if (!ev.isMember("commentRate"))        ev["commentRate"]        = 0;

  // rewardSensitivity ----------------------------------------------------------
  Json::Value& rs = ensureObject(doc, "rewardSensitivity");
  if (!rs.isMember("engagementSensitivity")) rs["engagementSensitivity"] = 0.5;
  if (!rs.isMember("viralPreference"))       rs["viralPreference"]       = 0.5;
  if (!rs.isMember("noveltyPreference"))     rs["noveltyPreference"]     = 0.5;
  if (!rs.isMember("socialProofInfluence"))  rs["socialProofInfluence"]  = 0.5;

  // recentlySeenPosts: default empty array (each { postId, seenAt(default now) }).
  if (!doc.isMember("recentlySeenPosts") || !doc["recentlySeenPosts"].isArray())
    doc["recentlySeenPosts"] = Json::Value(Json::arrayValue);

  // currentSession: postsViewed/likesGiven default 0 (startedAt/lastActivityAt
  // have no default — left unset until a session begins).
  Json::Value& cs = ensureObject(doc, "currentSession");
  if (!cs.isMember("postsViewed")) cs["postsViewed"] = 0;
  if (!cs.isMember("likesGiven"))  cs["likesGiven"]  = 0;

  // lastProfileUpdate default Date.now()
  if (!doc.isMember("lastProfileUpdate"))
    doc["lastProfileUpdate"] = pulse::bsonjson::nowIso8601();

  // timestamps: { createdAt, updatedAt }
  std::string now = pulse::bsonjson::nowIso8601();
  if (!doc.isMember("createdAt")) doc["createdAt"] = now;
  if (!doc.isMember("updatedAt")) doc["updatedAt"] = now;

  // Mongoose version key.
  if (!doc.isMember("__v")) doc["__v"] = 0;

  return doc;
}

Json::Value sanitizeForOutput(Json::Value doc) {
  if (!doc.isObject()) return doc;
  // No select:false / sensitive fields on this schema; only drop the version key.
  doc.removeMember("__v");
  return doc;
}

// ── Instance methods (pure document logic) ──────────────────────────────────

// methods.getMediaType(post)
std::string getMediaType(const Json::Value& post) {
  const Json::Value& content = post.isObject() ? post["content"] : Json::Value();
  const Json::Value& media = content.isObject() ? content["media"] : Json::Value();
  if (!media.isArray() || media.size() == 0) return "text";

  bool hasVideo = false, hasGif = false, hasImage = false;
  for (const auto& m : media) {
    if (!m.isObject()) continue;
    const std::string t = m.isMember("type") && m["type"].isString() ? m["type"].asString() : "";
    if (t == "video") hasVideo = true;
    else if (t == "gif") hasGif = true;
    else if (t == "image") hasImage = true;
  }
  if (hasVideo) return "video";
  if (hasGif)   return "gif";
  if (hasImage) return "image";
  return "text";
}

// methods.updateAffinitiesFromPost(post, dwellTimeMs=0, multiplier=1.0)
void updateAffinitiesFromPost(Json::Value& behavior,
                              const Json::Value& post,
                              double dwellTimeMs,
                              double multiplier) {
  // if (!post) return;
  if (post.isNull() || (post.isObject() && post.empty())) return;

  // dwellWeight = dwellTimeMs > 0 ? Math.min(dwellTimeMs / 5000, 2) : 0.5
  double dwellWeight = dwellTimeMs > 0 ? std::min(dwellTimeMs / 5000.0, 2.0) : 0.5;
  double weight = dwellWeight * multiplier;

  Json::Value& ca = ensureObject(behavior, "contentAffinities");

  // Update media type affinity (only when that key already exists).
  Json::Value& mt = ensureObject(ca, "mediaTypes");
  const std::string mediaType = getMediaType(post);
  if (mt.isMember(mediaType)) {
    double cur = asNumber(mt[mediaType]);
    mt[mediaType] = std::min(1.0, cur * 0.95 + 0.05 * weight);
  }

  // Update post length affinity.
  const Json::Value& content = post.isObject() ? post["content"] : Json::Value();
  int textLength = 0;
  if (content.isObject() && content["text"].isString())
    textLength = static_cast<int>(content["text"].asString().size());
  std::string lengthKey = "short";
  if (textLength > 200) lengthKey = "long";
  else if (textLength > 50) lengthKey = "medium";

  Json::Value& pl = ensureObject(ca, "postLengths");
  double curLen = asNumber(pl[lengthKey]);
  pl[lengthKey] = std::min(1.0, curLen * 0.95 + 0.05 * weight);

  // Update topic affinities from hashtags.
  Json::Value& topics = ca["topics"];
  if (!topics.isObject()) topics = Json::Value(Json::objectValue);
  const Json::Value& hashtags = content.isObject() ? content["hashtags"] : Json::Value();
  if (hashtags.isArray()) {
    for (const auto& tag : hashtags) {
      if (!tag.isString()) continue;
      const std::string normalized = toLower(tag.asString());
      double cur = topics.isMember(normalized) ? asNumber(topics[normalized]) : 0.0;
      topics[normalized] = std::min(1.0, cur + 0.1 * weight);
    }
  }

  behavior["lastProfileUpdate"] = pulse::bsonjson::nowIso8601();
}

// ── Statics ─────────────────────────────────────────────────────────────────

// statics.getOrCreate(userId)
Json::Value getOrCreate(const std::string& userId) {
  if (auto existing = findByUser(userId)) return *existing;

  // create({ user: userId })
  Json::Value doc(Json::objectValue);
  doc["user"] = userId;
  doc = applyDefaults(doc);
  saveBehavior(doc);
  return sanitizeForOutput(doc);
}

// statics.recordView(userId, post, dwellTimeMs=0)
Json::Value recordView(const std::string& userId,
                       const Json::Value& post,
                       double dwellTimeMs) {
  Json::Value behavior = getOrCreate(userId);
  const long long nowMs = nowMillis();

  Json::Value& cs = ensureObject(behavior, "currentSession");
  Json::Value& sp = ensureObject(behavior, "sessionPatterns");

  // isNewSession = !startedAt || (now - lastActivityAt) > SESSION_TIMEOUT_MS
  std::optional<long long> startedAtMs =
      cs.isMember("startedAt") ? jsonDateMillis(cs["startedAt"]) : std::nullopt;
  std::optional<long long> lastActivityMs =
      cs.isMember("lastActivityAt") ? jsonDateMillis(cs["lastActivityAt"]) : std::nullopt;

  bool isNewSession = !startedAtMs.has_value() ||
                      (nowMs - lastActivityMs.value_or(0)) > kSessionTimeoutMs;

  if (isNewSession) {
    // Save previous session stats when it had views.
    int prevPostsViewed = cs.isMember("postsViewed") ? cs["postsViewed"].asInt() : 0;
    if (prevPostsViewed > 0 && startedAtMs && lastActivityMs) {
      double sessionDuration = static_cast<double>(*lastActivityMs - *startedAtMs);
      double totalSessions = asNumber(sp["totalSessions"]) + 1;
      sp["totalSessions"] = static_cast<int>(totalSessions);
      double prevAvgDur = asNumber(sp["avgSessionDurationMs"]);
      sp["avgSessionDurationMs"] =
          (prevAvgDur * (totalSessions - 1) + sessionDuration) / totalSessions;
      double prevPPS = asNumber(sp["postsPerSession"]);
      sp["postsPerSession"] =
          (prevPPS * (totalSessions - 1) + prevPostsViewed) / totalSessions;
    }

    // Start new session.
    Json::Value session(Json::objectValue);
    session["startedAt"] = pulse::bsonjson::nowIso8601();
    session["postsViewed"] = 0;
    session["likesGiven"] = 0;
    session["lastActivityAt"] = pulse::bsonjson::nowIso8601();
    behavior["currentSession"] = session;
  }

  Json::Value& cur = behavior["currentSession"];
  // postsViewed += 1; lastActivityAt = now
  cur["postsViewed"] = (cur.isMember("postsViewed") ? cur["postsViewed"].asInt() : 0) + 1;
  cur["lastActivityAt"] = pulse::bsonjson::nowIso8601();

  // Track dwell time (EMA): avgDwellTimeMs = prevAvg * 0.9 + dwellTimeMs * 0.1
  if (dwellTimeMs > 0) {
    double prevAvg = asNumber(sp["avgDwellTimeMs"]);
    sp["avgDwellTimeMs"] = prevAvg * 0.9 + dwellTimeMs * 0.1;
  }

  // Track seen post: push { postId: post._id || post, seenAt: now }.
  Json::Value entry(Json::objectValue);
  std::string postId;
  if (post.isObject() && post.isMember("_id")) {
    const Json::Value& idv = post["_id"];
    postId = idv.isString() ? idv.asString()
             : (idv.isObject() && idv.isMember("_id") ? idv["_id"].asString() : "");
  } else if (post.isString()) {
    postId = post.asString();
  }
  entry["postId"] = postId;
  entry["seenAt"] = pulse::bsonjson::nowIso8601();

  if (!behavior.isMember("recentlySeenPosts") || !behavior["recentlySeenPosts"].isArray())
    behavior["recentlySeenPosts"] = Json::Value(Json::arrayValue);
  behavior["recentlySeenPosts"].append(entry);

  // Cap to last MAX_RECENT_POSTS (slice(-MAX_RECENT_POSTS)).
  Json::Value& seen = behavior["recentlySeenPosts"];
  if (static_cast<int>(seen.size()) > kMaxRecentPosts) {
    Json::Value trimmed(Json::arrayValue);
    int start = static_cast<int>(seen.size()) - kMaxRecentPosts;
    for (int i = start; i < static_cast<int>(seen.size()); ++i) trimmed.append(seen[i]);
    behavior["recentlySeenPosts"] = trimmed;
  }

  // Update content affinities from the viewed post.
  updateAffinitiesFromPost(behavior, post, dwellTimeMs, 1.0);

  saveBehavior(behavior);
  return sanitizeForOutput(behavior);
}

// statics.recordLike(userId, post, timeSinceViewMs=0)
Json::Value recordLike(const std::string& userId,
                       const Json::Value& post,
                       double timeSinceViewMs) {
  Json::Value behavior = getOrCreate(userId);

  Json::Value& cs = ensureObject(behavior, "currentSession");
  Json::Value& ev = ensureObject(behavior, "engagementVelocity");

  // currentSession.likesGiven += 1
  cs["likesGiven"] = (cs.isMember("likesGiven") ? cs["likesGiven"].asInt() : 0) + 1;

  // EMA avgTimeToLikeMs = prevAvg * 0.8 + timeSinceViewMs * 0.2
  if (timeSinceViewMs > 0) {
    double prevAvg = asNumber(ev["avgTimeToLikeMs"]);
    ev["avgTimeToLikeMs"] = prevAvg * 0.8 + timeSinceViewMs * 0.2;
  }

  // likeRate = likesGiven / postsViewed  (when postsViewed > 0)
  int postsViewed = cs.isMember("postsViewed") ? cs["postsViewed"].asInt() : 0;
  if (postsViewed > 0) {
    int likesGiven = cs["likesGiven"].asInt();
    ev["likeRate"] = static_cast<double>(likesGiven) / static_cast<double>(postsViewed);
  }

  // Boost affinities for liked content (2x weight, dwell = 0).
  updateAffinitiesFromPost(behavior, post, 0.0, 2.0);

  saveBehavior(behavior);
  return sanitizeForOutput(behavior);
}

// statics.getSeenPostIds(userId, withinHours=24)
std::set<std::string> getSeenPostIds(const std::string& userId, double withinHours) {
  std::set<std::string> seenIds;
  auto behavior = findByUser(userId);
  if (!behavior) return seenIds;  // new Set()

  // cutoff = new Date(Date.now() - withinHours * 60 * 60 * 1000)
  long long cutoffMs = nowMillis() - static_cast<long long>(withinHours * 60.0 * 60.0 * 1000.0);

  const Json::Value& posts = (*behavior)["recentlySeenPosts"];
  if (!posts.isArray()) return seenIds;
  for (const auto& p : posts) {
    if (!p.isObject()) continue;
    auto seenAtMs = p.isMember("seenAt") ? jsonDateMillis(p["seenAt"]) : std::nullopt;
    if (!seenAtMs || *seenAtMs < cutoffMs) continue;  // filter(p.seenAt >= cutoff)
    if (p.isMember("postId")) {
      const Json::Value& idv = p["postId"];
      if (idv.isString()) seenIds.insert(idv.asString());
      else if (idv.isObject() && idv.isMember("_id")) seenIds.insert(idv["_id"].asString());
    }
  }
  return seenIds;
}

// statics.getPreferences(userId)
Json::Value getPreferences(const std::string& userId) {
  auto behavior = findByUser(userId);

  if (!behavior) {
    // Default preferences object (returned verbatim from the JS).
    Json::Value def(Json::objectValue);
    Json::Value media(Json::objectValue);
    media["video"] = 0.5; media["image"] = 0.5; media["text"] = 0.5; media["gif"] = 0.5;
    def["mediaTypes"] = media;
    def["topics"] = Json::Value(Json::objectValue);  // new Map() -> {}
    def["noveltyPreference"] = 0.5;
    def["viralPreference"] = 0.5;
    def["socialProofInfluence"] = 0.5;
    def["sessionDepth"] = 0;
    return def;
  }

  const Json::Value& b = *behavior;
  const Json::Value& ca = b["contentAffinities"];
  const Json::Value& rs = b["rewardSensitivity"];
  const Json::Value& cs = b["currentSession"];

  Json::Value out(Json::objectValue);
  // mediaTypes: contentAffinities?.mediaTypes || {}
  out["mediaTypes"] = (ca.isObject() && ca.isMember("mediaTypes"))
                          ? ca["mediaTypes"] : Json::Value(Json::objectValue);
  // topics: contentAffinities?.topics || new Map()
  out["topics"] = (ca.isObject() && ca.isMember("topics"))
                      ? ca["topics"] : Json::Value(Json::objectValue);
  // noveltyPreference: rewardSensitivity?.noveltyPreference || 0.5
  out["noveltyPreference"] = (rs.isObject() && rs["noveltyPreference"].isNumeric())
                                 ? rs["noveltyPreference"] : Json::Value(0.5);
  out["viralPreference"] = (rs.isObject() && rs["viralPreference"].isNumeric())
                               ? rs["viralPreference"] : Json::Value(0.5);
  out["socialProofInfluence"] = (rs.isObject() && rs["socialProofInfluence"].isNumeric())
                                    ? rs["socialProofInfluence"] : Json::Value(0.5);
  // sessionDepth: currentSession?.postsViewed || 0
  out["sessionDepth"] = (cs.isObject() && cs["postsViewed"].isNumeric())
                            ? cs["postsViewed"] : Json::Value(0);
  return out;
}

} // namespace pulse::models::userbehavior
