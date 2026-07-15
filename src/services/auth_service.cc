// auth_service.cc — implementation of the auth service port (authService.js).
//
// See auth_service.hpp for the method map. Notes on fidelity:
//   * Tokens are hashed with SHA-256 (hex) before being stored / matched in
//     Mongo, exactly like JS `crypto.createHash('sha256')...digest('hex')`.
//   * Date fields that participate in TTL/range queries (expiresAt, lastActivity,
//     verifiedAt, lastLoginAt, updatedAt) are persisted as real BSON dates
//     (b_date), never ISO strings — otherwise `{ expiresAt: { $gt: now } }` in
//     refreshAccessToken would never match.
//   * Password hashing uses bcrypt cost 12 (JS `bcrypt.hash(password, 12)`).
//   * Google OAuth token verification (loginWithFirebase Strategy 2) is the only
//     external HTTP call authService makes itself; it uses Drogon's HttpClient
//     against the same userinfo / tokeninfo endpoints as the JS `fetch` calls.
#include "pulse/services/auth_service.hpp"

#include "pulse/db.hpp"
#include "pulse/cache.hpp"
#include "pulse/jwt_service.hpp"
#include "pulse/config.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"
#include "pulse/models/user.hpp"
#include "pulse/models/session.hpp"
#include "pulse/services/http_client.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/oid.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/find_one_and_update.hpp>
#include <mongocxx/exception/operation_exception.hpp>

#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>

#include <openssl/sha.h>
#include <openssl/rand.h>

#include <pulse_bcrypt.h>

#include <chrono>
#include <regex>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <algorithm>
#include <future>
#include <vector>
#include <utility>
#include <cstdlib>
#include <cctype>

namespace pulse::services {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;
using bld::make_array;

namespace {

// ── time helpers ──────────────────────────────────────────────────────────
bsoncxx::types::b_date nowDate() {
  return bsoncxx::types::b_date{std::chrono::system_clock::now()};
}
bsoncxx::types::b_date dateFromMillis(long long ms) {
  return bsoncxx::types::b_date{std::chrono::milliseconds{ms}};
}

// ── token hashing (crypto.createHash('sha256').update(t).digest('hex')) ──────
std::string hashToken(const std::string& token) {
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(token.data()), token.size(), hash);
  std::ostringstream os;
  for (unsigned char c : hash)
    os << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
  return os.str();
}

// crypto.randomBytes(n).toString('hex')
std::string randomHex(size_t bytes) {
  std::vector<unsigned char> buf(bytes);
  if (RAND_bytes(buf.data(), static_cast<int>(bytes)) != 1) {
    throw std::runtime_error("Secure random generation failed");
  }
  std::ostringstream os;
  for (unsigned char c : buf)
    os << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
  return os.str();
}

// bcrypt.hash(password, 12)
std::string bcryptHash(const std::string& password) {
  char salt[BCRYPT_HASHSIZE];
  char hash[BCRYPT_HASHSIZE];
  if (bcrypt_gensalt(12, salt) != 0)
    throw std::runtime_error("Failed to generate password salt");
  if (bcrypt_hashpw(password.c_str(), salt, hash) != 0)
    throw std::runtime_error("Failed to hash password");
  return std::string(hash);
}

// Read a possibly-missing string field as std::string ("" when absent/null).
std::string str(const Json::Value& v, const char* key) {
  return v.isObject() && v.isMember(key) && v[key].isString() ? v[key].asString() : "";
}

// The _id of a fetched User document (bsonjson renders _id as 24-hex string).
std::string docId(const Json::Value& user) { return str(user, "_id"); }

// applyDefaults(deviceInfo) defaulting, matching the JS
//   deviceName: deviceInfo.deviceName || 'Unknown Device', etc.
std::string deviceName(const DeviceInfo& d) {
  return d.deviceName && !d.deviceName->empty() ? *d.deviceName : "Unknown Device";
}
std::string appVersion(const DeviceInfo& d) {
  return d.appVersion && !d.appVersion->empty() ? *d.appVersion : "1.0.0";
}
std::string osVersion(const DeviceInfo& d) {
  return d.osVersion && !d.osVersion->empty() ? *d.osVersion : "Unknown";
}

// Build the access-token claims from a User document.
pulse::AccessClaims accessClaimsFor(const Json::Value& user) {
  pulse::AccessClaims c;
  c.userId     = docId(user);
  c.username   = str(user, "username");
  c.email      = str(user, "email");
  c.isVerified = user.isObject() && user.get("isVerified", false).asBool();
  return c;
}

std::vector<std::string> splitCsv(const std::string& input) {
  std::vector<std::string> out;
  std::stringstream ss(input);
  std::string value;
  while (std::getline(ss, value, ',')) {
    const auto first = value.find_first_not_of(" \t\r\n");
    const auto last = value.find_last_not_of(" \t\r\n");
    if (first != std::string::npos) out.push_back(value.substr(first, last - first + 1));
  }
  return out;
}

bool jsonBool(const Json::Value& value) {
  if (value.isBool()) return value.asBool();
  if (value.isString()) {
    std::string s = value.asString();
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s == "true" || s == "1";
  }
  return value.isNumeric() && value.asInt64() != 0;
}

std::string googleTokenAudience(const Json::Value& tokenInfo) {
  for (const char* key : {"aud", "audience", "issued_to", "azp"}) {
    const std::string value = str(tokenInfo, key);
    if (!value.empty()) return value;
  }
  return "";
}

bool isAllowedGoogleAudience(const std::string& audience) {
  if (audience.empty()) return false;
  auto& cfg = pulse::config();
  std::string configured = cfg.env("GOOGLE_OAUTH_CLIENT_IDS");
  if (configured.empty()) configured = cfg.env("GOOGLE_CLIENT_ID");
  for (const auto& allowed : splitCsv(configured)) {
    if (allowed == audience) return true;
  }
  return false;
}

}  // namespace

