// user_controller.cc — port of src/controllers/userController.js
// and src/routes/users.js (mounted at /api/v1/users).
//
// 1:1 functional parity with the Express handlers: same DB queries, same
// request/response JSON shapes, same status codes. The JS controller uses the
// bare shapes { success:true, data } / { success:true, message } and the error
// shape { success:false, message } (NO `error`/`code` fields), so those exact
// shapes are built here — NOT the {success,error,code} helper shape.
#include "pulse/controllers/user_controller.hpp"

#include <drogon/MultiPart.h>

#include "pulse/db.hpp"
#include "pulse/cache.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"
#include "pulse/http_response.hpp"
#include "pulse/jwt_service.hpp"
#include "pulse/models/user.hpp"
#include "pulse/models/post.hpp"
#include "pulse/models/follow.hpp"
#include "pulse/models/session.hpp"
#include "pulse/models/notification.hpp"
#include "pulse/services/embedding_service.hpp"
#include "pulse/services/user_vector_service.hpp"
#include "pulse/services/media_service.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/oid.hpp>

#include <mongocxx/collection.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/find_one_and_update.hpp>
#include <mongocxx/cursor.hpp>

#include <pulse_bcrypt.h>
#include <openssl/sha.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace pulse::controllers {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;
using bld::make_array;

namespace {

// ── Response shape helpers matching the JS res.json shapes exactly ──
// The JS controller never returns the {error,code} shape; it returns:
//   { success:false, message }   for errors
//   { success:true, message }    for action confirmations
//   { success:true, data }       for payloads (pulse::http::ok)

drogon::HttpResponsePtr errMessage(drogon::HttpStatusCode code, const std::string& message) {
  Json::Value body(Json::objectValue);
  body["success"] = false;
  body["message"] = message;
  return pulse::http::json(code, std::move(body));
}

drogon::HttpResponsePtr okMessage(const std::string& message) {
  Json::Value body(Json::objectValue);
  body["success"] = true;
  body["message"] = message;
  return pulse::http::json(drogon::k200OK, std::move(body));
}

// The authenticated user injected by AuthFilter under "user".
Json::Value authUser(const drogon::HttpRequestPtr& req) {
  return req->getAttributes()->get<Json::Value>("user");
}
std::string authUserId(const drogon::HttpRequestPtr& req) {
  Json::Value u = authUser(req);
  return u.isObject() ? u.get("userId", "").asString() : "";
}

std::string sha256Hex(const std::string& value) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest);
  std::ostringstream out;
  for (unsigned char byte : digest)
    out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
  return out.str();
}

std::string bearerToken(const drogon::HttpRequestPtr& req) {
  const std::string header = req->getHeader("authorization");
  return header.rfind("Bearer ", 0) == 0 ? header.substr(7) : "";
}

// escapeRegex(str) — mirrors src/utils/escapeRegex.js:
//   String(str).replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
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

// trim (JS String.prototype.trim — strips ASCII whitespace from both ends).
std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  auto isWs = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\n' ||
                                           c == '\r' || c == '\f' || c == '\v'; };
  while (b < e && isWs(s[b])) ++b;
  while (e > b && isWs(s[e - 1])) --e;
  return s.substr(b, e - b);
}

size_t utf8Length(const std::string& s) {
  size_t length = 0;
  for (unsigned char c : s)
    if ((c & 0xc0) != 0x80) ++length;
  return length;
}

// parseInt-like: JS parseInt(req.query.x) || fallback. Parses a leading integer;
// any non-numeric / empty result falls back to `def`.
int parseIntOr(const std::string& s, int def) {
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  size_t start = i;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
  size_t digits = i;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
  if (digits == i) return def;  // no digits -> NaN -> falsy -> def
  try {
    return std::stoi(s.substr(start, i - start));
  } catch (...) {
    return def;
  }
}

// bcrypt.compare(plain, hash) — bcrypt_checkpw returns 0 on match.
bool bcryptCompare(const std::string& plain, const std::string& hash) {
  return bcrypt_checkpw(plain.c_str(), hash.c_str()) == 0;
}
// bcrypt.hash(plain, 12)
std::string bcryptHash(const std::string& plain) {
  char salt[BCRYPT_HASHSIZE];
  char hash[BCRYPT_HASHSIZE];
  if (bcrypt_gensalt(12, salt) != 0) throw std::runtime_error("Failed to generate password salt");
  if (bcrypt_hashpw(plain.c_str(), salt, hash) != 0) throw std::runtime_error("Failed to hash password");
  return std::string(hash);
}

bsoncxx::types::b_date nowDate() {
  return bsoncxx::types::b_date{std::chrono::system_clock::now()};
}

// Project a fetched users-collection doc to the public list shape the JS
// populate select strings produce:
//   'username profile.displayName profile.avatar avatar isVerified' (+ _id).
bsoncxx::document::value listUserProjection() {
  return make_document(kvp("username", 1),
                       kvp("profile.displayName", 1),
                       kvp("profile.avatar", 1),
                       kvp("avatar", 1),
                       kvp("isVerified", 1));
}

// Build an allowlisted profile response. Never spread the stored user document
// into a public response: it also contains authentication, session, location,
// moderation and device fields that were never intended for other users.
Json::Value publicProfile(const Json::Value& user, bool isOwnProfile,
                          bool isFollowing) {
  Json::Value out(Json::objectValue);
  const auto copy = [&](const char* key) {
    if (user.isMember(key)) out[key] = user[key];
  };
  copy("_id");
  copy("username");
  copy("name");
  copy("avatar");
  copy("isVerified");

  const Json::Value privacy = user.isMember("privacy") && user["privacy"].isObject()
                                  ? user["privacy"]
                                  : Json::Value(Json::objectValue);
  const bool isPrivate = privacy.get("isPrivate", false).asBool();
  const bool maySeeDetails = isOwnProfile || isFollowing || !isPrivate;

  Json::Value publicPrivacy(Json::objectValue);
  publicPrivacy["isPrivate"] = isPrivate;
  if (privacy.isMember("allowMessages"))
    publicPrivacy["allowMessages"] = privacy["allowMessages"];
  if (privacy.isMember("allowTagging"))
    publicPrivacy["allowTagging"] = privacy["allowTagging"];
  out["privacy"] = std::move(publicPrivacy);

  Json::Value profile(Json::objectValue);
  if (user.isMember("profile") && user["profile"].isObject()) {
    const Json::Value& stored = user["profile"];
    for (const char* key : {"displayName", "avatar", "coverPhoto"}) {
      if (stored.isMember(key)) profile[key] = stored[key];
    }
    if (maySeeDetails) {
      for (const char* key : {"bio", "website"}) {
        if (stored.isMember(key)) profile[key] = stored[key];
      }
      if (privacy.get("showLocation", true).asBool() && stored.isMember("location"))
        profile["location"] = stored["location"];
    }
  }
  out["profile"] = std::move(profile);

  if (maySeeDetails && privacy.get("showOnlineStatus", true).asBool()) {
    copy("isOnline");
    copy("lastActive");
  }
  return out;
}

