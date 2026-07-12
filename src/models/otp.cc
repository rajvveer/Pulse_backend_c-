// otp.cc — C++ port of src/models/OTP.js. See pulse/models/otp.hpp.
#include "pulse/models/otp.hpp"

#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/options/index.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/find_one_and_update.hpp>
#include <mongocxx/exception/exception.hpp>

#include <chrono>
#include <ctime>
#include <cstdio>

namespace pulse::models::otp {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;

namespace {

// A bsoncxx b_date for "now", used by the date-comparison queries that the JS
// performed with `new Date()`.
bsoncxx::types::b_date nowDate() {
  return bsoncxx::types::b_date{std::chrono::system_clock::now()};
}

long long nowMillisLocal() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

// -----------------------------------------------------------------------------
// Indexes
// -----------------------------------------------------------------------------
void ensureIndexes() {
  try {
    auto col = pulse::db::collection(kCollection);

    // otpSchema.index({ identifier: 1, purpose: 1, verified: 1 })
    col.create_index(make_document(kvp("identifier", 1),
                                   kvp("purpose", 1),
                                   kvp("verified", 1)));

    // otpSchema.index({ userId: 1 })
    col.create_index(make_document(kvp("userId", 1)));

    // otpSchema.index({ createdAt: 1 }, { expireAfterSeconds: 1800 })
    {
      mongocxx::options::index opts{};
      opts.expire_after(std::chrono::seconds(1800));
      col.create_index(make_document(kvp("createdAt", 1)), opts);
    }

    // field expiresAt: { expires: 0 } -> TTL index { expiresAt: 1 } expireAfterSeconds:0
    {
      mongocxx::options::index opts{};
      opts.expire_after(std::chrono::seconds(0));
      col.create_index(make_document(kvp("expiresAt", 1)), opts);
    }

    pulse::log::info("OTP indexes ensured on collection '{}'", kCollection);
  } catch (const std::exception& e) {
    pulse::log::error("OTP ensureIndexes failed: {}", e.what());
    throw;
  }
}

// -----------------------------------------------------------------------------
// Defaults / serialization
// -----------------------------------------------------------------------------
Json::Value applyDefaults(Json::Value doc) {
  if (!doc.isObject()) doc = Json::Value(Json::objectValue);

  // userId: { default: null }
  if (!doc.isMember("userId")) doc["userId"] = Json::Value(Json::nullValue);

  // attempts: { default: 0 }
  if (!doc.isMember("attempts")) doc["attempts"] = 0;

  // maxAttempts: { default: 3 }
  if (!doc.isMember("maxAttempts")) doc["maxAttempts"] = 3;

  // verified: { default: false }
  if (!doc.isMember("verified")) doc["verified"] = false;

  // verifiedAt: { default: null }
  if (!doc.isMember("verifiedAt")) doc["verifiedAt"] = Json::Value(Json::nullValue);

  // userAgent: { default: '' }
  if (!doc.isMember("userAgent")) doc["userAgent"] = "";

  // timestamps: true -> createdAt / updatedAt
  const std::string now = pulse::bsonjson::nowIso8601();
  if (!doc.isMember("createdAt")) doc["createdAt"] = now;
  if (!doc.isMember("updatedAt")) doc["updatedAt"] = now;

  // Mongoose version key.
  if (!doc.isMember("__v")) doc["__v"] = 0;

  return doc;
}

Json::Value sanitizeForOutput(Json::Value doc) {
  if (!doc.isObject()) return doc;
  // No select:false fields and no custom toJSON transform in otpSchema; default
  // Mongoose JSON keeps every field. We only drop the internal version key.
  doc.removeMember("__v");
  return doc;
}

// -----------------------------------------------------------------------------
// Statics
// -----------------------------------------------------------------------------
std::optional<Json::Value> findValidOTP(const std::string& identifier,
                                        const std::string& purpose) {
  try {
    auto col = pulse::db::collection(kCollection);

    // { identifier, purpose, verified: false, expiresAt: { $gt: new Date() } }
    auto filter = make_document(
        kvp("identifier", identifier),
        kvp("purpose", purpose),
        kvp("verified", false),
        kvp("expiresAt", make_document(kvp("$gt", nowDate()))));

    // .sort({ createdAt: -1 })
    mongocxx::options::find opts{};
    opts.sort(make_document(kvp("createdAt", -1)));

    auto result = col.find_one(filter.view(), opts);
    if (!result) return std::nullopt;
    return pulse::bsonjson::toJson(result->view());
  } catch (const std::exception& e) {
    pulse::log::error("OTP findValidOTP failed: {}", e.what());
    throw;
  }
}

long long cleanupExpired() {
  try {
    auto col = pulse::db::collection(kCollection);

    // { expiresAt: { $lt: new Date() } }
    auto filter = make_document(
        kvp("expiresAt", make_document(kvp("$lt", nowDate()))));

    auto result = col.delete_many(filter.view());
    if (!result) return 0;
    return result->deleted_count();
  } catch (const std::exception& e) {
    pulse::log::error("OTP cleanupExpired failed: {}", e.what());
    throw;
  }
}

// -----------------------------------------------------------------------------
// Instance helpers
// -----------------------------------------------------------------------------
std::optional<int> incrementAttempts(const bsoncxx::oid& id) {
  try {
    auto col = pulse::db::collection(kCollection);

    // this.attempts += 1; this.save();
    auto filter = make_document(kvp("_id", id));
    auto update = make_document(kvp("$inc", make_document(kvp("attempts", 1))));

    mongocxx::options::find_one_and_update opts{};
    opts.return_document(mongocxx::options::return_document::k_after);

    auto result = col.find_one_and_update(filter.view(), update.view(), opts);
    if (!result) return std::nullopt;

    auto view = result->view();
    auto it = view.find("attempts");
    if (it == view.end()) return std::nullopt;
    switch (it->type()) {
      case bsoncxx::type::k_int32: return it->get_int32().value;
      case bsoncxx::type::k_int64: return static_cast<int>(it->get_int64().value);
      case bsoncxx::type::k_double: return static_cast<int>(it->get_double().value);
      default: return std::nullopt;
    }
  } catch (const std::exception& e) {
    pulse::log::error("OTP incrementAttempts failed: {}", e.what());
    throw;
  }
}

bool markAsVerified(const bsoncxx::oid& id) {
  try {
    auto col = pulse::db::collection(kCollection);

    // this.verified = true; this.verifiedAt = new Date(); this.save();
    auto filter = make_document(kvp("_id", id));
    auto update = make_document(kvp("$set", make_document(
        kvp("verified", true),
        kvp("verifiedAt", nowDate()))));

    auto result = col.update_one(filter.view(), update.view());
    if (!result) return false;
    return result->modified_count() > 0;
  } catch (const std::exception& e) {
    pulse::log::error("OTP markAsVerified failed: {}", e.what());
    throw;
  }
}

bool isExpired(long long expiresAtMillis, long long nowMillis) {
  // return this.expiresAt < new Date();
  return expiresAtMillis < nowMillis;
}

bool isExpired(const std::string& expiresAtIso) {
  // Parse the ISO-8601 UTC timestamp (format: YYYY-MM-DDTHH:MM:SS.mmmZ).
  std::tm tm{};
  int ms = 0;
  // Use sscanf for portability across compilers (no get_time locale quirks).
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
  if (std::sscanf(expiresAtIso.c_str(), "%d-%d-%dT%d:%d:%d.%dZ",
                  &y, &mo, &d, &h, &mi, &s, &ms) >= 6) {
    tm.tm_year = y - 1900;
    tm.tm_mon  = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min  = mi;
    tm.tm_sec  = s;
#if defined(_WIN32)
    std::time_t t = _mkgmtime(&tm);
#else
    std::time_t t = timegm(&tm);
#endif
    long long expiresMillis = static_cast<long long>(t) * 1000 + ms;
    return isExpired(expiresMillis, nowMillisLocal());
  }
  // Unparseable timestamp: treat as expired (fail safe).
  return true;
}

bool isMaxAttemptsReached(int attempts, int maxAttempts) {
  // return this.attempts >= this.maxAttempts;
  return attempts >= maxAttempts;
}

} // namespace pulse::models::otp