AuthService& AuthService::instance() {
  static AuthService s;
  return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// initiateAuth
// ─────────────────────────────────────────────────────────────────────────────
Json::Value AuthService::initiateAuth(const std::string& identifier,
                                      const std::string& method,
                                      const DeviceInfo& deviceInfo,
                                      const std::string& ipAddress) {
  try {
    pulse::log::info("Initiating {} authentication", method);

    // Validate method
    if (method != "email" && method != "phone") {
      throw std::runtime_error("Unsupported authentication method");
    }

    std::optional<Json::Value> existingUser;
    std::string processedIdentifier = identifier;

    if (method == "email") {
      existingUser = pulse::models::user::findByAuthMethod("email", identifier);
    } else {  // phone
      // Clean phone number properly: strip non-digits.
      std::string cleanPhone;
      for (char c : identifier)
        if (c >= '0' && c <= '9') cleanPhone.push_back(c);

      if (cleanPhone.rfind("91", 0) == 0 && cleanPhone.size() == 12) {
        processedIdentifier = cleanPhone;
      } else if (cleanPhone.size() == 10) {
        processedIdentifier = "91" + cleanPhone;
      } else {
        throw std::runtime_error("Invalid phone number format");
      }

      existingUser = pulse::models::user::findByAuthMethod("phone", "+" + processedIdentifier);
    }

    const bool userExists = existingUser.has_value();
    const std::string purpose = userExists ? "login" : "signup";

    // Send OTP based on method.
    const std::string existingUserId = userExists ? docId(*existingUser) : "";
    if (method == "email") {
      custom_otp::sendEmailOTP(identifier, purpose, existingUserId, ipAddress);
    } else {  // phone — use the 10-digit part for SMS
      const std::string phoneForSMS = processedIdentifier.substr(2);
      custom_otp::sendSMSOTP(phoneForSMS, purpose, existingUserId, ipAddress);
    }

    const std::string normalizedIdentifier =
        method == "phone" ? "+" + processedIdentifier : identifier;

    Json::Value out(Json::objectValue);
    out["success"]    = true;
    out["method"]     = method;
    out["identifier"] = normalizedIdentifier;
    out["nextStep"]   = "verify_otp";
    // Do not expose whether the identifier is already registered. Verification
    // recomputes the purpose server-side, so clients do not need this metadata.
    out["message"]    = "If the identifier is valid, an OTP has been sent";
    return out;
  } catch (const std::exception& e) {
    pulse::log::error("{} auth initiation error: {}", method, e.what());
    throw;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// loginWithFirebase
// ─────────────────────────────────────────────────────────────────────────────
namespace {

// One blocking GET via Drogon's HttpClient. Returns the parsed JSON body and the
// HTTP status code (0 on transport failure). Mirrors a single `fetch(...)`.
struct HttpResult { int status = 0; Json::Value body; bool ok = false; };

HttpResult httpGetJson(const std::string& url,
                       const std::vector<std::pair<std::string, std::string>>& headers = {}) {
  HttpResult result;
  try {
    // Split scheme+host, path, and query string.
    std::string scheme_host = url;
    std::string pathAndQuery = "/";
    auto schemeEnd = url.find("://");
    if (schemeEnd != std::string::npos) {
      auto pathStart = url.find('/', schemeEnd + 3);
      if (pathStart != std::string::npos) {
        scheme_host = url.substr(0, pathStart);
        pathAndQuery = url.substr(pathStart);
      }
    }
    auto client = drogon::HttpClient::newHttpClient(scheme_host);
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    // Transmit the path AND query string verbatim (Drogon forwards getPath()
    // unmodified in the request line), so e.g. tokeninfo?access_token=... works.
    req->setPath(pathAndQuery);
    for (const auto& h : headers) req->addHeader(h.first, h.second);

    std::promise<std::pair<drogon::ReqResult, drogon::HttpResponsePtr>> prom;
    auto fut = prom.get_future();
    client->sendRequest(
        req,
        [&prom](drogon::ReqResult rr, const drogon::HttpResponsePtr& resp) {
          prom.set_value({rr, resp});
        }, pulse::services::outboundHttpTimeoutSeconds());
    auto [rr, resp] = fut.get();

    if (rr != drogon::ReqResult::Ok || !resp) return result;
    result.status = static_cast<int>(resp->getStatusCode());
    result.ok = result.status >= 200 && result.status < 300;  // fetch .ok

    auto jsonPtr = resp->getJsonObject();
    if (jsonPtr) result.body = *jsonPtr;
  } catch (...) {
    // transport error -> status 0 / ok false, handled by caller
  }
  return result;
}

}  // namespace

Json::Value AuthService::loginWithFirebase(const std::string& idToken,
                                           const std::string& accessToken,
                                           const DeviceInfo& deviceInfo,
                                           const std::string& ipAddress,
                                           const Json::Value& extraInfo) {
  try {
    (void)extraInfo;  // profile metadata is accepted only from verified providers
    std::string email, name, picture, googleId;

    bool providerVerified = false;

    // Firebase ID tokens are accepted only through Firebase verification. An ID
    // token must never fall through and be reinterpreted as an OAuth access
    // token, because those token types have different audience semantics.
    if (!idToken.empty() && pulse::config().getBool("features.enableFirebaseAuth")) {
      if (!firebase::isAvailable()) {
        throw std::runtime_error("Firebase authentication is unavailable");
      }
      Json::Value decoded = firebase::verifyIdToken(idToken);
      email    = str(decoded, "email");
      name     = str(decoded, "name");
      picture  = str(decoded, "picture");
      googleId = str(decoded, "uid");
      if (googleId.empty())
        throw std::runtime_error("Firebase token has no subject");
      if (!decoded.get("emailVerified", false).asBool())
        throw std::runtime_error("Firebase email is not verified");
      providerVerified = true;
      pulse::log::info("Firebase identity token verified");
    }

    // Google access tokens are accepted only when the feature is explicitly
    // enabled and tokeninfo confirms that the token was issued to one of this
    // deployment's OAuth client IDs. userinfo alone proves a Google identity,
    // but not that the token was minted for Pulse.
    if (!providerVerified && !accessToken.empty()) {
      if (!pulse::config().getBool("features.enableGoogleLogin")) {
        throw std::runtime_error("Google authentication is disabled");
      }
      const std::string configuredClients =
          pulse::config().env("GOOGLE_OAUTH_CLIENT_IDS",
                              pulse::config().env("GOOGLE_CLIENT_ID"));
      if (configuredClients.empty()) {
        throw std::runtime_error("Google OAuth client ID is not configured");
      }

      try {
        auto tokenInfo = httpGetJson(
            "https://oauth2.googleapis.com/tokeninfo?access_token=" + accessToken);
        if (!tokenInfo.ok ||
            !isAllowedGoogleAudience(googleTokenAudience(tokenInfo.body))) {
          throw std::runtime_error("Google token audience mismatch");
        }

        auto googleResponse = httpGetJson(
            "https://www.googleapis.com/oauth2/v3/userinfo",
            {{"Authorization", "Bearer " + accessToken}});
        if (!googleResponse.ok) {
          throw std::runtime_error("Google userinfo request failed");
        }

        const std::string tokenEmail = str(tokenInfo.body, "email");
        const std::string userInfoEmail = str(googleResponse.body, "email");
        if (!tokenEmail.empty() && !userInfoEmail.empty() &&
            tokenEmail != userInfoEmail) {
          throw std::runtime_error("Google token identity mismatch");
        }

        bool emailVerified = false;
        for (const char* key : {"email_verified", "verified_email"}) {
          if (tokenInfo.body.isMember(key))
            emailVerified = emailVerified || jsonBool(tokenInfo.body[key]);
          if (googleResponse.body.isMember(key))
            emailVerified = emailVerified || jsonBool(googleResponse.body[key]);
        }
        if (!emailVerified) {
          throw std::runtime_error("Google email is not verified");
        }

        email    = !userInfoEmail.empty() ? userInfoEmail : tokenEmail;
        name     = str(googleResponse.body, "name");
        picture  = str(googleResponse.body, "picture");
        googleId = str(googleResponse.body, "sub");
        if (googleId.empty()) googleId = str(tokenInfo.body, "user_id");
        providerVerified = true;
        pulse::log::info("Google OAuth access token verified");
      } catch (const std::exception& googleErr) {
        pulse::log::warn("Google token verification rejected: {}", googleErr.what());
        throw std::runtime_error("Invalid authentication token");
      }
    }

    if (!providerVerified) {
      if (!idToken.empty())
        throw std::runtime_error("Firebase authentication is disabled");
      throw std::runtime_error("Invalid authentication token");
    }

    if (email.empty()) {
      throw std::runtime_error("Could not determine email from authentication token");
    }

    const std::string identifier = email;
    const std::string method = "email";
    pulse::log::info("Federated login identity verified");

    auto col = pulse::db::collection(pulse::models::user::kCollection);

    // Check if the user already exists.
    auto userOpt = pulse::models::user::findByAuthMethod(method, identifier);
    Json::Value user;

    if (!userOpt) {
      pulse::log::info("Creating user from verified federated identity");

      Json::Value via(Json::objectValue);
      via["verified"] = true;
      via["via"] = "google";
      user = createNewUser(method, identifier, via);

      // Update profile with Google info (only when not already set).
      bld::document setDoc;
      bool dirty = false;
      if (!picture.empty() && str(user, "avatar").empty()) { setDoc.append(kvp("avatar", picture)); user["avatar"] = picture; dirty = true; }
      if (!name.empty()    && str(user, "name").empty())   { setDoc.append(kvp("name", name));       user["name"] = name;       dirty = true; }
      if (!googleId.empty())                               { setDoc.append(kvp("googleId", googleId)); user["googleId"] = googleId; dirty = true; }
      if (dirty) {
        setDoc.append(kvp("updatedAt", nowDate()));
        auto oid = pulse::bsonjson::oid(docId(user));
        col.update_one(make_document(kvp("_id", oid)).view(),
                       make_document(kvp("$set", setDoc.extract())).view());
      }
    } else {
      user = updateUserAuthMethod(*userOpt, method, identifier);
      // Update Google profile info if newer (only when not already set).
      bld::document setDoc;
      bool dirty = false;
      if (!picture.empty() && str(user, "avatar").empty()) { setDoc.append(kvp("avatar", picture)); user["avatar"] = picture; dirty = true; }
      if (!name.empty()    && str(user, "name").empty())   { setDoc.append(kvp("name", name));       user["name"] = name;       dirty = true; }
      if (dirty) {
        setDoc.append(kvp("updatedAt", nowDate()));
        auto oid = pulse::bsonjson::oid(docId(user));
        col.update_one(make_document(kvp("_id", oid)).view(),
                       make_document(kvp("$set", setDoc.extract())).view());
      }
    }

    // Check if the user still needs to create a username.
    if (str(user, "username").empty()) {
      pulse::TempClaims tc;
      tc.userId = docId(user);
      tc.purpose = "username_creation";
      const std::string tempToken = pulse::jwt().generateTempToken(tc);

      Json::Value u(Json::objectValue);
      u["id"]         = docId(user);
      u["name"]       = user.get("name", Json::Value(Json::nullValue));
      u["email"]      = user.get("email", Json::Value(Json::nullValue));
      u["avatar"]     = user.get("avatar", Json::Value(Json::nullValue));
      u["isVerified"] = user.get("isVerified", false);

      Json::Value out(Json::objectValue);
      out["success"]          = true;
      out["nextStep"]         = "create_username";
      out["requiresUsername"] = true;
      out["tempToken"]        = tempToken;
      out["user"]             = u;
      out["message"]          = "Please create a username and password";
      return out;
    }

    // Create session — same tokens as the OTP flow.
    Json::Value sessionResult = createUserSession(user, deviceInfo, ipAddress);

    Json::Value out(Json::objectValue);
    out["success"]      = true;
    out["nextStep"]     = "complete";
    out["user"]         = sanitizeUser(user);
    out["accessToken"]  = sessionResult["tokens"]["accessToken"];
    out["refreshToken"] = sessionResult["tokens"]["refreshToken"];
    out["tokens"]       = sessionResult["tokens"];
    out["session"]      = sessionResult["session"];
    out["message"]      = "Authentication successful";
    return out;
  } catch (const std::exception& e) {
    pulse::log::error("Google login service error: {}", e.what());
    throw;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// verifyOTPAndAuth
// ─────────────────────────────────────────────────────────────────────────────
Json::Value AuthService::verifyOTPAndAuth(const std::string& identifier,
                                          const std::string& otp,
                                          const std::string& method,
                                          const DeviceInfo& deviceInfo,
                                          const std::string& ipAddress) {
  try {
    // Determine purpose the same way as initiate.
    std::optional<Json::Value> existingUser;
    if (method == "email") {
      existingUser = pulse::models::user::findByAuthMethod("email", identifier);
    } else if (method == "phone") {
      existingUser = pulse::models::user::findByAuthMethod("phone", identifier);
    }

    const std::string purpose = existingUser ? "login" : "signup";
    pulse::log::info("DEBUG: Verifying OTP with purpose: {}", purpose);

    // Verify OTP with correct purpose.
    custom_otp::verifyOTP(identifier, otp, purpose, ipAddress);

    // Check if user exists (fresh data).
    if (method == "email") {
      existingUser = pulse::models::user::findByAuthMethod("email", identifier);
    } else if (method == "phone") {
      existingUser = pulse::models::user::findByAuthMethod("phone", identifier);
    }

    Json::Value user;
    if (!existingUser) {
      user = createNewUser(method, identifier, Json::Value(Json::objectValue));
    } else {
      user = updateUserAuthMethod(*existingUser, method, identifier);
    }

    // Username creation flow.
    if (str(user, "username").empty()) {
      pulse::TempClaims tc;
      tc.userId = docId(user);
      tc.purpose = "username_creation";
      const std::string tempToken = pulse::jwt().generateTempToken(tc);

      Json::Value u(Json::objectValue);
      u["id"]         = docId(user);
      u["name"]       = user.get("name", Json::Value(Json::nullValue));
      u["email"]      = user.get("email", Json::Value(Json::nullValue));
      u["phone"]      = user.get("phone", Json::Value(Json::nullValue));
      u["isVerified"] = user.get("isVerified", false);

      Json::Value out(Json::objectValue);
      out["success"]   = true;
      out["nextStep"]  = "create_username";
      out["tempToken"] = tempToken;
      out["user"]      = u;
      out["message"]   = "Please create a username and password";
      return out;
    }

    Json::Value sessionResult = createUserSession(user, deviceInfo, ipAddress);

    Json::Value session(Json::objectValue);
    session["deviceId"]  = sessionResult["session"]["deviceId"];
    session["expiresAt"] = sessionResult["session"]["expiresAt"];

    Json::Value out(Json::objectValue);
    out["success"]  = true;
    out["nextStep"] = "complete";
    out["user"]     = sanitizeUser(user);
    out["tokens"]   = sessionResult["tokens"];
    out["session"]  = session;
    out["message"]  = "Authentication successful";
    return out;
  } catch (const std::exception& e) {
    pulse::log::error("OTP verification error: {}", e.what());
    throw;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// createNewUser
// ─────────────────────────────────────────────────────────────────────────────
Json::Value AuthService::createNewUser(const std::string& method,
                                       const std::string& identifier,
                                       const Json::Value& /*otpResult*/) {
  try {
    pulse::log::info("Creating new user for {} authentication", method);

    auto col = pulse::db::collection(pulse::models::user::kCollection);

    // name = email part before '@', or `user_<last4>` for phone.
    std::string name;
    if (method == "email") {
      auto at = identifier.find('@');
      name = at == std::string::npos ? identifier : identifier.substr(0, at);
    } else {
      std::string last4 = identifier.size() >= 4
                              ? identifier.substr(identifier.size() - 4)
                              : identifier;
      name = "user_" + last4;
    }

    // Build the exact userData object the JS passes to User.create(...).
    Json::Value userData(Json::objectValue);
    userData["name"] = name;

    Json::Value am(Json::objectValue);
    am["type"]       = method;
    am["identifier"] = identifier;  // keep '+' for phone (already present)
    am["verified"]   = true;
    am["verifiedAt"] = pulse::bsonjson::nowIso8601();
    Json::Value authMethods(Json::arrayValue);
    authMethods.append(am);
    userData["authMethods"] = authMethods;

    userData["isVerified"]  = true;
    userData["lastLoginAt"] = pulse::bsonjson::nowIso8601();

    Json::Value stats(Json::objectValue);
    stats["postsCount"]       = 0;
    stats["likesReceived"]    = 0;
    stats["commentsReceived"] = 0;
    userData["stats"] = stats;

    Json::Value settings(Json::objectValue);
    settings["radius"]             = 1000;
    settings["shareExactLocation"] = false;
    settings["anonymousPosting"]   = false;
    settings["pushNotifications"]  = true;
    settings["emailNotifications"] = true;
    userData["settings"] = settings;

    if (method == "email") {
      userData["email"] = identifier;
    } else if (method == "phone") {
      userData["phone"] = identifier;  // identifier already has '+'
    }

    // Apply the Mongoose schema defaults that User.create() would fill in —
    // crucially isActive:true (every subsequent findByAuthMethod / username
    // lookup filters on isActive:true), plus profile/privacy/role/etc.
    Json::Value defaulted = pulse::models::user::applyDefaults(userData);

    // Convert to BSON, coercing the known Date-typed fields to real BSON dates
    // (the rest pass through as their JSON types).
    static const std::vector<std::string> kDateKeys = {
        "lastLoginAt", "lastActive", "createdAt", "updatedAt"};
    auto isDateKey = [&](const std::string& k) {
      return std::find(kDateKeys.begin(), kDateKeys.end(), k) != kDateKeys.end();
    };

    const auto now = nowDate();
    bld::document doc;
    for (const auto& key : defaulted.getMemberNames()) {
      const Json::Value& v = defaulted[key];
      if (isDateKey(key) && v.isString()) {
        doc.append(kvp(key, now));
      } else if (key == "authMethods" && v.isArray()) {
        // Re-emit authMethods with verifiedAt as a BSON date.
        bld::array arr;
        for (const auto& entry : v) {
          bld::document e;
          for (const auto& ek : entry.getMemberNames()) {
            const Json::Value& ev = entry[ek];
            if (ek == "verifiedAt") {
              if (ev.isString()) e.append(kvp("verifiedAt", now));
              else               e.append(kvp("verifiedAt", bsoncxx::types::b_null{}));
            } else if (ev.isString())      e.append(kvp(ek, ev.asString()));
            else if (ev.isBool())          e.append(kvp(ek, ev.asBool()));
            else if (ev.isIntegral())      e.append(kvp(ek, static_cast<int64_t>(ev.asInt64())));
            else if (ev.isDouble())        e.append(kvp(ek, ev.asDouble()));
          }
          arr.append(e.extract());
        }
        doc.append(kvp("authMethods", arr.extract()));
      } else {
        // Generic JSON -> BSON for the remaining fields (objects/arrays/scalars).
        Json::Value wrap(Json::objectValue);
        wrap[key] = v;
        bsoncxx::document::value sub = pulse::bsonjson::fromJson(wrap);
        auto el = sub.view()[key];
        doc.append(kvp(key, el.get_value()));
      }
    }

    auto inserted = col.insert_one(doc.view());
    if (!inserted) throw std::runtime_error("Failed to create user");

    auto created = col.find_one(make_document(kvp("_id", inserted->inserted_id())).view());
    if (!created) throw std::runtime_error("Failed to load created user");

    Json::Value newUser = pulse::bsonjson::toJson(created->view());
    pulse::log::info("New user created");
    return newUser;
  } catch (const std::exception& e) {
    pulse::log::error("New user creation error: {}", e.what());
    throw;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// updateUserAuthMethod
// ─────────────────────────────────────────────────────────────────────────────
Json::Value AuthService::updateUserAuthMethod(Json::Value user,
                                              const std::string& method,
                                              const std::string& identifier) {
  try {
    auto col = pulse::db::collection(pulse::models::user::kCollection);
    auto oid = pulse::bsonjson::oid(docId(user));
    const auto now = nowDate();

      // Mutate only the matching array element. Replacing authMethods from a
      // stale user snapshot could discard a concurrent login method and also
      // rewrote unrelated verifiedAt timestamps.
      auto match = make_document(kvp("type", method),
                                 kvp("identifier", identifier));
      auto existing = col.update_one(
          make_document(kvp("_id", oid),
                        kvp("authMethods", make_document(
                            kvp("$elemMatch", match.view())))),
          make_document(kvp("$set", make_document(
              kvp("authMethods.$.verified", true),
              kvp("authMethods.$.verifiedAt", now),
              kvp("isVerified", true), kvp("lastLoginAt", now),
              kvp("updatedAt", now)))));

      if (!existing || existing->matched_count() == 0) {
        // Concurrent additions of different methods compose via $push; the
        // absence predicate prevents duplicate copies of the same pair.
        auto absent = make_document(kvp(
            "$not", make_document(kvp("$elemMatch", match.view()))));
        auto added = col.update_one(
            make_document(kvp("_id", oid), kvp("authMethods", absent.view())),
            make_document(
                kvp("$push", make_document(kvp("authMethods", make_document(
                    kvp("type", method), kvp("identifier", identifier),
                    kvp("verified", true), kvp("verifiedAt", now))))),
                kvp("$set", make_document(
                    kvp("isVerified", true), kvp("lastLoginAt", now),
                    kvp("updatedAt", now)))));
        if (!added || added->matched_count() == 0) {
          // Another request inserted this pair between the two operations.
          col.update_one(
              make_document(kvp("_id", oid),
                            kvp("authMethods", make_document(
                                kvp("$elemMatch", match.view())))),
              make_document(kvp("$set", make_document(
                  kvp("authMethods.$.verified", true),
                  kvp("authMethods.$.verifiedAt", now),
                  kvp("isVerified", true), kvp("lastLoginAt", now),
                  kvp("updatedAt", now)))));
        }
      }

      // Fill the canonical shortcut only while it is truly unset; never
      // overwrite a value established by another authentication request.
      const std::string shortcut = method == "email" ? "email" :
                                   method == "phone" ? "phone" : "";
      if (!shortcut.empty()) {
        bld::array unsetValues;
        unsetValues.append(make_document(
            kvp(shortcut, make_document(kvp("$exists", false)))));
        unsetValues.append(
            make_document(kvp(shortcut, bsoncxx::types::b_null{})));
        unsetValues.append(make_document(kvp(shortcut, "")));
        col.update_one(
            make_document(kvp("_id", oid),
                          kvp("$or", unsetValues.extract())),
            make_document(
                kvp("$set", make_document(kvp(shortcut, identifier)))));
      }

      pulse::log::info("Updated user authentication method");
      auto refreshed = col.find_one(make_document(kvp("_id", oid)));
      if (refreshed) return pulse::bsonjson::toJson(refreshed->view());
      return user;

  } catch (const std::exception& e) {
    pulse::log::error("Update user auth method error: {}", e.what());
    throw;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// createUsernameAndPassword
// ─────────────────────────────────────────────────────────────────────────────
Json::Value AuthService::createUsernameAndPassword(const std::string& tempToken,
                                                   const std::string& username,
                                                   const std::string& password,
                                                   const DeviceInfo& deviceInfo,
                                                   const std::string& ipAddress) {
  try {
    // Verify temp token.
    pulse::TempClaims decoded = pulse::jwt().verifyTempToken(tempToken);
    if (decoded.purpose != "username_creation") {
      throw std::runtime_error("Invalid temporary token purpose");
    }

    auto col = pulse::db::collection(pulse::models::user::kCollection);
    auto oid = pulse::bsonjson::oid(decoded.userId);

    // Get user.
    auto userOpt = col.find_one(make_document(kvp("_id", oid)).view());
    if (!userOpt) throw std::runtime_error("User not found");
    Json::Value user = pulse::bsonjson::toJson(userOpt->view());

    // Username already set?
    if (!str(user, "username").empty()) {
      throw std::runtime_error("Username already set for this user");
    }

    // Username availability: findOne({ username: lower, isActive: true }).
    std::string lowered = username;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    auto existing = col.find_one(
        make_document(kvp("username", lowered), kvp("isActive", true)).view());
    if (existing) throw std::runtime_error("Username already exists");

    // Validate username: /^[a-zA-Z0-9_]{3,20}$/
    static const std::regex kUsernameRe("^[a-zA-Z0-9_]{3,20}$");
    if (!std::regex_match(username, kUsernameRe)) {
      throw std::runtime_error(
          "Username must be 3-20 characters, alphanumeric and underscores only");
    }

    // bcrypt only considers the first 72 bytes; rejecting longer values avoids
    // accepting distinct-looking passwords that verify as the same secret.
    if (password.size() < 8 || password.size() > 72) {
      throw std::runtime_error("Password must be between 8 and 72 bytes long");
    }

    // Hash password (bcrypt cost 12) and set username.
    const std::string passwordHash = bcryptHash(password);
    const auto now = nowDate();

    auto setDoc = make_document(
        kvp("username", lowered),
        kvp("profile.displayName", lowered),
        kvp("passwordHash", passwordHash),
        kvp("updatedAt", now));
    try {
      auto updated = col.update_one(
          make_document(
              kvp("_id", oid), kvp("isActive", true),
              kvp("$or", make_array(
                  make_document(kvp("username", make_document(kvp("$exists", false)))),
                  make_document(kvp("username", bsoncxx::types::b_null{})),
                  make_document(kvp("username", ""))))),
          make_document(kvp("$set", setDoc.view())));
      if (!updated || updated->matched_count() != 1) {
        throw std::runtime_error("Username already set for this user");
      }
    } catch (const mongocxx::operation_exception& dbError) {
      if (dbError.code().value() == 11000)
        throw std::runtime_error("Username already exists");
      throw;
    }
    pulse::log::info("Username created for verified user");

    // Reflect the change locally for the session + response.
    user["username"]     = lowered;
    user["profile"]["displayName"] = lowered;
    user["passwordHash"] = passwordHash;

    Json::Value sessionResult = createUserSession(user, deviceInfo, ipAddress);

    Json::Value session(Json::objectValue);
    session["deviceId"]  = sessionResult["session"]["deviceId"];
    session["expiresAt"] = sessionResult["session"]["expiresAt"];

    Json::Value out(Json::objectValue);
    out["success"] = true;
    out["user"]    = sanitizeUser(user);
    out["tokens"]  = sessionResult["tokens"];
    out["session"] = session;
    out["message"] = "Account setup completed successfully";
    return out;
  } catch (const std::exception& e) {
    pulse::log::error("Username creation error: {}", e.what());
    throw;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// createUserSession
// ─────────────────────────────────────────────────────────────────────────────
Json::Value AuthService::createUserSession(const Json::Value& user,
                                           const DeviceInfo& deviceInfo,
                                           const std::string& ipAddress) {
  try {
    if (deviceInfo.deviceId.empty() || deviceInfo.deviceId.size() > 200) {
      throw std::invalid_argument("Invalid device ID");
    }
    if (!pulse::models::session::isValidPlatform(deviceInfo.platform)) {
      throw std::invalid_argument("Invalid device platform");
    }
    if (deviceName(deviceInfo).size() > 200 || appVersion(deviceInfo).size() > 64 ||
        osVersion(deviceInfo).size() > 128) {
      throw std::invalid_argument("Invalid device metadata");
    }
    auto col = pulse::db::collection(pulse::models::session::kCollection);
    auto userOid = pulse::bsonjson::oid(docId(user));

    // Generate tokens.
    const std::string accessToken = pulse::jwt().generateAccessToken(accessClaimsFor(user));

    pulse::RefreshClaims rc;
    rc.userId   = docId(user);
    rc.deviceId = deviceInfo.deviceId;
    rc.tokenId  = randomHex(16);
    const std::string refreshToken = pulse::jwt().generateRefreshToken(rc);

    // Session timestamps.
    const auto nowMs = pulse::bsonjson::nowMillis();
    const auto now = dateFromMillis(nowMs);
    const long long expiresAtMs = nowMs +
        static_cast<long long>(pulse::jwt().refreshTokenTtlSeconds()) * 1000LL;
    const auto expiresAt = dateFromMillis(expiresAtMs);

    // Deactivate other active sessions for this user+device.
    col.update_many(
        make_document(
            kvp("userId", userOid),
            kvp("deviceId", deviceInfo.deviceId),
            kvp("isActive", true))
            .view(),
        make_document(kvp("$set", make_document(kvp("isActive", false)))).view());

    // Create the new session (tokens stored as SHA-256 hashes).
    auto sessionDoc = make_document(
        kvp("userId", userOid),
        kvp("deviceId", deviceInfo.deviceId),
        kvp("deviceInfo", make_document(
                              kvp("platform", deviceInfo.platform),
                              kvp("deviceName", deviceName(deviceInfo)),
                              kvp("appVersion", appVersion(deviceInfo)),
                              kvp("osVersion", osVersion(deviceInfo)))),
        kvp("accessToken", hashToken(accessToken)),
        kvp("refreshToken", hashToken(refreshToken)),
        kvp("ipAddress", ipAddress),
        kvp("isActive", true),
        kvp("lastActivity", now),
        kvp("expiresAt", expiresAt),
        kvp("createdAt", now),
        kvp("updatedAt", now));

    auto inserted = col.insert_one(sessionDoc.view());
    if (!inserted) throw std::runtime_error("Failed to create session");

    pulse::log::info("User session created");

    Json::Value tokens(Json::objectValue);
    tokens["accessToken"]  = accessToken;
    tokens["refreshToken"] = refreshToken;
    tokens["tokenType"]    = "Bearer";
    tokens["expiresIn"]    = pulse::jwt().accessTokenTtlSeconds();

    Json::Value session(Json::objectValue);
    session["deviceId"]  = deviceInfo.deviceId;
    session["expiresAt"] = pulse::bsonjson::toJson(
                               make_document(kvp("expiresAt", expiresAt)).view())["expiresAt"];

    Json::Value out(Json::objectValue);
    out["tokens"]  = tokens;
    out["session"] = session;
    return out;
  } catch (const std::exception& e) {
    pulse::log::error("Session creation error: {}", e.what());
    throw;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// refreshAccessToken
// ─────────────────────────────────────────────────────────────────────────────
Json::Value AuthService::refreshAccessToken(const std::string& refreshToken) {
  try {
    // Verify refresh token.
    pulse::RefreshClaims decoded = pulse::jwt().verifyRefreshToken(refreshToken);

    auto sessions = pulse::db::collection(pulse::models::session::kCollection);
    auto users = pulse::db::collection(pulse::models::user::kCollection);

    auto userOid = pulse::bsonjson::oid(decoded.userId);

    // The user must still be active. REST and WebSocket authentication both
    // depend on refresh never minting fresh credentials for a deleted/banned
    // account.
    auto userOpt = users.find_one(
        make_document(kvp("_id", userOid), kvp("isActive", true)).view());
    if (!userOpt) throw std::runtime_error("User not found");
    Json::Value user = pulse::bsonjson::toJson(userOpt->view());

    // Generate new tokens.
    const std::string newAccessToken =
        pulse::jwt().generateAccessToken(accessClaimsFor(user));

    pulse::RefreshClaims rc;
    rc.userId   = decoded.userId;
    rc.deviceId = decoded.deviceId;
    rc.tokenId  = randomHex(16);
    const std::string newRefreshToken = pulse::jwt().generateRefreshToken(rc);

    // Atomically rotate the refresh hash. The old hash is part of the update
    // predicate, so only one of several concurrent reuses can succeed.
    auto setDoc = make_document(
        kvp("accessToken", hashToken(newAccessToken)),
        kvp("refreshToken", hashToken(newRefreshToken)),
        kvp("lastActivity", nowDate()),
        kvp("updatedAt", nowDate()));
    mongocxx::options::find_one_and_update rotateOpts{};
    rotateOpts.return_document(mongocxx::options::return_document::k_after);
    auto rotated = sessions.find_one_and_update(
        make_document(
            kvp("userId", userOid),
            kvp("deviceId", decoded.deviceId),
            kvp("refreshToken", hashToken(refreshToken)),
            kvp("isActive", true),
            kvp("expiresAt", make_document(kvp("$gt", nowDate())))),
        make_document(kvp("$set", setDoc.view())), rotateOpts);
    if (!rotated) throw std::runtime_error("Invalid or expired refresh token");

    pulse::log::info("User tokens refreshed");

    Json::Value tokens(Json::objectValue);
    tokens["accessToken"]  = newAccessToken;
    tokens["refreshToken"] = newRefreshToken;
    tokens["tokenType"]    = "Bearer";
    tokens["expiresIn"]    = pulse::jwt().accessTokenTtlSeconds();

    Json::Value out(Json::objectValue);
    out["success"] = true;
    out["tokens"]  = tokens;
    return out;
  } catch (const std::exception& e) {
    pulse::log::error("Token refresh error: {}", e.what());
    throw;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// checkUsernameAvailability
// ─────────────────────────────────────────────────────────────────────────────
Json::Value AuthService::checkUsernameAvailability(const std::string& username) {
  try {
    static const std::regex kUsernameRe("^[a-zA-Z0-9_]{3,20}$");
    if (!std::regex_match(username, kUsernameRe)) {
      Json::Value out(Json::objectValue);
      out["success"]   = false;
      out["available"] = false;
      out["error"] =
          "Username must be 3-20 characters, alphanumeric and underscores only";
      return out;
    }

    auto col = pulse::db::collection(pulse::models::user::kCollection);
    std::string lowered = username;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    auto existing = col.find_one(
        make_document(kvp("username", lowered), kvp("isActive", true)).view());

    const bool taken = static_cast<bool>(existing);
    Json::Value out(Json::objectValue);
    out["success"]   = true;
    out["available"] = !taken;
    out["username"]  = username;
    out["message"]   = taken ? "Username is already taken" : "Username is available";
    return out;
  } catch (const std::exception& e) {
    pulse::log::error("Username check error: {}", e.what());
    throw;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// resendOTP
// ─────────────────────────────────────────────────────────────────────────────
Json::Value AuthService::resendOTP(const std::string& identifier,
                                   const std::string& method,
                                   const std::string& ipAddress) {
  try {
    std::optional<Json::Value> existingUser;
    if (method == "email") {
      existingUser = pulse::models::user::findByAuthMethod("email", identifier);
    } else if (method == "phone") {
      existingUser = pulse::models::user::findByAuthMethod("phone", identifier);
    }

    const std::string purpose = existingUser ? "login" : "signup";
    const std::string existingUserId = existingUser ? docId(*existingUser) : "";

    custom_otp::resendOTP(identifier, method, purpose, existingUserId, ipAddress);

    Json::Value out(Json::objectValue);
    out["success"]    = true;
    out["method"]     = method;
    out["identifier"] = identifier;
    out["message"]    = "If the identifier is valid, an OTP has been sent";
    return out;
  } catch (const std::exception& e) {
    pulse::log::error("Resend OTP error: {}", e.what());
    throw;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// getCurrentUser
// ─────────────────────────────────────────────────────────────────────────────
Json::Value AuthService::getCurrentUser(const std::string& userId) {
  try {
    auto col = pulse::db::collection(pulse::models::user::kCollection);
    auto oid = pulse::bsonjson::tryOid(userId);
    if (!oid) throw std::runtime_error("User not found");

    // .select('-passwordHash') — exclude passwordHash from the projection.
    mongocxx::options::find opts;
    opts.projection(make_document(kvp("passwordHash", 0)));
    auto userOpt = col.find_one(make_document(kvp("_id", *oid)).view(), opts);
    if (!userOpt) throw std::runtime_error("User not found");

    Json::Value user = pulse::bsonjson::toJson(userOpt->view());

    Json::Value out(Json::objectValue);
    out["success"] = true;
    out["user"]    = sanitizeUser(user);
    return out;
  } catch (const std::exception& e) {
    pulse::log::error("Get current user error: {}", e.what());
    throw;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// logoutUser
// ─────────────────────────────────────────────────────────────────────────────
Json::Value AuthService::logoutUser(const std::string& userId,
                                    const std::string& deviceId,
                                    const std::string& accessToken) {
  try {
    auto col = pulse::db::collection(pulse::models::session::kCollection);
    auto oid = pulse::bsonjson::oid(userId);

    // Deactivate the active sessions for this user+device.
    col.update_many(
        make_document(
            kvp("userId", oid),
            kvp("deviceId", deviceId),
            kvp("isActive", true))
            .view(),
        make_document(kvp("$set", make_document(
                                      kvp("isActive", false),
                                      kvp("updatedAt", nowDate()))))
            .view());

    // Revoke the access token immediately for its configured maximum lifetime.
    if (!accessToken.empty()) {
      try {
        pulse::cache().set("revoked_token:" + hashToken(accessToken), "1",
                           pulse::jwt().accessTokenTtlSeconds());
      } catch (const std::exception& e) {
        pulse::log::warn("Token revocation cache write failed: {}", e.what());
      }
    }

    pulse::log::info("User session logged out");

    Json::Value out(Json::objectValue);
    out["success"] = true;
    out["message"] = "Logged out successfully";
    return out;
  } catch (const std::exception& e) {
    pulse::log::error("Logout error: {}", e.what());
    throw;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// sanitizeUser
// ─────────────────────────────────────────────────────────────────────────────
Json::Value AuthService::sanitizeUser(const Json::Value& user) {
  // Returns the JS-listed fields, preserving nulls for absent fields.
  Json::Value out(Json::objectValue);
  out["id"]         = user.get("_id", Json::Value(Json::nullValue));
  out["username"]   = user.get("username", Json::Value(Json::nullValue));
  out["name"]       = user.get("name", Json::Value(Json::nullValue));
  out["email"]      = user.get("email", Json::Value(Json::nullValue));
  out["phone"]      = user.get("phone", Json::Value(Json::nullValue));

  // Avatar/displayName live under `profile` in the data model; the original
  // sanitizeUser only exposed a flat top-level `avatar`, so clients that read
  // `profile.avatar` (the actual location) got null on login and rendered a
  // placeholder until a later /users fetch populated it. Expose `profile` and
  // fall the flat `avatar` back to `profile.avatar` so the client has it
  // immediately. (Client reads `profile?.avatar || avatar`.)
  const Json::Value& profile = user["profile"];
  out["profile"] = profile.isObject() ? profile : Json::Value(Json::nullValue);
  Json::Value avatar = user.get("avatar", Json::Value(Json::nullValue));
  if ((avatar.isNull() || (avatar.isString() && avatar.asString().empty())) &&
      profile.isObject() && profile.isMember("avatar")) {
    avatar = profile["avatar"];
  }
  out["avatar"]     = avatar;

  out["bio"]        = user.get("bio", Json::Value(Json::nullValue));
  out["isVerified"] = user.get("isVerified", Json::Value(Json::nullValue));
  out["settings"]   = user.get("settings", Json::Value(Json::nullValue));
  out["stats"]      = user.get("stats", Json::Value(Json::nullValue));
  out["createdAt"]  = user.get("createdAt", Json::Value(Json::nullValue));
  out["updatedAt"]  = user.get("updatedAt", Json::Value(Json::nullValue));
  return out;
}

}  // namespace pulse::services