// Hydrate a list of follow docs' `<refField>` (follower/following ref) into the
// list-projected user subdocument, in one batch query, preserving order and
// dropping unresolved refs — mirrors .populate(refField, <select>) followed by
// .map(f => f[refField]).filter(Boolean).
std::vector<Json::Value> populateFollowRefs(const std::vector<Json::Value>& follows,
                                            const std::string& refField) {
  std::vector<std::string> ids;
  for (const auto& f : follows) {
    if (f.isMember(refField) && f[refField].isString()) {
      std::string hex = f[refField].asString();
      if (!hex.empty() && std::find(ids.begin(), ids.end(), hex) == ids.end())
        ids.push_back(hex);
    }
  }
  Json::Value byId(Json::objectValue);
  if (!ids.empty()) {
    bld::array in;
    for (const auto& hex : ids)
      if (auto o = pulse::bsonjson::tryOid(hex)) in.append(*o);
    auto filter = make_document(kvp("_id", make_document(kvp("$in", in.extract()))));
    mongocxx::options::find opts;
    opts.projection(listUserProjection());
    auto col = pulse::db::collection(pulse::models::user::kCollection);
    for (auto&& doc : col.find(filter.view(), opts)) {
      Json::Value u = pulse::bsonjson::toJson(doc);
      if (u.isMember("_id") && u["_id"].isString()) byId[u["_id"].asString()] = u;
    }
  }
  std::vector<Json::Value> out;
  for (const auto& f : follows) {
    if (!f.isMember(refField) || !f[refField].isString()) continue;
    std::string hex = f[refField].asString();
    if (byId.isMember(hex)) out.push_back(byId[hex]);  // .filter(Boolean)
  }
  return out;
}

// Build a Json array from a vector of Json values.
Json::Value toArray(const std::vector<Json::Value>& vec) {
  Json::Value arr(Json::arrayValue);
  for (const auto& v : vec) arr.append(v);
  return arr;
}

}  // namespace

// ==========================================================================
// SEARCH USERS — GET /api/v1/users/search
// ==========================================================================
void UserController::searchUsers(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    std::string q = req->getParameter("q");

    if (trim(q).empty()) {
      // res.json({ success: true, data: [] })
      callback(pulse::http::ok(Json::Value(Json::arrayValue)));
      return;
    }

    std::string term = trim(q);
    if (term.size() > 50) term = term.substr(0, 50);  // .slice(0, 50)

    const std::string currentUserId = authUserId(req);
    auto currentOid = pulse::bsonjson::tryOid(currentUserId);

    auto col = pulse::db::collection(pulse::models::user::kCollection);

    // Prefix search by username: { username: { $regex: `^${safePrefix}`, $options:'i' },
    //   isActive: true, _id: { $ne: req.user.userId } }
    std::string safePrefix = escapeRegex(term);
    bld::document prefixFilter;
    prefixFilter.append(kvp("username", make_document(kvp("$regex", "^" + safePrefix),
                                                      kvp("$options", "i"))));
    prefixFilter.append(kvp("isActive", true));
    if (currentOid)
      prefixFilter.append(kvp("_id", make_document(kvp("$ne", *currentOid))));

    mongocxx::options::find prefixOpts;
    prefixOpts.projection(listUserProjection());
    prefixOpts.limit(20);

    std::vector<Json::Value> users;
    std::unordered_set<std::string> seen;
    auto prefixFilterVal = prefixFilter.extract();
    for (auto&& doc : col.find(prefixFilterVal.view(), prefixOpts)) {
      Json::Value u = pulse::bsonjson::toJson(doc);
      users.push_back(u);
      if (u.isMember("_id") && u["_id"].isString()) seen.insert(u["_id"].asString());
    }

    // Augment with $text relevance when the prefix match is thin.
    if (users.size() < 20 && term.size() >= 2) {
      bld::document textFilter;
      textFilter.append(kvp("$text", make_document(kvp("$search", term))));
      textFilter.append(kvp("isActive", true));
      if (currentOid)
        textFilter.append(kvp("_id", make_document(kvp("$ne", *currentOid))));

      mongocxx::options::find textOpts;
      // projection includes textScore meta + the public fields.
      bld::document textProj;
      textProj.append(kvp("username", 1), kvp("profile.displayName", 1),
                      kvp("profile.avatar", 1), kvp("avatar", 1), kvp("isVerified", 1),
                      kvp("score", make_document(kvp("$meta", "textScore"))));
      textOpts.projection(textProj.extract());
      textOpts.sort(make_document(kvp("score", make_document(kvp("$meta", "textScore")))));
      textOpts.limit(20);

      auto textFilterVal = textFilter.extract();
      for (auto&& doc : col.find(textFilterVal.view(), textOpts)) {
        if (users.size() >= 20) break;
        Json::Value u = pulse::bsonjson::toJson(doc);
        std::string id = u.isMember("_id") && u["_id"].isString() ? u["_id"].asString() : "";
        if (!id.empty() && seen.find(id) == seen.end()) {
          u.removeMember("score");  // JS .select() did not include score in output
          users.push_back(u);
          seen.insert(id);
        }
      }
    }

    callback(pulse::http::ok(toArray(users)));
  } catch (const std::exception& e) {
    pulse::log::error("Search users error: {}", e.what());
    callback(errMessage(drogon::k500InternalServerError, e.what()));
  }
}

