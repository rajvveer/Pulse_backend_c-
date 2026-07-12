// userengagement.cc — C++ port of src/models/UserEngagement.js.
// See include/pulse/models/userengagement.hpp.
//
// Preserves the exact filters, update operators, sorts, limits, and the affinity
// recalculation/decay math from the Mongoose statics + instance method.
// Collection name and field names match the JS schema 1:1.
#include "pulse/models/userengagement.hpp"

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
#include <mongocxx/options/index.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/find_one_and_update.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <utility>

namespace pulse::models::userengagement {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;
using pulse::bsonjson::oid;
using pulse::bsonjson::oidToHex;
using pulse::bsonjson::tryOid;
using pulse::bsonjson::nowIso8601;
using pulse::bsonjson::nowMillis;

namespace {

// (signalName, weight) pairs in the exact order of the JS SIGNAL_WEIGHTS object.
// Object.entries iteration order is insertion order, which the recalculation
// relies on only for summation (commutative), but we keep it identical anyway.
constexpr std::array<std::pair<const char*, double>, 9> kSignalWeights = {{
    {"views",                 kWeightViews},
    {"likes",                 kWeightLikes},
    {"comments",              kWeightComments},
    {"shares",                kWeightShares},
    {"totalWatchTimeSeconds", kWeightTotalWatchTimeSeconds},
    {"profileVisits",         kWeightProfileVisits},
    {"dmsSent",               kWeightDmsSent},
    {"hides",                 kWeightHides},
    {"reports",               kWeightReports},
}};

// A bsoncxx b_date for the given epoch millis.
bsoncxx::types::b_date dateFromMillis(long long millis) {
  return bsoncxx::types::b_date{std::chrono::milliseconds{millis}};
}

// Read a numeric BSON element as double (int32/int64/double), defaulting to 0.
double numToDouble(const bsoncxx::document::element& el) {
  if (!el) return 0.0;
  switch (el.type()) {
    case bsoncxx::type::k_int32:  return static_cast<double>(el.get_int32().value);
    case bsoncxx::type::k_int64:  return static_cast<double>(el.get_int64().value);
    case bsoncxx::type::k_double: return el.get_double().value;
    default:                      return 0.0;
  }
}

// Extract `lastInteraction` (a BSON Date) from a document view as epoch millis.
// Falls back to `now` if the field is missing/not a date (matches JS where
// lastInteraction always has a default of Date.now).
long long lastInteractionMillis(const bsoncxx::document::view& view, long long now) {
  auto el = view["lastInteraction"];
  if (el && el.type() == bsoncxx::type::k_date) {
    return el.get_date().to_int64();
  }
  return now;
}

// affinityScore math shared by recalculateAffinity (instance) and the helpers
// below that operate over a BSON `signals` sub-document.
double computeAffinity(double rawScore,
                       long long lastInteractionMillis_,
                       long long nowMillis_) {
  // daysSinceInteraction = (Date.now() - this.lastInteraction) / msPerDay
  const double daysSinceInteraction =
      static_cast<double>(nowMillis_ - lastInteractionMillis_) /
      (1000.0 * 60.0 * 60.0 * 24.0);
  // decayFactor = Math.pow(0.5, daysSinceInteraction / DECAY_HALF_LIFE_DAYS)
  const double decayFactor =
      std::pow(0.5, daysSinceInteraction / kDecayHalfLifeDays);
  // this.affinityScore = Math.max(0, score * decayFactor)
  return std::max(0.0, rawScore * decayFactor);
}

// Sum signal*weight over a BSON `signals` sub-document (this.signals[signal] || 0).
double rawScoreFromSignals(const bsoncxx::document::view& signals) {
  double score = 0.0;
  for (const auto& [name, weight] : kSignalWeights) {
    score += numToDouble(signals[name]) * weight;
  }
  return score;
}

// Recompute and persist affinityScore for the document at `id`, mirroring
// `engagement.recalculateAffinity(); await engagement.save();`. Returns true if
// a document was updated.
bool recalcAndSave(mongocxx::collection& col, const bsoncxx::oid& id,
                   const bsoncxx::document::view& doc, long long now) {
  double rawScore = 0.0;
  auto signalsEl = doc["signals"];
  if (signalsEl && signalsEl.type() == bsoncxx::type::k_document) {
    rawScore = rawScoreFromSignals(signalsEl.get_document().value);
  }
  const long long li = lastInteractionMillis(doc, now);
  const double affinity = computeAffinity(rawScore, li, now);

  auto result = col.update_one(
      make_document(kvp("_id", id)),
      make_document(kvp("$set", make_document(kvp("affinityScore", affinity)))));
  return result && result->modified_count() > 0;
}

} // namespace

// =========================================================
//  INDEXES
// =========================================================
void ensureIndexes() {
  auto col = pulse::db::collection(kCollection);

  // Field-level `index: true` declarations.
  // user:           { index: true }
  col.create_index(make_document(kvp("user", 1)));
  // targetUser:     { index: true }
  col.create_index(make_document(kvp("targetUser", 1)));
  // affinityScore:  { index: true }
  col.create_index(make_document(kvp("affinityScore", 1)));
  // lastInteraction:{ index: true }
  col.create_index(make_document(kvp("lastInteraction", 1)));

  // Primary lookup: user's engagement with target (unique).
  // userEngagementSchema.index({ user: 1, targetUser: 1 }, { unique: true });
  {
    mongocxx::options::index opts{};
    opts.unique(true);
    col.create_index(make_document(kvp("user", 1), kvp("targetUser", 1)), opts);
  }

  // Get top affinities for a user (for personalization).
  // userEngagementSchema.index({ user: 1, affinityScore: -1 });
  col.create_index(make_document(kvp("user", 1), kvp("affinityScore", -1)));

  // Cleanup stale engagements.
  // userEngagementSchema.index({ lastInteraction: 1 });
  col.create_index(make_document(kvp("lastInteraction", 1)));

  pulse::log::info("Ensured indexes for collection '{}'", kCollection);
}

// =========================================================
//  DEFAULTS / OUTPUT SHAPING
// =========================================================
Json::Value applyDefaults(Json::Value doc) {
  if (!doc.isObject()) doc = Json::Value(Json::objectValue);

  // signals: embedded object, every field Number default 0.
  if (!doc.isMember("signals") || !doc["signals"].isObject()) {
    doc["signals"] = Json::Value(Json::objectValue);
  }
  Json::Value& signals = doc["signals"];
  // Content interactions.
  if (!signals.isMember("views"))                 signals["views"] = 0;
  if (!signals.isMember("likes"))                 signals["likes"] = 0;
  if (!signals.isMember("comments"))              signals["comments"] = 0;
  if (!signals.isMember("shares"))                signals["shares"] = 0;
  // Time-based engagement.
  if (!signals.isMember("totalWatchTimeSeconds")) signals["totalWatchTimeSeconds"] = 0;
  if (!signals.isMember("avgWatchPercentage"))    signals["avgWatchPercentage"] = 0;
  // Social interactions.
  if (!signals.isMember("profileVisits"))         signals["profileVisits"] = 0;
  if (!signals.isMember("dmsSent"))               signals["dmsSent"] = 0;
  // Negative signals.
  if (!signals.isMember("hides"))                 signals["hides"] = 0;
  if (!signals.isMember("reports"))               signals["reports"] = 0;

  // affinityScore: { default: 0 }
  if (!doc.isMember("affinityScore")) doc["affinityScore"] = 0;

  // lastInteraction: { default: Date.now }
  const std::string now = nowIso8601();
  if (!doc.isMember("lastInteraction")) doc["lastInteraction"] = now;

  // timestamps: true -> createdAt / updatedAt
  if (!doc.isMember("createdAt")) doc["createdAt"] = now;
  if (!doc.isMember("updatedAt")) doc["updatedAt"] = now;

  // Mongoose version key.
  if (!doc.isMember("__v")) doc["__v"] = 0;

  return doc;
}

Json::Value sanitizeForOutput(Json::Value doc) {
  if (!doc.isObject()) return doc;
  // The UserEngagement schema has no select:false / sensitive fields and no
  // custom toJSON transform. Strip the Mongoose version key to match the
  // lean/toJSON output shape used across the API.
  doc.removeMember("__v");
  return doc;
}

// =========================================================
//  INSTANCE METHODS
// =========================================================
double recalculateAffinity(const Json::Value& signals,
                           long long lastInteractionMillis_,
                           long long nowMillis_) {
  // score = sum over SIGNAL_WEIGHTS of (this.signals[signal] || 0) * weight.
  double score = 0.0;
  for (const auto& [name, weight] : kSignalWeights) {
    double value = 0.0;
    if (signals.isObject() && signals.isMember(name)) {
      const Json::Value& v = signals[name];
      if (v.isNumeric()) value = v.asDouble();
    }
    score += value * weight;
  }
  return computeAffinity(score, lastInteractionMillis_, nowMillis_);
}

// =========================================================
//  STATIC METHODS
// =========================================================
std::optional<Json::Value> recordSignal(const std::string& userId,
                                        const std::string& targetUserId,
                                        const std::string& signalType,
                                        double value) {
  // if (userId.toString() === targetUserId.toString()) return null;
  if (userId == targetUserId) return std::nullopt;

  auto col = pulse::db::collection(kCollection);
  const bsoncxx::oid userOid = oid(userId);
  const bsoncxx::oid targetOid = oid(targetUserId);
  const long long now = nowMillis();

  // update = {
  //   $inc: { [`signals.${signalType}`]: value },
  //   $set: { lastInteraction: new Date() }
  // }
  const std::string incPath = std::string("signals.") + signalType;
  auto update = make_document(
      kvp("$inc", make_document(kvp(incPath, value))),
      kvp("$set", make_document(kvp("lastInteraction", dateFromMillis(now)))));

  // findOneAndUpdate({ user, targetUser }, update, { upsert: true, new: true })
  mongocxx::options::find_one_and_update opts{};
  opts.upsert(true);
  opts.return_document(mongocxx::options::return_document::k_after);

  auto result = col.find_one_and_update(
      make_document(kvp("user", userOid), kvp("targetUser", targetOid)),
      update.view(), opts);

  if (!result) return std::nullopt;
  const bsoncxx::document::view view = result->view();

  // engagement.recalculateAffinity(); await engagement.save();
  // Recompute from the persisted signals + lastInteraction and store it.
  double rawScore = 0.0;
  auto signalsEl = view["signals"];
  if (signalsEl && signalsEl.type() == bsoncxx::type::k_document) {
    rawScore = rawScoreFromSignals(signalsEl.get_document().value);
  }
  const long long li = lastInteractionMillis(view, now);
  const double affinity = computeAffinity(rawScore, li, now);

  auto idEl = view["_id"];
  if (idEl && idEl.type() == bsoncxx::type::k_oid) {
    col.update_one(
        make_document(kvp("_id", idEl.get_oid().value)),
        make_document(kvp("$set", make_document(kvp("affinityScore", affinity)))));
  }

  // Return the document as it now stands (with the recalculated affinityScore).
  Json::Value out = pulse::bsonjson::toJson(view);
  out["affinityScore"] = affinity;
  return out;
}

double getAffinity(const std::string& userId, const std::string& targetUserId) {
  auto col = pulse::db::collection(kCollection);

  // findOne({ user, targetUser })
  auto result = col.find_one(make_document(
      kvp("user", oid(userId)),
      kvp("targetUser", oid(targetUserId))));

  // engagement ? engagement.affinityScore : 0
  if (!result) return 0.0;
  return numToDouble(result->view()["affinityScore"]);
}

std::map<std::string, double> getTopAffinities(const std::string& userId,
                                               long long limit) {
  auto col = pulse::db::collection(kCollection);

  // find({ user }).sort({ affinityScore: -1 }).limit(limit)
  //   .select('targetUser affinityScore').lean()
  mongocxx::options::find opts{};
  opts.sort(make_document(kvp("affinityScore", -1)));
  opts.limit(limit);
  opts.projection(make_document(kvp("targetUser", 1), kvp("affinityScore", 1)));

  std::map<std::string, double> affinityMap;
  auto cursor = col.find(make_document(kvp("user", oid(userId))), opts);
  for (auto&& d : cursor) {
    auto idEl = d["targetUser"];
    if (idEl && idEl.type() == bsoncxx::type::k_oid) {
      // affinityMap.set(e.targetUser.toString(), e.affinityScore)
      affinityMap[oidToHex(idEl.get_oid().value)] = numToDouble(d["affinityScore"]);
    }
  }
  return affinityMap;
}

std::map<std::string, double> getBatchAffinities(
    const std::string& userId,
    const std::vector<std::string>& targetUserIds) {
  auto col = pulse::db::collection(kCollection);

  // targetUser: { $in: targetUserIds }
  auto idArray = bld::array{};
  for (const auto& id : targetUserIds) {
    auto maybe = tryOid(id);
    if (maybe) idArray.append(*maybe);
    else       idArray.append(id);
  }

  // find({ user, targetUser: { $in: targetUserIds } })
  //   .select('targetUser affinityScore').lean()
  mongocxx::options::find opts{};
  opts.projection(make_document(kvp("targetUser", 1), kvp("affinityScore", 1)));

  std::map<std::string, double> affinityMap;
  auto cursor = col.find(make_document(
      kvp("user", oid(userId)),
      kvp("targetUser", make_document(kvp("$in", idArray)))), opts);
  for (auto&& d : cursor) {
    auto idEl = d["targetUser"];
    if (idEl && idEl.type() == bsoncxx::type::k_oid) {
      affinityMap[oidToHex(idEl.get_oid().value)] = numToDouble(d["affinityScore"]);
    }
  }

  // Fill zeros for unknown users (targetUserIds.forEach ... if (!has) set 0).
  for (const auto& id : targetUserIds) {
    if (affinityMap.find(id) == affinityMap.end()) affinityMap[id] = 0.0;
  }
  return affinityMap;
}

long long applyGlobalDecay() {
  auto col = pulse::db::collection(kCollection);
  const long long now = nowMillis();

  // staleThreshold = new Date(Date.now() - 30 * 24 * 60 * 60 * 1000); // 30 days
  const long long staleThreshold = now - 30LL * 24 * 60 * 60 * 1000;

  // deleteMany({ lastInteraction: { $lt: staleThreshold }, affinityScore: { $lt: 1 } })
  col.delete_many(make_document(
      kvp("lastInteraction", make_document(kvp("$lt", dateFromMillis(staleThreshold)))),
      kvp("affinityScore", make_document(kvp("$lt", 1)))));

  // Recalculate remaining — stream with a cursor instead of loading the entire
  // collection into memory: find({}).cursor(); for each recalc + save.
  long long processed = 0;
  auto cursor = col.find(make_document());
  for (auto&& d : cursor) {
    auto idEl = d["_id"];
    if (idEl && idEl.type() == bsoncxx::type::k_oid) {
      // engagement.recalculateAffinity(); await engagement.save();
      recalcAndSave(col, idEl.get_oid().value, d, now);
      ++processed;
    }
  }
  return processed;
}

} // namespace pulse::models::userengagement
