// session.cc — C++ port of src/models/Session.js. See session.hpp for the map
// from Mongoose schema/statics/methods to these free functions.
#include "pulse/models/session.hpp"

#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/options/index.hpp>
#include <mongocxx/exception/exception.hpp>

#include <chrono>

namespace pulse::models::session {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;

namespace {

// Current time as a BSON date (UTC). Mirrors `new Date()` in the JS source.
bsoncxx::types::b_date now() {
  return bsoncxx::types::b_date{std::chrono::system_clock::now()};
}

// `new Date(Date.now() - 7 * 24 * 60 * 60 * 1000)` — the 7-day cutoff used by
// cleanupExpired() for stale inactive sessions.
bsoncxx::types::b_date sevenDaysAgo() {
  using namespace std::chrono;
  auto cutoff = system_clock::now() - hours(7 * 24);
  return bsoncxx::types::b_date{cutoff};
}

} // namespace

bool isValidPlatform(const std::string& platform) {
  return platform == "ios" || platform == "android" ||
         platform == "web" || platform == "desktop";
}

// -----------------------------------------------------------------------------
// Indexes
// -----------------------------------------------------------------------------
void ensureIndexes() {
  try {
    auto col = pulse::db::collection(kCollection);

    // expiresAt TTL index (schema field option `expires: 0`): Mongo deletes the
    // document once expiresAt is reached (expireAfterSeconds = 0).
    {
      mongocxx::options::index opts{};
      opts.expire_after(std::chrono::seconds(0));
      col.create_index(make_document(kvp("expiresAt", 1)), opts);
    }

    // sessionSchema.index({ userId: 1, isActive: 1 })
    col.create_index(make_document(kvp("userId", 1), kvp("isActive", 1)));

    // sessionSchema.index({ deviceId: 1 })
    col.create_index(make_document(kvp("deviceId", 1)));

    // sessionSchema.index({ refreshToken: 1 })
    col.create_index(make_document(kvp("refreshToken", 1)));

    // sessionSchema.index({ lastActivity: -1 })
    col.create_index(make_document(kvp("lastActivity", -1)));

    pulse::log::info("Session indexes ensured ({})", kCollection);
  } catch (const std::exception& e) {
    pulse::log::error("Session ensureIndexes failed: {}", e.what());
    throw;
  }
}

// -----------------------------------------------------------------------------
// Defaults + sanitization
// -----------------------------------------------------------------------------
Json::Value applyDefaults(Json::Value doc) {
  if (!doc.isObject()) doc = Json::Value(Json::objectValue);

  // deviceInfo subdocument defaults (platform is required, no default).
  if (!doc.isMember("deviceInfo") || !doc["deviceInfo"].isObject())
    doc["deviceInfo"] = Json::Value(Json::objectValue);
  Json::Value& di = doc["deviceInfo"];
  if (!di.isMember("deviceName")) di["deviceName"] = "Unknown Device";
  if (!di.isMember("appVersion")) di["appVersion"] = "1.0.0";
  if (!di.isMember("osVersion"))  di["osVersion"]  = "Unknown";

  // firebaseToken default: null
  if (!doc.isMember("firebaseToken")) doc["firebaseToken"] = Json::Value(Json::nullValue);

  // userAgent default: ''
  if (!doc.isMember("userAgent")) doc["userAgent"] = "";

  // location.coordinates default: null (city/country have no default).
  if (doc.isMember("location") && doc["location"].isObject()) {
    Json::Value& loc = doc["location"];
    if (!loc.isMember("coordinates")) loc["coordinates"] = Json::Value(Json::nullValue);
  }

  // isActive default: true
  if (!doc.isMember("isActive")) doc["isActive"] = true;

  // lastActivity default: Date.now()
  if (!doc.isMember("lastActivity")) doc["lastActivity"] = pulse::bsonjson::nowIso8601();

  // timestamps: true -> createdAt / updatedAt
  std::string ts = pulse::bsonjson::nowIso8601();
  if (!doc.isMember("createdAt")) doc["createdAt"] = ts;
  if (!doc.isMember("updatedAt")) doc["updatedAt"] = ts;

  return doc;
}

Json::Value sanitizeForOutput(Json::Value doc) {
  if (!doc.isObject()) return doc;
  // select:false fields — never exposed unless explicitly selected in JS.
  doc.removeMember("accessToken");
  doc.removeMember("refreshToken");
  doc.removeMember("firebaseToken");
  // Mongoose version key.
  doc.removeMember("__v");
  return doc;
}

// -----------------------------------------------------------------------------
// Statics
// -----------------------------------------------------------------------------
std::optional<Json::Value> findActiveSession(const bsoncxx::oid& userId,
                                             const std::string& deviceId) {
  auto col = pulse::db::collection(kCollection);
  // findOne({ userId, deviceId, isActive: true, expiresAt: { $gt: now } })
  auto filter = make_document(
      kvp("userId", userId),
      kvp("deviceId", deviceId),
      kvp("isActive", true),
      kvp("expiresAt", make_document(kvp("$gt", now()))));

  auto result = col.find_one(filter.view());
  if (!result) return std::nullopt;
  return sanitizeForOutput(pulse::bsonjson::toJson(result->view()));
}

long long deactivateUserSessions(const bsoncxx::oid& userId,
                                 const std::optional<std::string>& excludeDeviceId) {
  auto col = pulse::db::collection(kCollection);

  // const query = { userId, isActive: true };
  // if (excludeDeviceId) query.deviceId = { $ne: excludeDeviceId };
  bld::document filter{};
  filter.append(kvp("userId", userId));
  filter.append(kvp("isActive", true));
  if (excludeDeviceId && !excludeDeviceId->empty()) {
    filter.append(kvp("deviceId", make_document(kvp("$ne", *excludeDeviceId))));
  }

  // updateMany(query, { $set: { isActive: false } })
  auto update = make_document(kvp("$set", make_document(kvp("isActive", false))));

  auto result = col.update_many(filter.view(), update.view());
  return result ? result->modified_count() : 0;
}

long long cleanupExpired() {
  auto col = pulse::db::collection(kCollection);

  // deleteMany({ $or: [
  //   { expiresAt: { $lt: now } },
  //   { isActive: false, updatedAt: { $lt: now - 7 days } } ] })
  bld::array orArr{};
  orArr.append(make_document(kvp("expiresAt", make_document(kvp("$lt", now())))));
  orArr.append(make_document(
      kvp("isActive", false),
      kvp("updatedAt", make_document(kvp("$lt", sevenDaysAgo())))));

  auto filter = make_document(kvp("$or", orArr));

  auto result = col.delete_many(filter.view());
  return result ? result->deleted_count() : 0;
}

// -----------------------------------------------------------------------------
// Instance helpers
// -----------------------------------------------------------------------------
long long updateActivity(const bsoncxx::oid& sessionId) {
  auto col = pulse::db::collection(kCollection);
  // this.lastActivity = new Date(); this.save();
  auto update = make_document(kvp("$set", make_document(kvp("lastActivity", now()))));
  auto result = col.update_one(make_document(kvp("_id", sessionId)).view(), update.view());
  return result ? result->modified_count() : 0;
}

long long deactivate(const bsoncxx::oid& sessionId) {
  auto col = pulse::db::collection(kCollection);
  // this.isActive = false; this.save();
  auto update = make_document(kvp("$set", make_document(kvp("isActive", false))));
  auto result = col.update_one(make_document(kvp("_id", sessionId)).view(), update.view());
  return result ? result->modified_count() : 0;
}

bool isExpired(long long expiresAtMillis) {
  // return this.expiresAt < new Date();
  return expiresAtMillis < pulse::bsonjson::nowMillis();
}

} // namespace pulse::models::session