// ==========================================================================
// GET CURRENT USER — GET /api/v1/users/me
// ==========================================================================
void UserController::getCurrentUser(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    const std::string userId = authUserId(req);
    auto oid = pulse::bsonjson::tryOid(userId);

    auto col = pulse::db::collection(pulse::models::user::kCollection);
    mongocxx::options::find opts;
    // .select('-passwordHash -authMethods')
    opts.projection(make_document(kvp("passwordHash", 0), kvp("authMethods", 0)));

    // find_one returns the driver's bsoncxx::stdx::optional, not std::optional;
    // keep it as auto so the types match in v4.
    auto found = oid ? col.find_one(make_document(kvp("_id", *oid)), opts)
                     : bsoncxx::v_noabi::stdx::optional<bsoncxx::document::value>{};
    if (!found) {
      callback(errMessage(drogon::k404NotFound, "User not found"));
      return;
    }
    Json::Value user = pulse::bsonjson::toJson(found->view());

    // Stats from dedicated collections.
    long long followerCount  = pulse::models::follow::getFollowerCount(userId);
    long long followingCount = pulse::models::follow::getFollowingCount(userId);

    long long postCount = 0;
    {
      auto posts = pulse::db::collection(pulse::models::post::kCollection);
      bld::document pf;
      if (oid) pf.append(kvp("author", *oid));
      pf.append(kvp("isActive", true));
      postCount = posts.count_documents(pf.extract());
    }

    // Sync stats if drifted (user.save()).
    Json::Value& stats = user["stats"];
    long long curFollowers = stats.get("followers", 0).asInt64();
    long long curFollowing = stats.get("following", 0).asInt64();
    long long curPosts     = stats.get("posts", 0).asInt64();
    bool needsSave = false;
    if (curFollowers != followerCount)  { stats["followers"] = (Json::Int64)followerCount;  needsSave = true; }
    if (curFollowing != followingCount) { stats["following"] = (Json::Int64)followingCount; needsSave = true; }
    if (curPosts     != postCount)      { stats["posts"]     = (Json::Int64)postCount;      needsSave = true; }

    if (needsSave && oid) {
      pulse::log::info("Auto-fixed stats for user: {}", user.get("username", "").asString());
      col.update_one(make_document(kvp("_id", *oid)),
                     make_document(kvp("$set", make_document(
                         kvp("stats.followers", (std::int64_t)followerCount),
                         kvp("stats.following", (std::int64_t)followingCount),
                         kvp("stats.posts", (std::int64_t)postCount)))));
    }

    // Not .lean(): the Mongoose toJSON transform runs on res.json (strips
    // followers/following/loginAttempts/lockUntil/twoFactorSecret/__v).
    callback(pulse::http::ok(pulse::models::user::sanitizeForOutput(user)));
  } catch (const std::exception& e) {
    pulse::log::error("GetCurrentUser Error: {}", e.what());
    callback(errMessage(drogon::k500InternalServerError, "Server error"));
  }
}

// ==========================================================================
// GET USER BY USERNAME — GET /api/v1/users/{username}
// ==========================================================================
void UserController::getUserByUsername(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string username) {
  try {
    const std::string currentUserId = authUserId(req);

    auto col = pulse::db::collection(pulse::models::user::kCollection);
    mongocxx::options::find opts;
    // Inclusion projection is deliberate: newly-added private schema fields do
    // not silently become public through this endpoint.
    opts.projection(make_document(
        kvp("username", 1), kvp("name", 1), kvp("avatar", 1),
        kvp("profile.displayName", 1), kvp("profile.bio", 1),
        kvp("profile.avatar", 1), kvp("profile.coverPhoto", 1),
        kvp("profile.website", 1), kvp("profile.location", 1),
        kvp("isVerified", 1), kvp("isOnline", 1), kvp("lastActive", 1),
        kvp("privacy.isPrivate", 1), kvp("privacy.showLocation", 1),
        kvp("privacy.showOnlineStatus", 1), kvp("privacy.allowMessages", 1),
        kvp("privacy.allowTagging", 1)));
    auto found = col.find_one(
        make_document(kvp("username", username), kvp("isActive", true)), opts);
    if (!found) {
      callback(errMessage(drogon::k404NotFound, "User not found"));
      return;
    }
    Json::Value user = pulse::bsonjson::toJson(found->view());
    const std::string targetId = user.get("_id", "").asString();

    bool isFollowing         = pulse::models::follow::isFollowing(currentUserId, targetId);
    long long followerCount  = pulse::models::follow::getFollowerCount(targetId);
    long long followingCount = pulse::models::follow::getFollowingCount(targetId);

    long long postCount = 0;
    {
      auto posts = pulse::db::collection(pulse::models::post::kCollection);
      bld::document pf;
      if (auto o = pulse::bsonjson::tryOid(targetId)) pf.append(kvp("author", *o));
      pf.append(kvp("isActive", true));
      postCount = posts.count_documents(pf.extract());
    }

    bool isOwnProfile = targetId == currentUserId;

    // data: { ...user, stats:{posts,followers,following}, isFollowing, isOwnProfile }
    Json::Value data = publicProfile(user, isOwnProfile, isFollowing);
    Json::Value newStats(Json::objectValue);
    newStats["posts"]     = (Json::Int64)postCount;
    newStats["followers"] = (Json::Int64)followerCount;
    newStats["following"] = (Json::Int64)followingCount;
    data["stats"] = newStats;
    data["isFollowing"] = isFollowing;
    data["isOwnProfile"] = isOwnProfile;

    callback(pulse::http::ok(data));
  } catch (const std::exception& e) {
    pulse::log::error("{}", e.what());
    callback(errMessage(drogon::k500InternalServerError, "Server error"));
  }
}

