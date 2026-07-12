// user.cc — implementation of the User model port (src/models/User.js).
#include "pulse/models/user.hpp"

#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"
#include "pulse/models/follow.hpp"  // getSuggestedUsers -> Follow.getFollowingIds

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/oid.hpp>

#include <mongocxx/collection.hpp>
#include <mongocxx/options/index.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/cursor.hpp>

#include <cctype>
#include <chrono>
#include <string>

namespace pulse::models::user {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;
using bld::make_array;

namespace {

// Escape a string for safe literal use inside a regex (mirrors the JS
// String(term).replace(/[.*+?^${}()|[\]\\]/g, '\\$&')).
std::string escapeRegex(const std::string& in) {
  static const std::string specials = ".*+?^${}()|[]\\";
  std::string out;
  out.reserve(in.size() * 2);
  for (char c : in) {
    if (specials.find(c) != std::string::npos) out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

// The public profile projection used by searchUsers / getTrendingUsers /
// getSuggestedUsers:
//   'username profile.displayName profile.avatar profile.bio isVerified stats'
// _id is included by default in Mongoose.
bsoncxx::document::value publicProfileProjection() {
  return make_document(
      kvp("username", 1),
      kvp("profile.displayName", 1),
      kvp("profile.avatar", 1),
      kvp("profile.bio", 1),
      kvp("isVerified", 1),
      kvp("stats", 1));
}

std::vector<Json::Value> runFind(mongocxx::cursor cursor) {
  std::vector<Json::Value> out;
  for (auto&& doc : cursor) out.push_back(bsonjson::toJson(doc));
  return out;
}

} // namespace

// ===================== INDEXES =====================
void ensureIndexes() {
  auto col = pulse::db::collection(kCollection);

  // safeCreateIndex tolerates "index already exists" conflicts (e.g. the email
  // index pre-created by the Node backend) so one conflict doesn't abort the
  // rest of the batch.
  // username: unique + sparse
  pulse::db::safeCreateIndex(col, make_document(kvp("username", 1)),
                   make_document(kvp("unique", true), kvp("sparse", true)));
  // email: unique + sparse
  pulse::db::safeCreateIndex(col, make_document(kvp("email", 1)),
                   make_document(kvp("unique", true), kvp("sparse", true)));
  // phone: unique + sparse
  pulse::db::safeCreateIndex(col, make_document(kvp("phone", 1)),
                   make_document(kvp("unique", true), kvp("sparse", true)));
  // authMethods.type + authMethods.identifier
  pulse::db::safeCreateIndex(col, make_document(kvp("authMethods.type", 1),
                                 kvp("authMethods.identifier", 1)));

  // Text index over profile.displayName / username / profile.bio
  pulse::db::safeCreateIndex(col, make_document(kvp("profile.displayName", "text"),
                                 kvp("username", "text"),
                                 kvp("profile.bio", "text")));
  // isActive + isVerified
  pulse::db::safeCreateIndex(col, make_document(kvp("isActive", 1), kvp("isVerified", 1)));
  // stats.followers desc (trending)
  pulse::db::safeCreateIndex(col, make_document(kvp("stats.followers", -1)));

  // lastLocation: 2dsphere
  pulse::db::safeCreateIndex(col, make_document(kvp("lastLocation", "2dsphere")));

  // Activity indexes
  pulse::db::safeCreateIndex(col, make_document(kvp("lastActive", -1)));
  pulse::db::safeCreateIndex(col, make_document(kvp("createdAt", -1)));
  // referralCode: unique + sparse
  pulse::db::safeCreateIndex(col, make_document(kvp("referralCode", 1)),
                   make_document(kvp("unique", true), kvp("sparse", true)));
}

// ===================== STATICS =====================

std::optional<Json::Value> findByAuthMethod(const std::string& type,
                                            const std::string& identifier) {
  auto col = pulse::db::collection(kCollection);
  bsoncxx::document::value query = make_document(); // placeholder, overwritten

  if (type == "email") {
    query = make_document(
        kvp("$or", make_array(
                       make_document(kvp("email", identifier)),
                       make_document(kvp("authMethods.type", "email"),
                                     kvp("authMethods.identifier", identifier)))),
        kvp("isActive", true));
  } else if (type == "phone") {
    // cleanPhone = identifier with non-digits stripped.
    std::string cleanPhone;
    for (char c : identifier)
      if (c >= '0' && c <= '9') cleanPhone.push_back(c);

    std::string fourth = (cleanPhone.rfind("91", 0) == 0)
                             ? cleanPhone.substr(2)
                             : ("91" + cleanPhone);

    bld::array formats;
    formats.append(identifier);
    formats.append(cleanPhone);
    formats.append("+" + cleanPhone);
    formats.append(fourth);
    auto formatsVal = formats.extract();

    query = make_document(
        kvp("$or", make_array(
                       make_document(kvp("phone", make_document(kvp("$in", formatsVal.view())))),
                       make_document(kvp("authMethods.type", "phone"),
                                     kvp("authMethods.identifier",
                                         make_document(kvp("$in", formatsVal.view())))))),
        kvp("isActive", true));
  } else {
    query = make_document(
        kvp("authMethods.type", type),
        kvp("authMethods.identifier", identifier),
        kvp("isActive", true));
  }

  auto res = col.find_one(query.view());
  if (!res) return std::nullopt;
  return bsonjson::toJson(res->view());
}

std::optional<Json::Value> findByCredentials(const std::string& identifier) {
  auto col = pulse::db::collection(kCollection);

  std::string lowered = identifier;
  for (auto& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  auto query = make_document(
      kvp("$or", make_array(
                     make_document(kvp("email", identifier)),
                     make_document(kvp("username", lowered)),
                     make_document(kvp("phone", identifier)))),
      kvp("isActive", true));

  // .select('+passwordHash'): Mongoose's +field re-includes a select:false field
  // on top of the full document. The server stores every field; passwordHash is
  // only hidden by Mongoose's projection logic. Issuing find_one with no
  // projection returns the complete doc, passwordHash included — exactly what the
  // login flow needs to compare the hash.
  auto res = col.find_one(query.view());
  if (!res) return std::nullopt;
  return bsonjson::toJson(res->view());
}

std::vector<Json::Value> searchUsers(const std::string& searchTerm, int limit) {
  auto col = pulse::db::collection(kCollection);
  std::string safeTerm = escapeRegex(searchTerm);

  auto query = make_document(
      kvp("$or", make_array(
                     make_document(kvp("username",
                                       make_document(kvp("$regex", safeTerm),
                                                     kvp("$options", "i")))),
                     make_document(kvp("profile.displayName",
                                       make_document(kvp("$regex", safeTerm),
                                                     kvp("$options", "i")))))),
      kvp("isActive", true));

  mongocxx::options::find opts;
  opts.projection(publicProfileProjection());
  opts.limit(limit);
  return runFind(col.find(query.view(), opts));
}

std::vector<Json::Value> getTrendingUsers(int limit) {
  auto col = pulse::db::collection(kCollection);
  auto query = make_document(kvp("isActive", true));

  mongocxx::options::find opts;
  opts.sort(make_document(kvp("stats.followers", -1), kvp("lastActive", -1)));
  opts.projection(publicProfileProjection());
  opts.limit(limit);
  return runFind(col.find(query.view(), opts));
}

std::vector<std::string> getFollowingIds(const std::string& userId) {
  // Mongoose: const Follow = mongoose.model('Follow');
  //           const followingIds = await Follow.getFollowingIds(userId);
  return pulse::models::follow::getFollowingIds(userId);
}

std::vector<Json::Value> getSuggestedUsers(const std::string& userId, int limit) {
  auto col = pulse::db::collection(kCollection);

  auto selfOid = bsonjson::tryOid(userId);

  // $nin: [userId, ...followingIds]
  bld::array nin;
  if (selfOid) nin.append(*selfOid);
  for (const auto& fid : getFollowingIds(userId)) {
    auto o = bsonjson::tryOid(fid);
    if (o) nin.append(*o);
  }
  auto ninVal = nin.extract();

  auto query = make_document(
      kvp("_id", make_document(kvp("$nin", ninVal.view()))),
      kvp("isActive", true));

  mongocxx::options::find opts;
  opts.sort(make_document(kvp("stats.followers", -1)));
  opts.projection(publicProfileProjection());
  opts.limit(limit);
  return runFind(col.find(query.view(), opts));
}

// ===================== INSTANCE HELPERS =====================

bool isAccountLocked(long long lockUntilMs, long long nowMs) {
  return lockUntilMs > 0 && lockUntilMs > nowMs;
}

void incrementLoginAttempts(const std::string& oid,
                            int currentLoginAttempts,
                            std::optional<long long> lockUntilMs) {
  auto col = pulse::db::collection(kCollection);
  auto id = bsonjson::tryOid(oid);
  if (!id) return;
  auto filter = make_document(kvp("_id", *id));

  long long now = bsonjson::nowMillis();

  // If the lock has expired, restart the counter at 1 and drop the lock.
  if (lockUntilMs.has_value() && *lockUntilMs < now) {
    auto update = make_document(
        kvp("$unset", make_document(kvp("lockUntil", 1))),
        kvp("$set", make_document(kvp("loginAttempts", 1))));
    col.update_one(filter.view(), update.view());
    return;
  }

  bool currentlyLocked =
      lockUntilMs.has_value() && isAccountLocked(*lockUntilMs, now);

  // updates = { $inc: { loginAttempts: 1 } } plus optional lock $set.
  if (currentLoginAttempts + 1 >= kMaxLoginAttempts && !currentlyLocked) {
    bsoncxx::types::b_date lockDate{
        std::chrono::milliseconds(now + kLockDurationMs)};
    auto update = make_document(
        kvp("$inc", make_document(kvp("loginAttempts", 1))),
        kvp("$set", make_document(kvp("lockUntil", lockDate))));
    col.update_one(filter.view(), update.view());
  } else {
    auto update = make_document(
        kvp("$inc", make_document(kvp("loginAttempts", 1))));
    col.update_one(filter.view(), update.view());
  }
}

void resetLoginAttempts(const std::string& oid) {
  auto col = pulse::db::collection(kCollection);
  auto id = bsonjson::tryOid(oid);
  if (!id) return;
  auto filter = make_document(kvp("_id", *id));
  auto update = make_document(
      kvp("$unset", make_document(kvp("loginAttempts", 1), kvp("lockUntil", 1))));
  col.update_one(filter.view(), update.view());
}

void updateLastActive(const std::string& oid) {
  auto col = pulse::db::collection(kCollection);
  auto id = bsonjson::tryOid(oid);
  if (!id) return;
  auto filter = make_document(kvp("_id", *id));
  bsoncxx::types::b_date now{std::chrono::milliseconds(bsonjson::nowMillis())};
  auto update = make_document(kvp("$set", make_document(kvp("lastActive", now))));
  col.update_one(filter.view(), update.view());
}

void setOnlineStatus(const std::string& oid, bool isOnline) {
  auto col = pulse::db::collection(kCollection);
  auto id = bsonjson::tryOid(oid);
  if (!id) return;
  auto filter = make_document(kvp("_id", *id));

  bld::document setDoc;
  setDoc.append(kvp("isOnline", isOnline));
  if (isOnline) {
    bsoncxx::types::b_date now{std::chrono::milliseconds(bsonjson::nowMillis())};
    setDoc.append(kvp("lastActive", now));
  }
  auto update = make_document(kvp("$set", setDoc.extract()));
  col.update_one(filter.view(), update.view());
}

namespace {
// Helper: does `arr` (a Json array of id strings/oids) contain idStr?
bool arrayContainsId(const Json::Value& arr, const std::string& idStr) {
  if (!arr.isArray()) return false;
  for (const auto& el : arr) {
    if (el.isString() && el.asString() == idStr) return true;
  }
  return false;
}
} // namespace

bool canViewProfile(const Json::Value& userDoc, const std::string& viewerId) {
  // !this.privacy?.isPrivate -> public
  const Json::Value& privacy = userDoc["privacy"];
  bool isPrivate = privacy.isObject() && privacy.get("isPrivate", false).asBool();
  if (!isPrivate) return true;

  // owner can always view
  std::string selfId = userDoc.get("_id", "").asString();
  if (selfId == viewerId) return true;

  // else: follower present in legacy followers array (default deny otherwise)
  return arrayContainsId(userDoc["followers"], viewerId);
}

bool isFollowing(const Json::Value& userDoc, const std::string& userId) {
  return arrayContainsId(userDoc["following"], userId);
}

bool isFollower(const Json::Value& userDoc, const std::string& userId) {
  return arrayContainsId(userDoc["followers"], userId);
}

bool isBlocked(const Json::Value& userDoc, const std::string& userId) {
  return arrayContainsId(userDoc["blockedUsers"], userId);
}

// ===================== DEFAULTS / OUTPUT =====================

namespace {
// Set obj[key] = value only when the member is absent (preserve provided input).
void def(Json::Value& obj, const char* key, const Json::Value& value) {
  if (!obj.isMember(key)) obj[key] = value;
}
} // namespace

Json::Value applyDefaults(Json::Value doc) {
  if (!doc.isObject()) doc = Json::Value(Json::objectValue);

  // ----- profile -----
  Json::Value& profile = doc["profile"];
  if (!profile.isObject()) profile = Json::Value(Json::objectValue);
  def(profile, "displayName", "");
  def(profile, "bio", "");
  def(profile, "avatar", "");
  def(profile, "coverPhoto",
      "https://res.cloudinary.com/pulse/image/upload/v1/defaults/cover.png");
  def(profile, "website", "");
  def(profile, "location", "");
  def(profile, "dateOfBirth", Json::Value(Json::nullValue));
  def(profile, "gender", ""); // enum default ''

  // ----- role -----
  def(doc, "role", "user");

  // ----- account status -----
  def(doc, "isActive", true);
  def(doc, "isVerified", false);
  def(doc, "isOnline", false);
  def(doc, "lastActive", bsonjson::nowIso8601()); // Date.now

  // ----- stats -----
  Json::Value& stats = doc["stats"];
  if (!stats.isObject()) stats = Json::Value(Json::objectValue);
  def(stats, "posts", 0);
  def(stats, "followers", 0);
  def(stats, "following", 0);
  def(stats, "likes", 0);

  // ----- lastLocation -----
  Json::Value& loc = doc["lastLocation"];
  if (!loc.isObject()) loc = Json::Value(Json::objectValue);
  def(loc, "type", "Point");
  if (!loc.isMember("coordinates")) {
    Json::Value coords(Json::arrayValue);
    coords.append(0);
    coords.append(0);
    loc["coordinates"] = coords; // [0, 0]
  }
  def(loc, "address", "");
  def(loc, "updatedAt", bsonjson::nowIso8601()); // Date.now

  // ----- privacy -----
  Json::Value& privacy = doc["privacy"];
  if (!privacy.isObject()) privacy = Json::Value(Json::objectValue);
  def(privacy, "isPrivate", false);
  def(privacy, "showEmail", false);
  def(privacy, "showPhone", false);
  def(privacy, "showLocation", true);
  def(privacy, "showOnlineStatus", true);
  def(privacy, "allowMessages", "everyone");
  def(privacy, "allowTagging", true);

  // ----- settings -----
  Json::Value& settings = doc["settings"];
  if (!settings.isObject()) settings = Json::Value(Json::objectValue);
  def(settings, "radius", 1000);
  def(settings, "shareExactLocation", false);
  def(settings, "anonymousPosting", false);
  def(settings, "pushNotifications", true);
  def(settings, "emailNotifications", true);
  def(settings, "notifyOnFollow", true);
  def(settings, "notifyOnLike", true);
  def(settings, "notifyOnComment", true);
  def(settings, "notifyOnMention", true);
  def(settings, "theme", "auto");
  def(settings, "language", "en");

  // ----- security -----
  def(doc, "twoFactorEnabled", false);
  def(doc, "lastLoginAt", Json::Value(Json::nullValue));
  def(doc, "loginAttempts", 0);
  def(doc, "lockUntil", Json::Value(Json::nullValue));

  // ----- activity tracking -----
  def(doc, "lastPostAt", Json::Value(Json::nullValue));
  def(doc, "lastCommentAt", Json::Value(Json::nullValue));

  // ----- referral -----
  def(doc, "referredBy", Json::Value(Json::nullValue));
  def(doc, "referralCount", 0);

  // ----- onboarding -----
  Json::Value& onboarding = doc["onboarding"];
  if (!onboarding.isObject()) onboarding = Json::Value(Json::objectValue);
  if (!onboarding.isMember("topics")) onboarding["topics"] = Json::Value(Json::arrayValue);
  if (!onboarding.isMember("vibes")) onboarding["vibes"] = Json::Value(Json::arrayValue);
  def(onboarding, "completedAt", Json::Value(Json::nullValue));

  // ----- authMethods sub-doc defaults (verified/verifiedAt) -----
  if (doc.isMember("authMethods") && doc["authMethods"].isArray()) {
    for (auto& am : doc["authMethods"]) {
      if (am.isObject()) {
        def(am, "verified", false);
        def(am, "verifiedAt", Json::Value(Json::nullValue));
      }
    }
  }

  // ----- pre('save'): displayName defaults to username -----
  if ((!profile.isMember("displayName") || profile["displayName"].asString().empty()) &&
      doc.isMember("username") && doc["username"].isString() &&
      !doc["username"].asString().empty()) {
    profile["displayName"] = doc["username"];
  }

  // ----- timestamps (schema option timestamps: true) -----
  std::string now = bsonjson::nowIso8601();
  def(doc, "createdAt", now);
  def(doc, "updatedAt", now);

  return doc;
}

Json::Value sanitizeForOutput(Json::Value doc) {
  if (!doc.isObject()) return doc;
  // toJSON transform: delete passwordHash, loginAttempts, lockUntil, __v.
  doc.removeMember("passwordHash");
  doc.removeMember("loginAttempts");
  doc.removeMember("lockUntil");
  doc.removeMember("__v");
  // select:false / sensitive fields never returned to clients.
  doc.removeMember("twoFactorSecret");
  doc.removeMember("followers");
  doc.removeMember("following");
  return doc;
}

} // namespace pulse::models::user
