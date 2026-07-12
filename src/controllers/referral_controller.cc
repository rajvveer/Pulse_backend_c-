// referral_controller.cc — port of src/controllers/referralController.js
// and src/routes/referralRoutes.js (mounted at /api/v1/referral).
//
// 1:1 functional parity with the Express handlers: same DB queries, same
// request/response JSON shapes, same status codes. The JS controller uses the
// bare shapes { success:true, data } / { success:true, message, data } and the
// error shape { success:false, error } (NO `code` field), so those exact shapes
// are built here — NOT the {success,error,code} helper shape.
#include "pulse/controllers/referral_controller.hpp"

#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"
#include "pulse/http_response.hpp"
#include "pulse/models/user.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/oid.hpp>

#include <mongocxx/collection.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/cursor.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <random>
#include <string>
#include <vector>

namespace pulse::controllers {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;
using bld::make_array;

namespace {

// ── Response shape helpers matching the JS res.json shapes exactly ──
// The JS referralController never returns the {error,code} shape; it returns:
//   { success:false, error }            for errors
//   { success:true, message, data }     for applyCode confirmation
//   { success:true, data }              for payloads (pulse::http::ok)

drogon::HttpResponsePtr errMessage(drogon::HttpStatusCode code, const std::string& message) {
  Json::Value body(Json::objectValue);
  body["success"] = false;
  body["error"] = message;
  return pulse::http::json(code, std::move(body));
}

// The authenticated user injected by AuthFilter under "user".
Json::Value authUser(const drogon::HttpRequestPtr& req) {
  return req->getAttributes()->get<Json::Value>("user");
}
// req.user.userId || req.user._id
std::string authUserId(const drogon::HttpRequestPtr& req) {
  Json::Value u = authUser(req);
  if (!u.isObject()) return "";
  if (u.isMember("userId") && u["userId"].isString() && !u["userId"].asString().empty())
    return u["userId"].asString();
  if (u.isMember("_id") && u["_id"].isString()) return u["_id"].asString();
  return "";
}

// JS String.prototype.toUpperCase (ASCII): code.toUpperCase().
std::string toUpper(const std::string& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return out;
}

// createId().substring(0, 8).toUpperCase() — cuid2 yields a lowercase
// alphanumeric id (a leading letter + base36 body); we only keep the first 8
// chars and uppercase them, so an 8-char base36 string (leading letter,
// uppercased) is functionally equivalent for a short human-friendly code.
std::string generateShortCode() {
  static const char letters[] = "abcdefghijklmnopqrstuvwxyz";
  static const char base36[]  = "abcdefghijklmnopqrstuvwxyz0123456789";
  static thread_local std::mt19937 rng{std::random_device{}()};
  std::uniform_int_distribution<int> letterDist(0, 25);
  std::uniform_int_distribution<int> base36Dist(0, 35);
  std::string id;
  id.reserve(8);
  id.push_back(letters[letterDist(rng)]);  // cuid2 always starts with a letter
  for (int i = 1; i < 8; ++i) id.push_back(base36[base36Dist(rng)]);
  return toUpper(id);
}

bsoncxx::types::b_date nowDate() {
  return bsoncxx::types::b_date{std::chrono::system_clock::now()};
}

// user.badges?.some(b => b.type === 'early-adopter')
bool hasBadge(const Json::Value& user, const std::string& type) {
  if (!user.isObject() || !user.isMember("badges") || !user["badges"].isArray()) return false;
  for (const auto& b : user["badges"]) {
    if (b.isObject() && b.isMember("type") && b["type"].isString() && b["type"].asString() == type)
      return true;
  }
  return false;
}

}  // namespace

// ==========================================================================
// GET MY CODE — GET /api/v1/referral/my-code
// Returns the current user's referral code, generating one if it doesn't exist.
// ==========================================================================
void ReferralController::getMyCode(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    const std::string userId = authUserId(req);
    auto oid = pulse::bsonjson::tryOid(userId);

    auto col = pulse::db::collection(pulse::models::user::kCollection);
    auto found = oid ? col.find_one(make_document(kvp("_id", *oid)))
                     : bsoncxx::v_noabi::stdx::optional<bsoncxx::document::value>{};
    if (!found) {
      callback(errMessage(drogon::k404NotFound, "User not found"));
      return;
    }
    Json::Value user = pulse::bsonjson::toJson(found->view());

    // Generate code if missing (user.save()).
    std::string referralCode =
        user.isMember("referralCode") && user["referralCode"].isString()
            ? user["referralCode"].asString() : "";
    if (referralCode.empty()) {
      referralCode = generateShortCode();
      if (oid)
        col.update_one(make_document(kvp("_id", *oid)),
                       make_document(kvp("$set", make_document(
                           kvp("referralCode", referralCode)))));
    }

    const std::string shareUrl = "https://getpulse.app/join?ref=" + referralCode;

    Json::Value data(Json::objectValue);
    data["referralCode"] = referralCode;
    data["shareUrl"] = shareUrl;
    data["shareMessage"] =
        "Join me on Pulse \xE2\x80\x94 the social app with two sides! Use my code " +
        referralCode + " or tap: " + shareUrl;
    long long referralCount = user.isMember("referralCount") ? user["referralCount"].asInt64() : 0;
    data["referralCount"] = (Json::Int64)referralCount;

    callback(pulse::http::ok(data));
  } catch (const std::exception& e) {
    pulse::log::error("getMyCode error: {}", e.what());
    callback(errMessage(drogon::k500InternalServerError, "Failed to get referral code"));
  }
}