// ==========================================================================
// GET USER POSTS — GET /api/v1/users/{username}/posts
// ==========================================================================
void UserController::getUserPosts(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string username) {
  try {
    const std::string currentUserId = authUserId(req);  // req.user?.userId

    auto users = pulse::db::collection(pulse::models::user::kCollection);
    mongocxx::options::find userOpts;
    userOpts.projection(make_document(kvp("privacy.isPrivate", 1)));
    auto found = users.find_one(
        make_document(kvp("username", username), kvp("isActive", true)), userOpts);
    if (!found) {
      auto resp = errMessage(drogon::k404NotFound, "User not found");
      resp->addHeader("Cache-Control", "no-store");
      callback(resp);
      return;
    }
    Json::Value user = pulse::bsonjson::toJson(found->view());
    const std::string targetId = user.get("_id", "").asString();
    auto targetOid = pulse::bsonjson::tryOid(targetId);
    if (!targetOid) {
      auto resp = errMessage(drogon::k404NotFound, "User not found");
      resp->addHeader("Cache-Control", "no-store");
      callback(resp);
      return;
    }

    bool isOwnProfile = !currentUserId.empty() && targetId == currentUserId;
    bool followsTarget = !isOwnProfile &&
        pulse::models::follow::isFollowing(currentUserId, targetId);

    // query = { author: user._id, isActive: true [, isAnonymous:false] }
    bld::document query;
    query.append(kvp("author", *targetOid));
    query.append(kvp("isActive", true));
    if (!isOwnProfile) {
      query.append(kvp("isAnonymous", false));
      if (followsTarget) {
        query.append(kvp("visibility", make_document(
            kvp("$in", make_array("public", "followers")))));
      } else {
        query.append(kvp("visibility", "public"));
      }
    }

    int page  = std::min(std::max(parseIntOr(req->getParameter("page"), 1), 1), 10000);
    int limit = std::min(std::max(parseIntOr(req->getParameter("limit"), 20), 1), 100);

    auto postsCol = pulse::db::collection(pulse::models::post::kCollection);
    mongocxx::options::find opts;
    opts.sort(make_document(kvp("createdAt", -1)));
    opts.skip(static_cast<std::int64_t>(page - 1) * limit);
    opts.limit(limit);

    std::vector<Json::Value> posts;
    auto queryVal = query.extract();
    for (auto&& doc : postsCol.find(queryVal.view(), opts))
      posts.push_back(pulse::models::post::sanitizeForOutput(
          pulse::bsonjson::toJson(doc)));

    // .populate('author', 'username name avatar profile')  — NO isVerified/stats.
    if (!posts.empty()) {
      std::vector<std::string> ids;
      for (const auto& p : posts) {
        std::string hex;
        if (p.isMember("author")) {
          const Json::Value& a = p["author"];
          if (a.isString()) hex = a.asString();
          else if (a.isObject() && a["_id"].isString()) hex = a["_id"].asString();
        }
        if (!hex.empty() && std::find(ids.begin(), ids.end(), hex) == ids.end())
          ids.push_back(hex);
      }
      Json::Value byId(Json::objectValue);
      if (!ids.empty()) {
        bld::array in;
        for (const auto& hex : ids)
          if (auto o = pulse::bsonjson::tryOid(hex)) in.append(*o);
        auto filter = make_document(kvp("_id", make_document(kvp("$in", in.extract()))));
        mongocxx::options::find aopts;
        aopts.projection(make_document(kvp("username", 1), kvp("name", 1),
                                       kvp("avatar", 1),
                                       kvp("profile.displayName", 1),
                                       kvp("profile.avatar", 1)));
        for (auto&& doc : users.find(filter.view(), aopts)) {
          Json::Value u = pulse::bsonjson::toJson(doc);
          if (u.isMember("_id") && u["_id"].isString()) byId[u["_id"].asString()] = u;
        }
      }
      for (auto& p : posts) {
        if (!p.isMember("author")) continue;
        std::string hex;
        const Json::Value& a = p["author"];
        if (a.isString()) hex = a.asString();
        else if (a.isObject() && a["_id"].isString()) hex = a["_id"].asString();
        if (!hex.empty() && byId.isMember(hex)) p["author"] = byId[hex];
      }
    }

    Json::Value body(Json::objectValue);
    body["success"] = true;
    body["data"] = toArray(posts);
    Json::Value pagination(Json::objectValue);
    pagination["page"] = page;
    pagination["limit"] = limit;
    pagination["hasMore"] = static_cast<int>(posts.size()) == limit;
    body["pagination"] = pagination;

    auto resp = pulse::http::json(drogon::k200OK, std::move(body));
    resp->addHeader("Cache-Control", "no-store");
    callback(resp);
  } catch (const std::exception& e) {
    pulse::log::error("{}", e.what());
    auto resp = errMessage(drogon::k500InternalServerError, "Server error");
    resp->addHeader("Cache-Control", "no-store");
    callback(resp);
  }
}

// ==========================================================================
// TOGGLE FOLLOW — POST /api/v1/users/{username}/follow
// ==========================================================================
void UserController::toggleFollow(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string username) {
  try {
    const std::string currentUserId = authUserId(req);

    auto users = pulse::db::collection(pulse::models::user::kCollection);
    mongocxx::options::find idOnly;
    idOnly.projection(make_document(kvp("_id", 1)));
    auto target = users.find_one(make_document(kvp("username", username)), idOnly);
    if (!target) {
      callback(errMessage(drogon::k404NotFound, "User not found"));
      return;
    }
    Json::Value targetUser = pulse::bsonjson::toJson(target->view());
    const std::string targetId = targetUser.get("_id", "").asString();

    if (targetId == currentUserId) {
      callback(errMessage(drogon::k400BadRequest, "Cannot follow yourself"));
      return;
    }

    auto toggle = pulse::models::follow::toggleFollow(currentUserId, targetId);
    long long followingCount = pulse::models::follow::getFollowingCount(currentUserId);

    // Maintain ONLY the cached counter stats.
    if (auto o = pulse::bsonjson::tryOid(targetId))
      users.update_one(make_document(kvp("_id", *o)),
                       make_document(kvp("$set", make_document(
                           kvp("stats.followers", (std::int64_t)toggle.followerCount)))));
    if (auto o = pulse::bsonjson::tryOid(currentUserId))
      users.update_one(make_document(kvp("_id", *o)),
                       make_document(kvp("$set", make_document(
                           kvp("stats.following", (std::int64_t)followingCount)))));

    // Invalidate cached follow graphs (best-effort).
    pulse::cache().del("followgraph:" + currentUserId);
    pulse::cache().del("reel:following:" + currentUserId);

    // Follow notification.
    if (toggle.followed) {
      try {
        Json::Value data(Json::objectValue);
        data["recipient"] = targetId;
        data["sender"] = currentUserId;
        data["type"] = "follow";
        data["message"] = "started following you";
        pulse::models::notification::createNotification(data);
      } catch (const std::exception& e) {
        pulse::log::error("Notification error: {}", e.what());
      }
    }

    Json::Value data(Json::objectValue);
    data["isFollowing"] = toggle.followed;
    callback(pulse::http::ok(data));
  } catch (const std::exception& e) {
    pulse::log::error("Follow Error: {}", e.what());
    callback(errMessage(drogon::k500InternalServerError, "Server error"));
  }
}

