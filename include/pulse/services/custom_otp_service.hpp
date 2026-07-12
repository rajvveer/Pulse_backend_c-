// custom_otp_service.hpp — OTP service, ports src/services/customOTPService.js.
//
// Handles OTP generation, sending (email via Brevo HTTP API, SMS via Twilio),
// verification, per-identifier rate limiting (Redis), and a global daily SMS
// spend cap. OTP codes are bcrypt-hashed and stored in MongoDB ("otps").
//
// 1:1 parity notes (do not change):
//   - Redis keys:  otp_rate_limit:{type}:{identifier}   (TTL = windowMs/1000)
//                  sms_budget:{YYYY-MM-DD}              (TTL = 86400, UTC day)
//   - Email OTP expiry: 10 minutes; SMS OTP expiry: 5 minutes; maxAttempts: 3.
//   - Indian phone normalization to E.164 (+91XXXXXXXXXX).
//   - Email "from": 'Pulsee <rajveershekhawat626@gmail.com>'.
//   - Fail CLOSED in production when Redis is unavailable; allow in dev.
//
// Mirrors the JS module singleton: `module.exports = new CustomOTPService()`.
// Use pulse::customOtp() to reach the process-wide instance.
#pragma once
#include <string>
#include <optional>
#include <stdexcept>
#include <json/json.h>

namespace pulse {

// Thrown for every failure path the JS service surfaced via `throw new Error(...)`.
// The message string matches the JS message verbatim so callers that string-match
// (e.g. authService) behave identically.
class OtpError : public std::runtime_error {
public:
  explicit OtpError(const std::string& m) : std::runtime_error(m) {}
};

class CustomOTPService {
public:
  // Singleton accessor (mirrors the JS module singleton). First call runs the
  // constructor: detects EMAIL_API_KEY / TWILIO_* config and logs readiness.
  static CustomOTPService& instance();

  // generateOTP(length = 6): random decimal digits via a CSPRNG.
  std::string generateOTP(int length = 6);

  // checkGlobalSmsBudget(): increments sms_budget:{UTC-date}; throws OtpError if
  // the rolling-day total exceeds SMS_GLOBAL_DAILY_MAX (default 5000). Returns
  // the current count. Fails closed in production if Redis is unavailable.
  long long checkGlobalSmsBudget();

  // checkRateLimit(identifier, type, maxAttempts=5, windowMs=15*60*1000):
  // increments otp_rate_limit:{type}:{identifier}; throws OtpError past the cap.
  long long checkRateLimit(const std::string& identifier, const std::string& type,
                           int maxAttempts = 5, long long windowMs = 15LL * 60 * 1000);

  // sendEmailOTP(email, purpose='login', userId=null, ipAddress='127.0.0.1').
  // Returns { success, identifier, type:'email', purpose, expiresIn:'10 minutes',
  // message }. userId is an optional 24-hex ObjectId (nullopt -> stored null).
  Json::Value sendEmailOTP(const std::string& email,
                           const std::string& purpose = "login",
                           const std::optional<std::string>& userId = std::nullopt,
                           const std::string& ipAddress = "127.0.0.1");

  // sendSMSOTP(phone, purpose='login', userId=null, ipAddress='127.0.0.1').
  // Returns { success, identifier (E.164), type:'sms', purpose,
  // expiresIn:'5 minutes', message }.
  Json::Value sendSMSOTP(const std::string& phone,
                         const std::string& purpose = "login",
                         const std::optional<std::string>& userId = std::nullopt,
                         const std::string& ipAddress = "127.0.0.1");

  // verifyOTP(identifier, inputOTP, purpose, ipAddress='127.0.0.1').
  // Returns { success, otpId, userId, identifier, type, purpose, verifiedAt }.
  // Throws OtpError on invalid / expired / max-attempts / not-found.
  Json::Value verifyOTP(const std::string& identifier, const std::string& inputOTP,
                        const std::string& purpose,
                        const std::string& ipAddress = "127.0.0.1");

  // resendOTP(identifier, type, purpose, userId, ipAddress): deletes unverified
  // OTPs for the identifier+purpose, then re-sends. Returns the send result.
  Json::Value resendOTP(const std::string& identifier, const std::string& type,
                        const std::string& purpose,
                        const std::optional<std::string>& userId = std::nullopt,
                        const std::string& ipAddress = "127.0.0.1");

  // cleanupExpiredOTPs(): deleteMany({ expiresAt: { $lt: now } }). Returns count.
  long long cleanupExpiredOTPs();

  // Template helpers (public to mirror the JS methods; used internally on send).
  std::string getEmailSubject(const std::string& purpose);
  std::string getEmailTemplate(const std::string& purpose, const std::string& otp);
  std::string getSMSTemplate(const std::string& purpose, const std::string& otp);

  // Exposed for parity with the JS private helper `_normalizePhoneToE164`.
  std::string normalizePhoneToE164(const std::string& phone);

  bool emailConfigured() const { return emailConfigured_; }
  bool smsConfigured() const { return twilioConfigured_; }

private:
  CustomOTPService();

  // Sends a transactional email via the Brevo HTTP API (Drogon HttpClient).
  void sendBrevoEmail(const std::string& toEmail, const std::string& subject,
                      const std::string& html);

  // Sends an SMS via the Twilio Messages REST API (Drogon HttpClient).
  void sendTwilioSms(const std::string& toE164, const std::string& body);

  // Email (Brevo): EMAIL_API_KEY present.
  bool        emailConfigured_ = false;
  std::string emailApiKey_;

  // SMS (Twilio): all three credentials present.
  bool        twilioConfigured_ = false;
  std::string twilioAccountSid_;
  std::string twilioAuthToken_;
  std::string twilioFrom_;
};

// Convenience accessor matching the JS `customOTPService` singleton usage.
inline CustomOTPService& customOtp() { return CustomOTPService::instance(); }

} // namespace pulse