// ==========================================================================
// APPLY CODE — POST /api/v1/referral/apply
// Body: { code: "ABC123XY" }
// Links the current user to the referrer and awards badges to both.
// ==========================================================================
void ReferralController::applyCode(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    auto bodyJson = req->getJsonObject();
    Json::Value body = bodyJson ? *bodyJson : Json::Value(Json::objectValue);

    // if (!code || typeof code !== 'string')
    bool codeIsString = body.isMember("code") && body["code"].isString();
    std::string code = codeIsString ? body["code"].asString() : "";
    if (!codeIsString || code.empty()) {
      callback(errMessage(drogon::k400BadRequest, "Referral code is required"));
      return;
    }

    auto col = pulse::db::collection(pulse::models::user::kCollection);

    const std::string currentUserId = authUserId(req);
    auto currentOid = pulse::bsonjson::tryOid(currentUserId);
    bsoncxx::v_noabi::stdx::optional<bsoncxx::document::value> currentFound;
    if (currentOid) currentFound = col.find_one(make_document(kvp("_id", *currentOid)));
    if (!currentFound) {
      callback(errMessage(drogon::k404NotFound, "User not found"));
      return;
    }
    Json::Value currentUser = pulse::bsonjson::toJson(currentFound->view());

    // Already referred? (currentUser.referredBy is non-null)
    if (currentUser.isMember("referredBy") && !currentUser["referredBy"].isNull()) {
      callback(errMessage(drogon::k409Conflict, "You have already used a referral code"));
      return;
    }

    // Find referrer: { referralCode: code.toUpperCase(), isActive: true }
    auto referrerFound = col.find_one(make_document(
        kvp("referralCode", toUpper(code)), kvp("isActive", true)));
    if (!referrerFound) {
      callback(errMessage(drogon::k404NotFound, "Invalid referral code"));
      return;
    }
    Json::Value referrer = pulse::bsonjson::toJson(referrerFound->view());
    const std::string referrerId = referrer.get("_id", "").asString();

    // Can't refer yourself.
    if (referrerId == currentUser.get("_id", "").asString()) {
      callback(errMessage(drogon::k400BadRequest, "You cannot use your own referral code"));
      return;
    }
    auto referrerOid = pulse::bsonjson::tryOid(referrerId);

    // ── Apply referral to current user (currentUser.save()) ──
    // currentUser.referredBy = referrer._id; award early-adopter badge if missing.
    const bool hasEarlyAdopter = hasBadge(currentUser, "early-adopter");
    bld::document currentSet;
    if (referrerOid) currentSet.append(kvp("referredBy", *referrerOid));
    bld::document currentUpdate;
    currentUpdate.append(kvp("$set", currentSet.extract()));
    if (!hasEarlyAdopter) {
      currentUpdate.append(kvp("$push", make_document(kvp("badges",
          make_document(kvp("type", "early-adopter"), kvp("earnedAt", nowDate()))))));
    }
    if (currentOid)
      col.update_one(make_document(kvp("_id", *currentOid)), currentUpdate.extract());

    // ── Update referrer (referrer.save()) ──
    // referrer.referralCount = (referralCount || 0) + 1; award badge if missing.
    long long referrerCount =
        referrer.isMember("referralCount") ? referrer["referralCount"].asInt64() : 0;
    const bool referrerHasBadge = hasBadge(referrer, "early-adopter");
    bld::document referrerUpdate;
    referrerUpdate.append(kvp("$set", make_document(
        kvp("referralCount", (std::int64_t)(referrerCount + 1)))));
    if (!referrerHasBadge) {
      referrerUpdate.append(kvp("$push", make_document(kvp("badges",
          make_document(kvp("type", "early-adopter"), kvp("earnedAt", nowDate()))))));
    }
    if (referrerOid)
      col.update_one(make_document(kvp("_id", *referrerOid)), referrerUpdate.extract());

    // res.json({ success:true, message, data:{ referredBy:{username,displayName}, badgeAwarded } })
    Json::Value referredBy(Json::objectValue);
    referredBy["username"] = referrer.get("username", Json::Value(Json::nullValue));
    // referrer.profile?.displayName
    Json::Value displayName(Json::nullValue);
    if (referrer.isMember("profile") && referrer["profile"].isObject() &&
        referrer["profile"].isMember("displayName"))
      displayName = referrer["profile"]["displayName"];
    referredBy["displayName"] = displayName;

    Json::Value data(Json::objectValue);
    data["referredBy"] = referredBy;
    data["badgeAwarded"] = !hasEarlyAdopter;

    Json::Value extra(Json::objectValue);
    extra["message"] =
        "Referral code applied successfully! You both earned an Early Adopter badge \xF0\x9F\x8E\x89";
    extra["data"] = data;
    callback(pulse::http::success(extra));
  } catch (const std::exception& e) {
    pulse::log::error("applyCode error: {}", e.what());
    callback(errMessage(drogon::k500InternalServerError, "Failed to apply referral code"));
  }
}