// ==========================================================================
// UPDATE PROFILE — PATCH /api/v1/users/me
// ==========================================================================
void UserController::updateProfile(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    static const std::vector<std::string> allowedFields = {
        "profile.displayName", "profile.bio", "profile.location",
        "profile.website", "profile.avatar", "profile.coverPhoto", "avatar",
        "settings.pushNotifications", "settings.emailNotifications",
        "settings.notifyOnLike", "settings.notifyOnComment",
        "settings.notifyOnFollow", "settings.notifyOnMention",
        "settings.theme", "settings.radius", "settings.shareExactLocation",
        "settings.anonymousPosting",
        "privacy.isPrivate", "privacy.showOnlineStatus", "privacy.allowMessages",
        "privacy.showEmail", "privacy.showPhone", "privacy.showLocation",
        "privacy.allowTagging"};

    auto bodyJson = req->getJsonObject();
    Json::Value body = bodyJson ? *bodyJson : Json::Value(Json::objectValue);

    static const std::unordered_set<std::string> booleanFields = {
        "settings.pushNotifications", "settings.emailNotifications",
        "settings.notifyOnLike", "settings.notifyOnComment",
        "settings.notifyOnFollow", "settings.notifyOnMention",
        "settings.shareExactLocation", "settings.anonymousPosting",
        "privacy.isPrivate", "privacy.showOnlineStatus",
        "privacy.showEmail", "privacy.showPhone", "privacy.showLocation",
        "privacy.allowTagging"};

    // Validate against the schema before constructing the dotted $set update.
    Json::Value updates(Json::objectValue);
    if (body.isObject()) {
      for (const auto& key : body.getMemberNames()) {
        if (std::find(allowedFields.begin(), allowedFields.end(), key) != allowedFields.end()) {
          Json::Value value = body[key];

          if (key.rfind("profile.", 0) == 0 || key == "avatar") {
            if (!value.isString()) {
              callback(errMessage(drogon::k400BadRequest,
                                  key + " must be a string"));
              return;
            }
            std::string text = value.asString();
            size_t maxLength = 2048;
            if (key == "profile.displayName") {
              text = trim(text);
              maxLength = 50;
            } else if (key == "profile.bio") {
              maxLength = 150;
            } else if (key == "profile.location" || key == "profile.website") {
              maxLength = 100;
            }
            if (utf8Length(text) > maxLength) {
              callback(errMessage(drogon::k400BadRequest,
                                  key + " exceeds its maximum length"));
              return;
            }
            value = text;
          } else if (booleanFields.count(key) != 0) {
            if (!value.isBool()) {
              callback(errMessage(drogon::k400BadRequest,
                                  key + " must be a boolean"));
              return;
            }
          } else if (key == "settings.radius") {
            if (!value.isNumeric() || !std::isfinite(value.asDouble()) ||
                value.asDouble() < 100.0 || value.asDouble() > 50000.0) {
              callback(errMessage(drogon::k400BadRequest,
                                  "settings.radius must be between 100 and 50000"));
              return;
            }
          } else if (key == "settings.theme") {
            if (!value.isString() ||
                (value.asString() != "light" && value.asString() != "dark" &&
                 value.asString() != "auto")) {
              callback(errMessage(drogon::k400BadRequest,
                                  "settings.theme must be light, dark, or auto"));
              return;
            }
          } else if (key == "privacy.allowMessages") {
            if (!value.isString() ||
                (value.asString() != "everyone" &&
                 value.asString() != "followers" &&
                 value.asString() != "none")) {
              callback(errMessage(
                  drogon::k400BadRequest,
                  "privacy.allowMessages must be everyone, followers, or none"));
              return;
            }
          }

          updates[key] = value;
          if (key == "profile.avatar") updates["avatar"] = value;
          if (key == "avatar") updates["profile.avatar"] = value;
        }
      }
    }

    if (updates.getMemberNames().empty()) {
      callback(errMessage(drogon::k400BadRequest, "No valid fields to update"));
      return;
    }

    const std::string userId = authUserId(req);
    auto oid = pulse::bsonjson::tryOid(userId);

    // $set: updates — serialize the whole dotted-key `updates` object to BSON.
    // The keys are dotted paths (e.g. "profile.bio"); Mongo treats a dotted key
    // inside $set as a nested-path set, exactly like Mongoose's { $set: updates }.
    auto updatesBson = pulse::bsonjson::fromJson(updates);

    auto col = pulse::db::collection(pulse::models::user::kCollection);
    mongocxx::options::find_one_and_update fopts;
    fopts.return_document(mongocxx::options::return_document::k_after);  // { new:true }
    fopts.projection(make_document(kvp("passwordHash", 0), kvp("authMethods", 0)));

    bsoncxx::v_noabi::stdx::optional<bsoncxx::document::value> updated;
    if (oid)
      updated = col.find_one_and_update(
          make_document(kvp("_id", *oid)),
          make_document(kvp("$set", updatesBson.view())),
          fopts);

    // Not .lean(): apply the Mongoose toJSON transform on the returned doc.
    Json::Value user = updated
        ? pulse::models::user::sanitizeForOutput(pulse::bsonjson::toJson(updated->view()))
        : Json::Value(Json::nullValue);
    callback(pulse::http::ok(user));
  } catch (const std::exception& e) {
    // res.status(400).json({ success:false, message: error.message })
    callback(errMessage(drogon::k400BadRequest, e.what()));
  }
}

// ==========================================================================
// UPLOAD AVATAR — POST /api/v1/users/me/avatar
// ==========================================================================
void UserController::uploadAvatar(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    // upload.single('avatar') -> req.file. Read the multipart 'avatar' part.
    drogon::MultiPartParser parser;
    std::string fileBytes;
    std::string fileName = "avatar";
    if (parser.parse(req) == 0) {
      for (const auto& f : parser.getFiles()) {
        if (f.getItemName() == "avatar") {
          fileBytes.assign(f.fileContent().data(), f.fileContent().size());
          if (!f.getFileName().empty()) fileName = f.getFileName();
          break;
        }
      }
    }

    if (fileBytes.empty()) {
      callback(errMessage(drogon::k400BadRequest, "No file uploaded"));
      return;
    }

    pulse::UploadResult result;
    try {
      // cloudinary.uploader.upload_stream({ folder:'pulse/avatars',
      //   resource_type:'auto', transformation:[{400x400 fill face}] })
      result = pulse::media().uploadAvatar(fileBytes, fileName);
    } catch (const std::exception& upErr) {
      // The JS upload callback error branch:
      //   res.status(500).json({ success:false, message:'Upload failed', error: error.message })
      Json::Value body(Json::objectValue);
      body["success"] = false;
      body["message"] = "Upload failed";
      body["error"] = upErr.what();
      callback(pulse::http::json(drogon::k500InternalServerError, std::move(body)));
      return;
    }

    const std::string userId = authUserId(req);
    auto oid = pulse::bsonjson::tryOid(userId);

    auto col = pulse::db::collection(pulse::models::user::kCollection);
    mongocxx::options::find_one_and_update fopts;
    fopts.return_document(mongocxx::options::return_document::k_after);  // { new:true }
    fopts.projection(make_document(kvp("passwordHash", 0), kvp("authMethods", 0)));

    bsoncxx::v_noabi::stdx::optional<bsoncxx::document::value> updated;
    if (oid)
      updated = col.find_one_and_update(
          make_document(kvp("_id", *oid)),
          make_document(kvp("$set", make_document(
              kvp("profile.avatar", result.secureUrl),
              kvp("avatar", result.secureUrl)))),
          fopts);

    // Not .lean(): apply the Mongoose toJSON transform on the returned doc.
    Json::Value user = updated
        ? pulse::models::user::sanitizeForOutput(pulse::bsonjson::toJson(updated->view()))
        : Json::Value(Json::nullValue);
    callback(pulse::http::ok(user));
  } catch (const std::exception& e) {
    pulse::log::error("Upload Error: {}", e.what());
    callback(errMessage(drogon::k500InternalServerError, "Upload failed"));
  }
}

