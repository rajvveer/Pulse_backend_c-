// auth_controller.cc — implementation of AuthController (ports authController.js).
//
// Each handler mirrors the corresponding Express handler: same input validation
// (same checks, error codes, messages, status codes), same delegation to
// pulse::services::authService, and the same try/catch -> HTTP error mapping.
#include "pulse/controllers/auth_controller.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <stdexcept>
#include <string>

#include "pulse/bson_json.hpp"
#include "pulse/http_response.hpp"
#include "pulse/logger.hpp"
#include "pulse/filters/rate_limit_filters.hpp"
#include "pulse/models/session.hpp"
#include "pulse/services/auth_service.hpp"

using namespace pulse::controllers;
namespace http = pulse::http;
using pulse::services::DeviceInfo;
using pulse::services::authService;

// ---------------------------------------------------------------------------
// File-local validation helpers (ported from authController.js inline helpers).
// They throw std::runtime_error with the EXACT JS message so callers translate
// them to HTTP errors identically to the Express try/catch blocks.
// ---------------------------------------------------------------------------
namespace {

// validatePhoneNumber(phone): strip non-digits; accept 10 digits starting 6-9,
// or 12 digits "91" + 10-digit (6-9). Returns the bare 10-digit number.
std::string validatePhoneNumber(const std::string& phone) {
  std::string cleanPhone;
  for (char c : phone) {
    if (std::isdigit(static_cast<unsigned char>(c))) cleanPhone.push_back(c);
  }
  // ^[6-9]\d{9}$
  if (cleanPhone.size() == 10 && cleanPhone[0] >= '6' && cleanPhone[0] <= '9') {
    bool rest = true;
    for (size_t i = 1; i < cleanPhone.size(); ++i)
      if (!std::isdigit(static_cast<unsigned char>(cleanPhone[i]))) { rest = false; break; }
    if (rest) return cleanPhone;
  }
  // ^91[6-9]\d{9}$
  if (cleanPhone.size() == 12 && cleanPhone[0] == '9' && cleanPhone[1] == '1' &&
      cleanPhone[2] >= '6' && cleanPhone[2] <= '9') {
    bool rest = true;
    for (size_t i = 3; i < cleanPhone.size(); ++i)
      if (!std::isdigit(static_cast<unsigned char>(cleanPhone[i]))) { rest = false; break; }
    if (rest) return cleanPhone.substr(2);
  }
  throw std::runtime_error(
      "Please enter a valid Indian mobile number (10 digits starting with 6-9)");
}

// validateEmail(email): /^[^\s@]+@[^\s@]+\.[^\s@]+$/, then lowercase + trim.
std::string validateEmail(const std::string& email) {
  if (email.empty() || email.size() > 254) {
    throw std::runtime_error("Please enter a valid email address");
  }
  // Manual check mirroring the regex: non-empty local part (no space/@),
  // single '@', domain with a '.' and non-space/@ segments around it.
  auto isBad = [](char c) { return c == ' ' || c == '\t' || c == '\n' ||
                                   c == '\r' || c == '\f' || c == '\v' || c == '@'; };
  size_t at = std::string::npos;
  for (size_t i = 0; i < email.size(); ++i) {
    if (email[i] == '@') {
      if (at != std::string::npos) { at = std::string::npos; break; }  // second '@'
      at = i;
    }
  }
  bool valid = false;
  if (at != std::string::npos && at > 0 && at <= 64 &&
      at + 1 < email.size() && email.size() - at - 1 <= 253) {
    // local part: [^\s@]+
    bool localOk = true;
    for (size_t i = 0; i < at; ++i) if (isBad(email[i])) { localOk = false; break; }
    // domain part must be: [^\s@]+\.[^\s@]+
    std::string domain = email.substr(at + 1);
    size_t dot = domain.rfind('.');
    bool domainOk = false;
    if (dot != std::string::npos && dot > 0 && dot + 1 < domain.size()) {
      domainOk = true;
      for (char c : domain) if (isBad(c)) { domainOk = false; break; }
    }
    valid = localOk && domainOk;
  }
  if (!valid) {
    throw std::runtime_error("Please enter a valid email address");
  }
  // toLowerCase().trim()
  std::string out = email;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  size_t start = out.find_first_not_of(" \t\n\r\f\v");
  size_t end = out.find_last_not_of(" \t\n\r\f\v");
  if (start == std::string::npos) return "";
  return out.substr(start, end - start + 1);
}

// buildE164Phone(identifier): "+91" + validatePhoneNumber(identifier).
std::string buildE164Phone(const std::string& identifier) {
  return "+91" + validatePhoneNumber(identifier);
}

// req.ip || '127.0.0.1'
std::string clientIp(const drogon::HttpRequestPtr& req) {
  std::string ip = pulse::filters::clientIp(req);
  if (ip.empty()) return "127.0.0.1";
  return ip;
}

// Read a string field from a JSON body object; returns "" if missing/not string.
std::string jstr(const Json::Value& body, const char* key) {
  if (body.isObject() && body.isMember(key) && body[key].isString())
    return body[key].asString();
  return "";
}

// True if the field is present and "truthy" as the JS `!field` checks would see
// (i.e. present, a non-empty string, or any non-null/non-false value).
bool jhas(const Json::Value& body, const char* key) {
  if (!body.isObject() || !body.isMember(key)) return false;
  const Json::Value& v = body[key];
  if (v.isNull()) return false;
  if (v.isString()) return !v.asString().empty();
  if (v.isBool()) return v.asBool();
  return true;
}

bool validDeviceInfo(const DeviceInfo& info) {
  const auto optionalWithin = [](const std::optional<std::string>& value,
                                 std::size_t max) {
    return !value || value->size() <= max;
  };
  return !info.deviceId.empty() && info.deviceId.size() <= 200 &&
         pulse::models::session::isValidPlatform(info.platform) &&
         optionalWithin(info.deviceName, 200) &&
         optionalWithin(info.appVersion, 64) &&
         optionalWithin(info.osVersion, 128);
}

// std::string::contains substitute (pre-C++23).
bool contains(const std::string& s, const char* sub) {
  return s.find(sub) != std::string::npos;
}

}  // namespace

