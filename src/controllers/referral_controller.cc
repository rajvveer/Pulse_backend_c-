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
#include <openssl/rand.h>

#include <mongocxx/collection.hpp>
#include <mongocxx/client_session.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/find_one_and_update.hpp>
#include <mongocxx/cursor.hpp>
#include <mongocxx/exception/operation_exception.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <stdexcept>
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
std::uint32_t secureUniform(std::uint32_t upperExclusive) {
  if (upperExclusive == 0) throw std::invalid_argument("empty random range");
  const std::uint64_t range =
      static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1;
  const std::uint64_t limit = range - (range % upperExclusive);
  std::uint32_t value = 0;
  do {
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&value), sizeof(value)) != 1)
      throw std::runtime_error("secure referral-code generation failed");
  } while (static_cast<std::uint64_t>(value) >= limit);
  return value % upperExclusive;
}

std::string generateShortCode() {
  static const char letters[] = "abcdefghijklmnopqrstuvwxyz";
  static const char base36[]  = "abcdefghijklmnopqrstuvwxyz0123456789";
  std::string id;
  id.reserve(8);
  id.push_back(letters[secureUniform(26)]);
  for (int i = 1; i < 8; ++i) id.push_back(base36[secureUniform(36)]);
  return toUpper(id);
}

bool isDuplicateKey(const mongocxx::operation_exception& e) {
  return e.code().value() == 11000 ||
         std::string(e.what()).find("E11000") != std::string::npos;
}

bsoncxx::types::b_date nowDate() {
  return bsoncxx::types::b_date{std::chrono::system_clock::now()};
}

class ReferralRequestError : public std::runtime_error {
public:
  ReferralRequestError(drogon::HttpStatusCode status,
                       const std::string& message)
      : std::runtime_error(message), status_(status) {}