// ==========================================================================
// GET STATS — GET /api/v1/referral/stats
// Returns referral count and the list of users who used the current user's code.
// ==========================================================================
void ReferralController::getStats(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    const std::string userId = authUserId(req);
    auto oid = pulse::bsonjson::tryOid(userId);

    auto col = pulse::db::collection(pulse::models::user::kCollection);

    // User.findById(userId).select('referralCode referralCount')
    mongocxx::options::find selfOpts;
    selfOpts.projection(make_document(kvp("referralCode", 1), kvp("referralCount", 1)));
    bsoncxx::v_noabi::stdx::optional<bsoncxx::document::value> found;
    if (oid) found = col.find_one(make_document(kvp("_id", *oid)), selfOpts);
    if (!found) {
      callback(errMessage(drogon::k404NotFound, "User not found"));
      return;
    }
    Json::Value user = pulse::bsonjson::toJson(found->view());

    // User.find({ referredBy: userId }).sort({createdAt:-1}).limit(50)
    //   .select('username profile.displayName profile.avatar createdAt').lean()
    std::vector<Json::Value> referred;
    if (oid) {
      mongocxx::options::find opts;
      opts.projection(make_document(kvp("username", 1), kvp("profile.displayName", 1),
                                    kvp("profile.avatar", 1), kvp("createdAt", 1)));
      opts.sort(make_document(kvp("createdAt", -1)));
      opts.limit(50);
      for (auto&& doc : col.find(make_document(kvp("referredBy", *oid)), opts))
        referred.push_back(pulse::bsonjson::toJson(doc));
    }

    // referredUsers.map(u => ({ username, displayName, avatar, joinedAt }))
    Json::Value referredUsers(Json::arrayValue);
    for (const auto& u : referred) {
      Json::Value entry(Json::objectValue);
      entry["username"] = u.get("username", Json::Value(Json::nullValue));
      Json::Value displayName(Json::nullValue);
      Json::Value avatar(Json::nullValue);
      if (u.isMember("profile") && u["profile"].isObject()) {
        if (u["profile"].isMember("displayName")) displayName = u["profile"]["displayName"];
        if (u["profile"].isMember("avatar")) avatar = u["profile"]["avatar"];
      }
      entry["displayName"] = displayName;
      entry["avatar"] = avatar;
      entry["joinedAt"] = u.get("createdAt", Json::Value(Json::nullValue));
      referredUsers.append(entry);
    }

    Json::Value data(Json::objectValue);
    // user.referralCode || null
    data["referralCode"] =
        user.isMember("referralCode") && user["referralCode"].isString() &&
                !user["referralCode"].asString().empty()
            ? Json::Value(user["referralCode"].asString())
            : Json::Value(Json::nullValue);
    // user.referralCount || 0
    long long totalReferrals = user.isMember("referralCount") ? user["referralCount"].asInt64() : 0;
    data["totalReferrals"] = (Json::Int64)totalReferrals;
    data["referredUsers"] = referredUsers;

    callback(pulse::http::ok(data));
  } catch (const std::exception& e) {
    pulse::log::error("getStats error: {}", e.what());
    callback(errMessage(drogon::k500InternalServerError, "Failed to get referral stats"));
  }
}

}  // namespace pulse::controllers