// ---------------------------------------------------------------------------
// POST /api/v1/auth/initiate
// ---------------------------------------------------------------------------
void AuthController::initiateAuth(const HttpRequestPtr& req,
                                  std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    auto bodyPtr = req->getJsonObject();
    Json::Value body = bodyPtr ? *bodyPtr : Json::Value(Json::objectValue);

    std::string method = jstr(body, "method");
    std::string identifier = jstr(body, "identifier");

    if (!jhas(body, "method") || !jhas(body, "identifier")) {
      callback(http::error(drogon::k400BadRequest,
                           "Method and identifier are required",
                           "MISSING_REQUIRED_FIELDS"));
      return;
    }

    if (method != "email" && method != "phone") {
      callback(http::error(drogon::k400BadRequest,
                           "Method must be either \"email\" or \"phone\"",
                           "INVALID_METHOD"));
      return;
    }

    std::string processedIdentifier;
    std::string displayIdentifier;

    if (method == "phone") {
      processedIdentifier = buildE164Phone(identifier);
      displayIdentifier = processedIdentifier;
    } else {  // email
      processedIdentifier = validateEmail(identifier);
      displayIdentifier = processedIdentifier;
    }

    DeviceInfo deviceInfo;
    deviceInfo.deviceId = jhas(body, "deviceId")
                              ? jstr(body, "deviceId")
                              : ("web-" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count()));
    deviceInfo.platform = jhas(body, "platform") ? jstr(body, "platform") : "web";
    deviceInfo.deviceName = jhas(body, "deviceName") ? jstr(body, "deviceName") : "Unknown Device";
    deviceInfo.appVersion = jhas(body, "appVersion") ? jstr(body, "appVersion") : "1.0.0";

    if (deviceInfo.deviceId.empty()) {
      callback(http::error(drogon::k400BadRequest,
                           "Device ID is required",
                           "MISSING_DEVICE_ID"));
      return;
    }
    if (!validDeviceInfo(deviceInfo)) {
      callback(http::error(drogon::k400BadRequest,
                           "Invalid device information",
                           "INVALID_DEVICE_INFO"));
      return;
    }

    Json::Value result = authService().initiateAuth(
        processedIdentifier, method, deviceInfo, clientIp(req));

    result["identifier"] = displayIdentifier;

    callback(http::json(drogon::k200OK, result));

  } catch (const std::exception& error) {
    pulse::log::error("Auth initiation error: {}", error.what());
    callback(http::error(drogon::k400BadRequest,
                         "Unable to start authentication",
                         "AUTH_INITIATION_FAILED"));
  }
}