// ==========================================================================
// GET FOLLOWERS — GET /api/v1/users/{username}/followers
// ==========================================================================
void UserController::getFollowers(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string username) {
  try {
    int page  = std::min(std::max(parseIntOr(req->getParameter("page"), 1), 1), 10000);
    int limit = std::min(std::max(parseIntOr(req->getParameter("limit"), 20), 1), 100);

    auto users = pulse::db::collection(pulse::models::user::kCollection);
    mongocxx::options::find idOnly;
    idOnly.projection(make_document(kvp("_id", 1)));
    auto user = users.find_one(make_document(kvp("username", username)), idOnly);
    if (!user) {
      callback(errMessage(drogon::k404NotFound, "User not found"));
      return;
    }
    const std::string targetId = pulse::bsonjson::toJson(user->view()).get("_id", "").asString();
    auto targetOid = pulse::bsonjson::tryOid(targetId);

    // Follow.find({ following: user._id }).sort(createdAt:-1).skip().limit()
    auto follows = pulse::db::collection(pulse::models::follow::kCollection);
    bld::document ff;
    if (targetOid) ff.append(kvp("following", *targetOid));
    mongocxx::options::find opts;
    opts.sort(make_document(kvp("createdAt", -1)));
    opts.skip(static_cast<std::int64_t>(page - 1) * limit);
    opts.limit(limit);

    std::vector<Json::Value> followDocs;
    auto ffVal = ff.extract();
    for (auto&& doc : follows.find(ffVal.view(), opts))
      followDocs.push_back(pulse::bsonjson::toJson(doc));

    // .populate('follower', <select>).map(f=>f.follower).filter(Boolean)
    std::vector<Json::Value> followers = populateFollowRefs(followDocs, "follower");
    callback(pulse::http::ok(toArray(followers)));
  } catch (const std::exception& e) {
    pulse::log::error("{}", e.what());
    callback(errMessage(drogon::k500InternalServerError, "Server error"));
  }
}

// ==========================================================================
// GET FOLLOWING — GET /api/v1/users/{username}/following
// ==========================================================================
void UserController::getFollowing(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string username) {
  try {
    int page  = std::min(std::max(parseIntOr(req->getParameter("page"), 1), 1), 10000);
    int limit = std::min(std::max(parseIntOr(req->getParameter("limit"), 20), 1), 100);

    auto users = pulse::db::collection(pulse::models::user::kCollection);
    mongocxx::options::find idOnly;
    idOnly.projection(make_document(kvp("_id", 1)));
    auto user = users.find_one(make_document(kvp("username", username)), idOnly);
    if (!user) {
      callback(errMessage(drogon::k404NotFound, "User not found"));
      return;
    }
    const std::string targetId = pulse::bsonjson::toJson(user->view()).get("_id", "").asString();
    auto targetOid = pulse::bsonjson::tryOid(targetId);

    // Follow.find({ follower: user._id }).sort(createdAt:-1).skip().limit()
    auto follows = pulse::db::collection(pulse::models::follow::kCollection);
    bld::document ff;
    if (targetOid) ff.append(kvp("follower", *targetOid));
    mongocxx::options::find opts;
    opts.sort(make_document(kvp("createdAt", -1)));
    opts.skip(static_cast<std::int64_t>(page - 1) * limit);
    opts.limit(limit);

    std::vector<Json::Value> followDocs;
    auto ffVal = ff.extract();
    for (auto&& doc : follows.find(ffVal.view(), opts))
      followDocs.push_back(pulse::bsonjson::toJson(doc));

    // .populate('following', <select>).map(f=>f.following).filter(Boolean)
    std::vector<Json::Value> following = populateFollowRefs(followDocs, "following");
    callback(pulse::http::ok(toArray(following)));
  } catch (const std::exception& e) {
    pulse::log::error("{}", e.what());
    callback(errMessage(drogon::k500InternalServerError, "Server error"));
  }
}

// ==========================================================================
// CHANGE PASSWORD — PATCH /api/v1/users/me/password
// ==========================================================================
void UserController::changePassword(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    auto bodyJson = req->getJsonObject();
    Json::Value body = bodyJson ? *bodyJson : Json::Value(Json::objectValue);
    std::string currentPassword = body.get("currentPassword", "").asString();
    std::string newPassword     = body.isMember("newPassword") && body["newPassword"].isString()
                                      ? body["newPassword"].asString() : "";

    // !newPassword || String(newPassword).length < 8
    bool newProvided = body.isMember("newPassword") && !body["newPassword"].isNull() &&
                       (!body["newPassword"].isString() || !body["newPassword"].asString().empty());
    if (!newProvided || newPassword.size() < 8) {
      callback(errMessage(drogon::k400BadRequest, "New password must be at least 8 characters"));
      return;
    }

    const std::string userId = authUserId(req);
    auto oid = pulse::bsonjson::tryOid(userId);

    auto col = pulse::db::collection(pulse::models::user::kCollection);
    // .select('+passwordHash') — full doc includes passwordHash.
    bsoncxx::v_noabi::stdx::optional<bsoncxx::document::value> found;
    if (oid) found = col.find_one(make_document(kvp("_id", *oid)));
    if (!found) {
      callback(errMessage(drogon::k404NotFound, "User not found"));
      return;
    }
    Json::Value user = pulse::bsonjson::toJson(found->view());

    std::string passwordHash = user.isMember("passwordHash") && user["passwordHash"].isString()
                                   ? user["passwordHash"].asString() : "";
    if (!passwordHash.empty()) {
      if (!body.isMember("currentPassword") || currentPassword.empty()) {
        callback(errMessage(drogon::k400BadRequest, "Current password is required"));
        return;
      }
      if (!bcryptCompare(currentPassword, passwordHash)) {
        callback(errMessage(drogon::k401Unauthorized, "Current password is incorrect"));
        return;
      }
    }

    std::string newHash = bcryptHash(newPassword);
    if (oid) {
      // Invalidate every refresh session before committing the new password.
      // Existing devices must authenticate again instead of silently refreshing.
      pulse::models::session::deactivateUserSessions(*oid);
      col.update_one(make_document(kvp("_id", *oid)),
                     make_document(kvp("$set", make_document(
                         kvp("passwordHash", newHash), kvp("updatedAt", nowDate())))));
      pulse::cache().del("auth_user:" + userId);
      const std::string accessToken = bearerToken(req);
      if (!accessToken.empty()) {
        pulse::cache().set("revoked_token:" + sha256Hex(accessToken), "1",
                           pulse::jwt().accessTokenTtlSeconds());
      }
    }

    callback(okMessage("Password updated successfully"));
  } catch (const std::exception& e) {
    pulse::log::error("Change password error: {}", e.what());
    callback(errMessage(drogon::k500InternalServerError, "Server error"));
  }
}

