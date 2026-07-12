// auth_service.hpp — C++ port of src/services/authService.js.
//
// Central authentication service: email/phone OTP signup+login, Firebase /
// Google OAuth login, username+password creation, session management, token
// refresh, logout, and current-user lookup. Mirrors the JS module singleton
// (`module.exports = new AuthService()`) via AuthService::instance() /
// pulse::services::authService().
//
// Parity contract (kept 1:1 with the JS source):
//   * phone normalization: strip non-digits, accept 10 digits (prepend "91")
//     or 12 digits starting "91"; lookups use the "+<digits>" E.164 form.
//   * userExists drives purpose: "login" when found, else "signup".
//   * tokens: access (15m) + refresh (7d); sessions store SHA-256 hashes of both.
//   * Redis: logout writes `revoked_token:<sha256(accessToken)>` with TTL 900.
//
// Each method returns the exact JS response object as a Json::Value and THROWS
// std::runtime_error (message == the JS `throw new Error(...)` text) on failure,
// so controllers translate thrown errors to HTTP responses just as the Express
// route handlers wrapped these service calls.
#pragma once
#include <string>
#include <optional>
#include <json/json.h>

namespace pulse::services {

// Device metadata passed from the controller (req body / headers). Optional
// fields fall back to the schema defaults inside createUserSession, matching the
// JS `deviceInfo.deviceName || 'Unknown Device'` style defaulting.
struct DeviceInfo {
  std::string deviceId;
  std::string platform;
  std::optional<std::string> deviceName;
  std::optional<std::string> appVersion;
  std::optional<std::string> osVersion;
};

class AuthService {
 public:
  // Process-wide singleton, mirroring the exported JS instance.
  static AuthService& instance();

  // initiateAuth(identifier, method, deviceInfo, ipAddress)
  // -> { success, method, identifier, nextStep:'verify_otp', purpose,
  //      userExists, message }
  Json::Value initiateAuth(const std::string& identifier,
                           const std::string& method,
                           const DeviceInfo& deviceInfo,
                           const std::string& ipAddress);

  // loginWithFirebase(idToken, accessToken, deviceInfo, ipAddress, extraInfo={})
  // -> create_username or complete payload (see .cc).
  Json::Value loginWithFirebase(const std::string& idToken,
                                const std::string& accessToken,
                                const DeviceInfo& deviceInfo,
                                const std::string& ipAddress,
                                const Json::Value& extraInfo = Json::Value(Json::objectValue));

  // verifyOTPAndAuth(identifier, otp, method, deviceInfo, ipAddress)
  Json::Value verifyOTPAndAuth(const std::string& identifier,
                               const std::string& otp,
                               const std::string& method,
                               const DeviceInfo& deviceInfo,
                               const std::string& ipAddress);

  // createNewUser(method, identifier, otpResult) -> the created User document.
  Json::Value createNewUser(const std::string& method,
                            const std::string& identifier,
                            const Json::Value& otpResult);

  // updateUserAuthMethod(user, method, identifier) — mutates + persists user.
  // `user` is the fetched User document; the refreshed document is returned.
  Json::Value updateUserAuthMethod(Json::Value user,
                                   const std::string& method,
                                   const std::string& identifier);

  // createUsernameAndPassword(tempToken, username, password, deviceInfo, ip)
  Json::Value createUsernameAndPassword(const std::string& tempToken,
                                        const std::string& username,
                                        const std::string& password,
                                        const DeviceInfo& deviceInfo,
                                        const std::string& ipAddress);

  // createUserSession(user, deviceInfo, ipAddress)
  // -> { tokens:{accessToken,refreshToken,tokenType,expiresIn},
  //      session:{deviceId,expiresAt} }
  Json::Value createUserSession(const Json::Value& user,
                                const DeviceInfo& deviceInfo,
                                const std::string& ipAddress);

  // refreshAccessToken(refreshToken) -> { success, tokens }
  Json::Value refreshAccessToken(const std::string& refreshToken);

  // checkUsernameAvailability(username)
  Json::Value checkUsernameAvailability(const std::string& username);

  // resendOTP(identifier, method, ipAddress)
  Json::Value resendOTP(const std::string& identifier,
                        const std::string& method,
                        const std::string& ipAddress);

  // getCurrentUser(userId) -> { success, user }
  Json::Value getCurrentUser(const std::string& userId);

  // logoutUser(userId, deviceId, accessToken?) -> { success, message }
  Json::Value logoutUser(const std::string& userId,
                         const std::string& deviceId,
                         const std::string& accessToken = "");

  // sanitizeUser(user) -> the public user object (see authService.sanitizeUser).
  Json::Value sanitizeUser(const Json::Value& user);

 private:
  AuthService() = default;
};

// Convenience accessor mirroring the JS singleton import.
inline AuthService& authService() { return AuthService::instance(); }

}  // namespace pulse::services

// =============================================================================
// External service dependencies (separate ports — declared, not implemented
// here). authService.js delegates OTP delivery to customOTPService and Firebase
// ID-token verification to the firebase config module. These declarations
// mirror the exact JS method surface authService calls; the corresponding
// ports (custom_otp_service.cc / firebase_service.cc) provide the definitions.
// Keeping them here lets auth_service.cc call them with the same arguments and
// return shapes as the JS, without re-implementing their internals.
// =============================================================================
namespace pulse::services::custom_otp {

// customOTPService.sendEmailOTP(email, purpose, userId, ipAddress)
Json::Value sendEmailOTP(const std::string& email, const std::string& purpose,
                         const std::string& userId, const std::string& ipAddress);

// customOTPService.sendSMSOTP(phone, purpose, userId, ipAddress)
Json::Value sendSMSOTP(const std::string& phone, const std::string& purpose,
                       const std::string& userId, const std::string& ipAddress);

// customOTPService.verifyOTP(identifier, otp, purpose, ipAddress)
Json::Value verifyOTP(const std::string& identifier, const std::string& otp,
                      const std::string& purpose, const std::string& ipAddress);

// customOTPService.resendOTP(identifier, method, purpose, userId, ipAddress)
Json::Value resendOTP(const std::string& identifier, const std::string& method,
                      const std::string& purpose, const std::string& userId,
                      const std::string& ipAddress);

}  // namespace pulse::services::custom_otp

namespace pulse::services::firebase {

// firebaseConfig.isAvailable()
bool isAvailable();

// firebaseConfig.verifyIdToken(idToken) -> { uid, email, name, picture, ... }.
// Throws on an invalid token (matching the Admin SDK behavior the JS relied on).
Json::Value verifyIdToken(const std::string& idToken);

}  // namespace pulse::services::firebase