// ---------------------------------------------------------------------------
// POST /api/v1/auth/verify-otp
// ---------------------------------------------------------------------------
void AuthController::verifyOTP(const HttpRequestPtr& req,
                               std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    auto bodyPtr = req->getJsonObject();
    Json::Value body = bodyPtr ? *bodyPtr : Json::Value(Json::objectValue);

    std::string identifier = jstr(body, "identifier");
    std::string otp = jstr(body, "otp");
    std::string method = jstr(body, "method");
    std::string deviceId = jstr(body, "deviceId");

    if (!jhas(body, "identifier") || !jhas(body, "otp") || !jhas(body, "method") ||
        !jhas(body, "deviceId")) {
      callback(http::error(drogon::k400BadRequest,
                           "Identifier, OTP, method, and deviceId are required",
                           "MISSING_REQUIRED_FIELDS"));
      return;
    }

    if (method != "email" && method != "phone") {
      callback(http::error(drogon::k400BadRequest,
                           "Method must be either \"email\" or \"phone\"",
                           "INVALID_METHOD"));
      return;
    }

    std::string lookupIdentifier;
    if (method == "phone") {
      lookupIdentifier = buildE164Phone(identifier);
    } else {  // email
      lookupIdentifier = validateEmail(identifier);
    }

    DeviceInfo deviceInfo;
    deviceInfo.deviceId = deviceId;
    deviceInfo.platform = jhas(body, "platform") ? jstr(body, "platform") : "web";
    if (!validDeviceInfo(deviceInfo)) {
      callback(http::error(drogon::k400BadRequest,
                           "Invalid device information",
                           "INVALID_DEVICE_INFO"));
      return;
    }

    Json::Value result = authService().verifyOTPAndAuth(
        lookupIdentifier, otp, method, deviceInfo, clientIp(req));

    callback(http::json(drogon::k200OK, result));

  } catch (const std::exception& error) {
    std::string msg = error.what();
    pulse::log::error("OTP verification error: {}", msg);

    if (!msg.empty() && (contains(msg, "Invalid") || contains(msg, "expired"))) {
      callback(http::error(drogon::k401Unauthorized, msg, "INVALID_OTP"));
      return;
    }

    callback(http::error(drogon::k400BadRequest,
                         "OTP verification failed",
                         "OTP_VERIFICATION_FAILED"));
  }
}

