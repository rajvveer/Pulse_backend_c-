// custom_otp_adapter.cc — defines the pulse::services::custom_otp free-function
// adapter declared in auth_service.hpp, delegating to the CustomOTPService
// singleton. auth_service.cc calls these with (identifier, purpose, userId,
// ipAddress) where userId is a (possibly empty) 24-hex string; the singleton
// takes std::optional<std::string>, so an empty userId maps to std::nullopt
// (stored as null in the OTP doc, matching the JS `userId || null`).
#include "pulse/services/auth_service.hpp"        // the free-function declarations
#include "pulse/services/custom_otp_service.hpp"  // the singleton

#include <optional>
#include <string>

namespace pulse::services::custom_otp {

namespace {
std::optional<std::string> opt(const std::string& s) {
  return s.empty() ? std::nullopt : std::optional<std::string>(s);
}
} // namespace

Json::Value sendEmailOTP(const std::string& email, const std::string& purpose,
                         const std::string& userId, const std::string& ipAddress) {
  return pulse::customOtp().sendEmailOTP(email, purpose, opt(userId), ipAddress);
}

Json::Value sendSMSOTP(const std::string& phone, const std::string& purpose,
                       const std::string& userId, const std::string& ipAddress) {
  return pulse::customOtp().sendSMSOTP(phone, purpose, opt(userId), ipAddress);
}

Json::Value verifyOTP(const std::string& identifier, const std::string& otp,
                      const std::string& purpose, const std::string& ipAddress) {
  return pulse::customOtp().verifyOTP(identifier, otp, purpose, ipAddress);
}

Json::Value resendOTP(const std::string& identifier, const std::string& method,
                      const std::string& purpose, const std::string& userId,
                      const std::string& ipAddress) {
  return pulse::customOtp().resendOTP(identifier, method, purpose, opt(userId), ipAddress);
}

} // namespace pulse::services::custom_otp