// ==========================================================================
// DELETE ACCOUNT — DELETE /api/v1/users/me
// ==========================================================================
void UserController::deleteAccount(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    const std::string userId = authUserId(req);
    auto oid = pulse::bsonjson::tryOid(userId);

    auto col = pulse::db::collection(pulse::models::user::kCollection);
    bsoncxx::v_noabi::stdx::optional<bsoncxx::document::value> found;
    if (oid) found = col.find_one(make_document(kvp("_id", *oid)));
    if (!found) {
      callback(errMessage(drogon::k404NotFound, "User not found"));
      return;
    }
    Json::Value user = pulse::bsonjson::toJson(found->view());

    // suffix = user._id.toString().slice(-6)
    std::string id = user.get("_id", "").asString();
    std::string suffix = id.size() >= 6 ? id.substr(id.size() - 6) : id;

    // Soft-delete: deactivate + scrub identifying / auth data.
    bld::document setDoc;
    setDoc.append(kvp("isActive", false));
    setDoc.append(kvp("isOnline", false));
    setDoc.append(kvp("username", "deleted_" + suffix));
    setDoc.append(kvp("profile.displayName", "Deleted User"));
    setDoc.append(kvp("profile.bio", ""));
    setDoc.append(kvp("profile.avatar", ""));
    bld::document unsetDoc;
    unsetDoc.append(kvp("email", ""));
    unsetDoc.append(kvp("phone", ""));
    unsetDoc.append(kvp("passwordHash", ""));
    // authMethods=[], fcmTokens=[]
    setDoc.append(kvp("authMethods", make_array()));
    setDoc.append(kvp("fcmTokens", make_array()));

    if (oid)
      col.update_one(make_document(kvp("_id", *oid)),
                     make_document(kvp("$set", setDoc.extract()),
                                   kvp("$unset", unsetDoc.extract())));

    if (oid) pulse::models::session::deactivateUserSessions(*oid);
    // AuthFilter caches account activity; evict it immediately so an already
    // issued access token cannot ride a stale "true" value for five minutes.
    pulse::cache().del("auth_user:" + userId);
    pulse::cache().del("followgraph:" + userId);
    pulse::cache().del("reel:following:" + userId);

    // Best-effort cleanup.
    try {
      auto posts = pulse::db::collection(pulse::models::post::kCollection);
      if (oid)
        posts.update_many(make_document(kvp("author", *oid)),
                          make_document(kvp("$set", make_document(kvp("isActive", false)))));
    } catch (...) {}
    try {
      auto follows = pulse::db::collection(pulse::models::follow::kCollection);
      if (oid)
        follows.delete_many(make_document(kvp("$or", make_array(
            make_document(kvp("follower", *oid)),
            make_document(kvp("following", *oid))))));
    } catch (...) {}

    callback(okMessage("Account deleted"));
  } catch (const std::exception& e) {
    pulse::log::error("Delete account error: {}", e.what());
    callback(errMessage(drogon::k500InternalServerError, "Server error"));
  }
}

// ==========================================================================
// BLOCK USER — POST /api/v1/users/{username}/block
// ==========================================================================
void UserController::blockUser(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string username) {
  try {
    const std::string currentUserId = authUserId(req);
    auto currentOid = pulse::bsonjson::tryOid(currentUserId);

    auto users = pulse::db::collection(pulse::models::user::kCollection);
    mongocxx::options::find idOnly;
    idOnly.projection(make_document(kvp("_id", 1)));
    auto target = users.find_one(make_document(kvp("username", username)), idOnly);
    if (!target) {
      callback(errMessage(drogon::k404NotFound, "User not found"));
      return;
    }
    const std::string targetId = pulse::bsonjson::toJson(target->view()).get("_id", "").asString();
    auto targetOid = pulse::bsonjson::tryOid(targetId);

    if (targetId == currentUserId) {
      callback(errMessage(drogon::k400BadRequest, "You cannot block yourself"));
      return;
    }

    // $addToSet: { blockedUsers: target._id }
    if (currentOid && targetOid)
      users.update_one(make_document(kvp("_id", *currentOid)),
                       make_document(kvp("$addToSet", make_document(kvp("blockedUsers", *targetOid)))));

    // Drop follow relationship both directions.
    try {
      auto follows = pulse::db::collection(pulse::models::follow::kCollection);
      if (currentOid && targetOid) {
        follows.delete_one(make_document(kvp("follower", *currentOid), kvp("following", *targetOid)));
        follows.delete_one(make_document(kvp("follower", *targetOid), kvp("following", *currentOid)));
      }
    } catch (...) {}

    callback(okMessage("User blocked"));
  } catch (const std::exception& e) {
    pulse::log::error("Block user error: {}", e.what());
    callback(errMessage(drogon::k500InternalServerError, "Server error"));
  }
}

// ==========================================================================
// UNBLOCK USER — DELETE /api/v1/users/{username}/block
// ==========================================================================
void UserController::unblockUser(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string username) {
  try {
    const std::string userId = authUserId(req);
    auto oid = pulse::bsonjson::tryOid(userId);

    auto users = pulse::db::collection(pulse::models::user::kCollection);
    mongocxx::options::find idOnly;
    idOnly.projection(make_document(kvp("_id", 1)));
    auto target = users.find_one(make_document(kvp("username", username)), idOnly);
    if (!target) {
      callback(errMessage(drogon::k404NotFound, "User not found"));
      return;
    }
    const std::string targetId = pulse::bsonjson::toJson(target->view()).get("_id", "").asString();
    auto targetOid = pulse::bsonjson::tryOid(targetId);

    // $pull: { blockedUsers: target._id }
    if (oid && targetOid)
      users.update_one(make_document(kvp("_id", *oid)),
                       make_document(kvp("$pull", make_document(kvp("blockedUsers", *targetOid)))));

    callback(okMessage("User unblocked"));
  } catch (const std::exception& e) {
    pulse::log::error("Unblock user error: {}", e.what());
    callback(errMessage(drogon::k500InternalServerError, "Server error"));
  }
}