// ---------------------------------------------------------------------------
// POST /api/v1/auth/create-username
// ---------------------------------------------------------------------------
void AuthController::createUsername(const HttpRequestPtr& req,
                                    std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    auto bodyPtr = req->getJsonObject();
    Json::Value body = bodyPtr ? *bodyPtr : Json::Value(Json::objectValue);

    std::string tempToken = jstr(body, "tempToken");
    std::string username = jstr(body, "username");
    std::string password = jstr(body, "password");
    std::string deviceId = jstr(body, "deviceId");

    if (!jhas(body, "tempToken") || !jhas(body, "username") || !jhas(body, "password") ||
        !jhas(body, "deviceId")) {
      callback(http::error(drogon::k400BadRequest,
                           "Temporary token, username, password, and deviceId are required",
                           "MISSING_REQUIRED_FIELDS"));
      return;
    }

    DeviceInfo deviceInfo;
    deviceInfo.deviceId = deviceId;
    deviceInfo.platform = jhas(body, "platform") ? jstr(body, "platform") : "web";
    if (!validDeviceInfo(deviceInfo)) {
      callback(http::error(drogon::k400BadRequest,
                           "Invalid device information",
                           "INVALID_DEVICE_INFO"));
      return;
    }

    Json::Value result = authService().createUsernameAndPassword(
        tempToken, username, password, deviceInfo, clientIp(req));

    callback(http::json(drogon::k200OK, result));

  } catch (const std::exception& error) {
    std::string msg = error.what();
    pulse::log::error("Username creation error: {}", msg);

    if (!msg.empty() && contains(msg, "Username must be")) {
      callback(http::error(drogon::k400BadRequest, msg, "INVALID_USERNAME"));
      return;
    }

    if (!msg.empty() && contains(msg, "Password must be")) {
      callback(http::error(drogon::k400BadRequest, msg, "INVALID_PASSWORD"));
      return;
    }

    if (!msg.empty() && contains(msg, "already exists")) {
      callback(http::error(drogon::k409Conflict, msg, "USERNAME_EXISTS"));
      return;
    }

    if (!msg.empty() && contains(msg, "already set")) {
      callback(http::error(drogon::k400BadRequest, msg, "USERNAME_ALREADY_SET"));
      return;
    }

    callback(http::error(drogon::k500InternalServerError,
                         "Internal server error", "INTERNAL_ERROR"));
  }
}

// ---------------------------------------------------------------------------
// POST /api/v1/auth/refresh-token
// ---------------------------------------------------------------------------
void AuthController::refreshToken(const HttpRequestPtr& req,
                                  std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    auto bodyPtr = req->getJsonObject();
    Json::Value body = bodyPtr ? *bodyPtr : Json::Value(Json::objectValue);

    std::string refreshTokenValue = jstr(body, "refreshToken");

    if (!jhas(body, "refreshToken")) {
      callback(http::error(drogon::k400BadRequest,
                           "Refresh token is required",
                           "MISSING_REFRESH_TOKEN"));
      return;
    }

    Json::Value result = authService().refreshAccessToken(refreshTokenValue);
    callback(http::json(drogon::k200OK, result));

  } catch (const std::exception& error) {
    std::string msg = error.what();
    pulse::log::error("Token refresh error: {}", msg);

    if (!msg.empty() && (contains(msg, "Invalid") || contains(msg, "expired"))) {
      callback(http::error(drogon::k401Unauthorized, msg, "INVALID_REFRESH_TOKEN"));
      return;
    }

    callback(http::error(drogon::k500InternalServerError,
                         "Internal server error", "INTERNAL_ERROR"));
  }
}

// ---------------------------------------------------------------------------
// POST /api/v1/auth/resend-otp
// ---------------------------------------------------------------------------
void AuthController::resendOTP(const HttpRequestPtr& req,
                               std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    auto bodyPtr = req->getJsonObject();
    Json::Value body = bodyPtr ? *bodyPtr : Json::Value(Json::objectValue);

    std::string identifier = jstr(body, "identifier");
    std::string method = jstr(body, "method");

    if (!jhas(body, "identifier") || !jhas(body, "method")) {
      callback(http::error(drogon::k400BadRequest,
                           "Identifier and method are required",
                           "MISSING_REQUIRED_FIELDS"));
      return;
    }

    if (method != "email" && method != "phone") {
      callback(http::error(drogon::k400BadRequest,
                           "Method must be either \"email\" or \"phone\"",
                           "INVALID_METHOD"));
      return;
    }

    std::string processedIdentifier;
    if (method == "phone") {
      processedIdentifier = buildE164Phone(identifier);
    } else {  // email
      processedIdentifier = validateEmail(identifier);
    }

    Json::Value result = authService().resendOTP(processedIdentifier, method, clientIp(req));

    callback(http::json(drogon::k200OK, result));

  } catch (const std::exception& error) {
    pulse::log::error("Resend OTP error: {}", error.what());
    callback(http::error(drogon::k400BadRequest,
                         "Unable to resend OTP",
                         "RESEND_OTP_FAILED"));
  }
}