  drogon::HttpStatusCode status() const noexcept { return status_; }

private:
  drogon::HttpStatusCode status_;
};

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
      mongocxx::options::find_one_and_update opts{};
      opts.return_document(mongocxx::options::return_document::k_after);

      for (int attempt = 0; attempt < 8 && referralCode.empty(); ++attempt) {
        const std::string candidate = generateShortCode();
        try {
          auto updated = col.find_one_and_update(
              make_document(
                  kvp("_id", *oid),
                  kvp("$or", make_array(
                      make_document(kvp("referralCode", make_document(
                          kvp("$exists", false)))),
                      make_document(kvp("referralCode",
                                        bsoncxx::types::b_null{})),
                      make_document(kvp("referralCode", ""))))),
              make_document(kvp("$set", make_document(
                  kvp("referralCode", candidate),
                  kvp("updatedAt", nowDate())))),
              opts);
          if (updated) {
            referralCode = pulse::bsonjson::toJson(updated->view())
                               .get("referralCode", "").asString();
          } else {
            // Another request generated this user's code first.
            auto raced = col.find_one(make_document(kvp("_id", *oid)));
            if (raced)
              referralCode = pulse::bsonjson::toJson(raced->view())
                                 .get("referralCode", "").asString();
          }
        } catch (const mongocxx::operation_exception& e) {
          if (!isDuplicateKey(e)) throw;
          // Candidate collision with another user; generate a fresh code.
        }
      }
      if (referralCode.empty())
        throw std::runtime_error("Failed to allocate a unique referral code");
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

    const std::string currentUserId = authUserId(req);
    auto currentOid = pulse::bsonjson::tryOid(currentUserId);
    if (!currentOid) {
      callback(errMessage(drogon::k404NotFound, "User not found"));
      return;
    }
    Json::Value referrer(Json::objectValue);
    bool badgeAwarded = false;

    // The claim, both badge awards, and count increment commit together. The
    // referredBy:null predicate is the one-time gate, so only the winning
    // concurrent request can reach the referrer's $inc.
    pulse::db::ClientHandle dbHandle;
    auto col = dbHandle.collection(pulse::models::user::kCollection);
    auto session = dbHandle.client().start_session();
    session.with_transaction([&](mongocxx::client_session* tx) {
      badgeAwarded = false;

      auto currentFound =
          col.find_one(*tx, make_document(kvp("_id", *currentOid)));
      if (!currentFound)
        throw ReferralRequestError(drogon::k404NotFound, "User not found");
      Json::Value currentUser =
          pulse::bsonjson::toJson(currentFound->view());
      if (currentUser.isMember("referredBy") &&
          !currentUser["referredBy"].isNull()) {
        throw ReferralRequestError(
            drogon::k409Conflict, "You have already used a referral code");
      }

      auto referrerFound = col.find_one(
          *tx, make_document(kvp("referralCode", toUpper(code)),
                             kvp("isActive", true)));
      if (!referrerFound)
        throw ReferralRequestError(drogon::k404NotFound,
                                   "Invalid referral code");
      referrer = pulse::bsonjson::toJson(referrerFound->view());
      auto referrerOid =
          pulse::bsonjson::tryOid(referrer.get("_id", "").asString());
      if (!referrerOid)
        throw ReferralRequestError(drogon::k404NotFound,
                                   "Invalid referral code");
      if (*referrerOid == *currentOid)
        throw ReferralRequestError(drogon::k400BadRequest,
                                   "You cannot use your own referral code");

      mongocxx::options::find_one_and_update claimOpts{};
      claimOpts.return_document(mongocxx::options::return_document::k_after);
      auto claimed = col.find_one_and_update(
          *tx,
          make_document(kvp("_id", *currentOid),
                        // Equality to null also matches a missing field.
                        kvp("referredBy", bsoncxx::types::b_null{})),
          make_document(kvp("$set", make_document(
              kvp("referredBy", *referrerOid), kvp("updatedAt", nowDate())))),
          claimOpts);
      if (!claimed)
        throw ReferralRequestError(
            drogon::k409Conflict, "You have already used a referral code");

      // Badge type is the identity. Conditional pushes prevent duplicate badge
      // types even though each earnedAt value is necessarily different.
      auto currentBadgeResult = col.update_one(
          *tx,
          make_document(kvp("_id", *currentOid),
                        kvp("badges.type", make_document(
                            kvp("$ne", "early-adopter")))),
          make_document(kvp("$push", make_document(kvp(
              "badges", make_document(kvp("type", "early-adopter"),
                                      kvp("earnedAt", nowDate())))))));
      badgeAwarded = currentBadgeResult &&
                     currentBadgeResult->modified_count() == 1;

      auto countResult = col.update_one(
          *tx,
          make_document(kvp("_id", *referrerOid), kvp("isActive", true)),
          make_document(
              kvp("$inc", make_document(kvp("referralCount", 1))),
              kvp("$set", make_document(kvp("updatedAt", nowDate())))));
      if (!countResult || countResult->matched_count() != 1)
        throw ReferralRequestError(drogon::k404NotFound,
                                   "Invalid referral code");

      col.update_one(
          *tx,
          make_document(kvp("_id", *referrerOid),
                        kvp("badges.type", make_document(
                            kvp("$ne", "early-adopter")))),
          make_document(kvp("$push", make_document(kvp(
              "badges", make_document(kvp("type", "early-adopter"),
                                      kvp("earnedAt", nowDate())))))));
    });

    // ── Apply referral to current user (currentUser.save()) ──
    // ── Update referrer (referrer.save()) ──
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
    data["badgeAwarded"] = badgeAwarded;

    Json::Value extra(Json::objectValue);
    extra["message"] =
        "Referral code applied successfully! You both earned an Early Adopter badge \xF0\x9F\x8E\x89";
    extra["data"] = data;
    callback(pulse::http::success(extra));
  } catch (const ReferralRequestError& e) {
    callback(errMessage(e.status(), e.what()));
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