// ==========================================================================
// GET BLOCKED USERS — GET /api/v1/users/me/blocked
// ==========================================================================
void UserController::getBlockedUsers(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    const std::string userId = authUserId(req);
    auto oid = pulse::bsonjson::tryOid(userId);

    auto col = pulse::db::collection(pulse::models::user::kCollection);
    mongocxx::options::find opts;
    opts.projection(make_document(kvp("blockedUsers", 1)));
    bsoncxx::v_noabi::stdx::optional<bsoncxx::document::value> found;
    if (oid) found = col.find_one(make_document(kvp("_id", *oid)), opts);

    Json::Value user = found ? pulse::bsonjson::toJson(found->view()) : Json::Value(Json::nullValue);

    // .populate('blockedUsers', 'username profile.displayName profile.avatar avatar isVerified')
    std::vector<Json::Value> blocked;
    if (user.isObject() && user.isMember("blockedUsers") && user["blockedUsers"].isArray()) {
      std::vector<std::string> ids;
      for (const auto& el : user["blockedUsers"])
        if (el.isString()) ids.push_back(el.asString());

      if (!ids.empty()) {
        bld::array in;
        for (const auto& hex : ids)
          if (auto o = pulse::bsonjson::tryOid(hex)) in.append(*o);
        auto filter = make_document(kvp("_id", make_document(kvp("$in", in.extract()))));
        mongocxx::options::find bopts;
        bopts.projection(listUserProjection());
        Json::Value byId(Json::objectValue);
        for (auto&& doc : col.find(filter.view(), bopts)) {
          Json::Value u = pulse::bsonjson::toJson(doc);
          if (u.isMember("_id") && u["_id"].isString()) byId[u["_id"].asString()] = u;
        }
        // Preserve the blockedUsers array order; drop unresolved (Mongoose
        // populate returns the resolved docs in array order).
        for (const auto& hex : ids)
          if (byId.isMember(hex)) blocked.push_back(byId[hex]);
      }
    }

    // data: user?.blockedUsers || []
    callback(pulse::http::ok(toArray(blocked)));
  } catch (const std::exception& e) {
    pulse::log::error("Get blocked users error: {}", e.what());
    callback(errMessage(drogon::k500InternalServerError, "Server error"));
  }
}

// ==========================================================================
// GET ONBOARDING OPTIONS — GET /api/v1/users/onboarding/options
// ==========================================================================
void UserController::getOnboardingOptions(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    Json::Value data(Json::objectValue);
    Json::Value topics(Json::arrayValue);
    for (const auto& t : pulse::EmbeddingService::topicKeys()) topics.append(t);
    Json::Value vibes(Json::arrayValue);
    for (const auto& v : pulse::EmbeddingService::vibes()) vibes.append(v);
    data["topics"] = topics;
    data["vibes"] = vibes;
    callback(pulse::http::ok(data));
  } catch (const std::exception& e) {
    pulse::log::error("{}", e.what());
    callback(errMessage(drogon::k500InternalServerError, "Server error"));
  }
}

// ==========================================================================
// SUBMIT ONBOARDING — POST /api/v1/users/onboarding
// ==========================================================================
void UserController::submitOnboarding(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    const std::string userId = authUserId(req);

    auto bodyJson = req->getJsonObject();
    Json::Value body = bodyJson ? *bodyJson : Json::Value(Json::objectValue);

    const auto& topicKeys = pulse::EmbeddingService::topicKeys();
    const auto& vibeKeys  = pulse::EmbeddingService::vibes();

    auto inList = [](const std::vector<std::string>& list, const std::string& v) {
      return std::find(list.begin(), list.end(), v) != list.end();
    };

    // topics = Array.isArray(body.topics) ? body.topics.filter(known).slice(0,10) : []
    std::vector<std::string> topics;
    if (body.isMember("topics") && body["topics"].isArray()) {
      for (const auto& t : body["topics"]) {
        if (topics.size() >= 10) break;
        if (t.isString() && inList(topicKeys, t.asString())) topics.push_back(t.asString());
      }
    }
    // vibes = Array.isArray(body.vibes) ? body.vibes.filter(known).slice(0,5) : []
    std::vector<std::string> vibes;
    if (body.isMember("vibes") && body["vibes"].isArray()) {
      for (const auto& v : body["vibes"]) {
        if (vibes.size() >= 5) break;
        if (v.isString() && inList(vibeKeys, v.asString())) vibes.push_back(v.asString());
      }
    }

    if (topics.empty() && vibes.empty()) {
      callback(errMessage(drogon::k400BadRequest, "Pick at least one topic or vibe"));
      return;
    }

    // Seed the cached taste vector now.
    pulse::userVector().seedFromOnboarding(userId, topics, vibes);

    // Persist picks: $set onboarding.topics/vibes/completedAt = new Date().
    auto oid = pulse::bsonjson::tryOid(userId);
    if (oid) {
      bld::array topicsArr;
      for (const auto& t : topics) topicsArr.append(t);
      bld::array vibesArr;
      for (const auto& v : vibes) vibesArr.append(v);
      auto col = pulse::db::collection(pulse::models::user::kCollection);
      col.update_one(make_document(kvp("_id", *oid)),
                     make_document(kvp("$set", make_document(
                         kvp("onboarding.topics", topicsArr.extract()),
                         kvp("onboarding.vibes", vibesArr.extract()),
                         kvp("onboarding.completedAt", nowDate())))));
    }

    // res.json({ success:true, data:{ topics, vibes } })
    Json::Value data(Json::objectValue);
    Json::Value topicsJson(Json::arrayValue);
    for (const auto& t : topics) topicsJson.append(t);
    Json::Value vibesJson(Json::arrayValue);
    for (const auto& v : vibes) vibesJson.append(v);
    data["topics"] = topicsJson;
    data["vibes"] = vibesJson;
    callback(pulse::http::ok(data));
  } catch (const std::exception& e) {
    pulse::log::error("Onboarding error: {}", e.what());
    callback(errMessage(drogon::k500InternalServerError, "Server error"));
  }
}

}  // namespace pulse::controllers