// ---------------------------------------------------------------------------
// GET /api/v1/auth/check-username
// ---------------------------------------------------------------------------
void AuthController::checkUsername(const HttpRequestPtr& req,
                                   std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    std::string username = req->getParameter("username");

    if (username.empty()) {
      callback(http::error(drogon::k400BadRequest,
                           "Username is required",
                           "MISSING_USERNAME"));
      return;
    }

    Json::Value result = authService().checkUsernameAvailability(username);
    callback(http::json(drogon::k200OK, result));

  } catch (const std::exception& error) {
    pulse::log::error("Username check error: {}", error.what());
    callback(http::error(drogon::k500InternalServerError,
                         "Internal server error", "INTERNAL_ERROR"));
  }
}

// ---------------------------------------------------------------------------
// GET /api/v1/auth/me  (AuthFilter)
// ---------------------------------------------------------------------------
void AuthController::getCurrentUser(const HttpRequestPtr& req,
                                    std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    Json::Value user = req->getAttributes()->get<Json::Value>("user");
    Json::Value result = authService().getCurrentUser(user["userId"].asString());
    callback(http::json(drogon::k200OK, result));
  } catch (const std::exception& error) {
    pulse::log::error("Get current user error: {}", error.what());
    callback(http::error(drogon::k500InternalServerError,
                         "Internal server error", "INTERNAL_ERROR"));
  }
}

// ---------------------------------------------------------------------------
// POST /api/v1/auth/logout  (AuthFilter)
// ---------------------------------------------------------------------------
void AuthController::logout(const HttpRequestPtr& req,
                            std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    auto bodyPtr = req->getJsonObject();
    Json::Value body = bodyPtr ? *bodyPtr : Json::Value(Json::objectValue);

    std::string deviceId = jstr(body, "deviceId");
    Json::Value user = req->getAttributes()->get<Json::Value>("user");

    if (!jhas(body, "deviceId")) {
      callback(http::error(drogon::k400BadRequest,
                           "Device ID is required",
                           "MISSING_DEVICE_ID"));
      return;
    }

    // Pass the raw access token so it can be revoked immediately:
    // req.headers.authorization?.split(' ')[1] || null
    std::string accessToken;
    std::string authHeader = req->getHeader("authorization");
    if (!authHeader.empty()) {
      size_t sp = authHeader.find(' ');
      if (sp != std::string::npos) accessToken = authHeader.substr(sp + 1);
    }

    Json::Value result =
        authService().logoutUser(user["userId"].asString(), deviceId, accessToken);
    callback(http::json(drogon::k200OK, result));

  } catch (const std::exception& error) {
    pulse::log::error("Logout error: {}", error.what());
    callback(http::error(drogon::k500InternalServerError,
                         "Internal server error", "INTERNAL_ERROR"));
  }
}

// ---------------------------------------------------------------------------
// POST /api/v1/auth/firebase-login
// ---------------------------------------------------------------------------
void AuthController::firebaseLogin(const HttpRequestPtr& req,
                                   std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    auto bodyPtr = req->getJsonObject();
    Json::Value body = bodyPtr ? *bodyPtr : Json::Value(Json::objectValue);

    std::string idToken = jstr(body, "idToken");
    std::string googleAccessToken = jstr(body, "accessToken");
    std::string deviceId = jstr(body, "deviceId");

    // Need at least one token and a device ID:
    // (!idToken && !googleAccessToken) || !deviceId
    if ((!jhas(body, "idToken") && !jhas(body, "accessToken")) || !jhas(body, "deviceId")) {
      callback(http::error(drogon::k400BadRequest,
                           "Authentication token and Device ID are required",
                           "MISSING_FIELDS"));
      return;
    }

    DeviceInfo deviceInfo;
    deviceInfo.deviceId = deviceId;
    deviceInfo.platform = jhas(body, "platform") ? jstr(body, "platform") : "web";
    deviceInfo.deviceName = jhas(body, "deviceName") ? jstr(body, "deviceName") : "Unknown";
    if (!validDeviceInfo(deviceInfo)) {
      callback(http::error(drogon::k400BadRequest,
                           "Invalid device information",
                           "INVALID_DEVICE_INFO"));
      return;
    }

    // Pass extra info from frontend as fallback for token verification.
    Json::Value extraInfo(Json::objectValue);
    extraInfo["email"] = body.get("email", Json::Value::nullSingleton());
    extraInfo["name"] = body.get("name", Json::Value::nullSingleton());
    extraInfo["picture"] = body.get("picture", Json::Value::nullSingleton());
    extraInfo["googleId"] = body.get("googleId", Json::Value::nullSingleton());

    Json::Value result = authService().loginWithFirebase(
        idToken, googleAccessToken, deviceInfo, clientIp(req), extraInfo);

    callback(http::json(drogon::k200OK, result));

  } catch (const std::exception& error) {
    std::string msg = error.what();
    pulse::log::error("Google login controller error: {}", msg);

    if (contains(msg, "expired")) {
      callback(http::error(drogon::k401Unauthorized,
                           "Session expired. Please login again.",
                           "TOKEN_EXPIRED"));
      return;
    }

    callback(http::error(drogon::k401Unauthorized,
                         "Authentication failed", "AUTH_FAILED"));
  }
}

// ---------------------------------------------------------------------------
// GET /api/v1/auth/test  (inline handler in the Express router)
// ---------------------------------------------------------------------------
void AuthController::test(const HttpRequestPtr& req,
                          std::function<void(const HttpResponsePtr&)>&& callback) {
  (void)req;
  Json::Value body(Json::objectValue);
  body["success"] = true;
  body["message"] = "Auth API is working!";
  body["timestamp"] = pulse::bsonjson::nowIso8601();

  Json::Value endpoints(Json::objectValue);
  Json::Value publicEndpoints(Json::arrayValue);
  publicEndpoints.append("POST /api/v1/auth/initiate - Start email/phone authentication");
  publicEndpoints.append("POST /api/v1/auth/verify-otp - Verify OTP code");
  publicEndpoints.append("POST /api/v1/auth/create-username - Create username/password (new users)");
  publicEndpoints.append("POST /api/v1/auth/refresh-token - Refresh access token");
  publicEndpoints.append("POST /api/v1/auth/resend-otp - Resend OTP code");
  publicEndpoints.append("GET /api/v1/auth/check-username - Check username availability");
  publicEndpoints.append("POST /api/v1/auth/firebase-login - Login with Firebase ID token");
  endpoints["public"] = publicEndpoints;

  Json::Value protectedEndpoints(Json::arrayValue);
  protectedEndpoints.append("GET /api/v1/auth/me - Get current user info");
  protectedEndpoints.append("POST /api/v1/auth/logout - Logout from device");
  endpoints["protected"] = protectedEndpoints;
  body["endpoints"] = endpoints;

  Json::Value features(Json::arrayValue);
  features.append("\xF0\x9F\x93\xA7 Email OTP via Gmail SMTP");
  features.append("\xF0\x9F\x93\xB1 SMS OTP via MSG91 (India)");
  features.append("\xF0\x9F\x94\x90 JWT access + refresh tokens");
  features.append("\xF0\x9F\x92\xBE Multi-device session management");
  features.append("\xF0\x9F\x94\x92 Rate limiting & security");
  features.append("\xF0\x9F\x91\xA4 Username/password creation");
  features.append("\xE2\x9C\x85 Account verification");
  features.append("\xF0\x9F\x94\xA5 Firebase Integration");
  body["features"] = features;

  callback(http::json(drogon::k200OK, body));
}
