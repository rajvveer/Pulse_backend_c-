// custom_otp_service.cc — implementation. Ports src/services/customOTPService.js.
//
// External HTTP (Brevo email, Twilio SMS) goes through Drogon's HttpClient.
// OTP codes are bcrypt-hashed (cost 10) and persisted to the "otps" collection.
#include "pulse/services/custom_otp_service.hpp"

#include "pulse/cache.hpp"
#include "pulse/config.hpp"
#include "pulse/logger.hpp"
#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/models/otp.hpp"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <trantor/net/EventLoopThread.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#endif

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/oid.hpp>
#include <mongocxx/exception/exception.hpp>

#include <openssl/rand.h>
#include <openssl/evp.h>
#include <pulse_bcrypt.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <future>
#include <regex>
#include <string>
#include <utility>

namespace pulse {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;

namespace {

bool isProduction() { return config().isProduction(); }

// --- bcrypt (cost 10), matching `bcrypt.hash(otp, 10)` / `bcrypt.compare` ------
std::string bcryptHash(const std::string& plain) {
  char salt[BCRYPT_HASHSIZE];
  char hash[BCRYPT_HASHSIZE];
  if (bcrypt_gensalt(10, salt) != 0)
    throw OtpError("Failed to generate bcrypt salt");
  if (bcrypt_hashpw(plain.c_str(), salt, hash) != 0)
    throw OtpError("Failed to hash OTP");
  return std::string(hash);
}

bool bcryptCompare(const std::string& plain, const std::string& hash) {
  // bcrypt_checkpw returns 0 on match, nonzero on mismatch/error.
  return bcrypt_checkpw(plain.c_str(), hash.c_str()) == 0;
}

// b_date helper for "now" + an offset (in ms), mirroring `new Date(Date.now()+x)`.
bsoncxx::types::b_date nowPlusMs(long long ms) {
  return bsoncxx::types::b_date{
      std::chrono::system_clock::now() + std::chrono::milliseconds(ms)};
}
bsoncxx::types::b_date nowDate() {
  return bsoncxx::types::b_date{std::chrono::system_clock::now()};
}

std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

// UTC date as YYYY-MM-DD, matching `new Date().toISOString().slice(0,10)`.
std::string utcDateYmd() {
  std::time_t t = std::chrono::system_clock::to_time_t(
      std::chrono::system_clock::now());
  std::tm tmv{};
#if defined(_WIN32)
  gmtime_s(&tmv, &t);
#else
  gmtime_r(&t, &tmv);
#endif
  char buf[11];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
  return std::string(buf);
}

// Persist one OTP document the way `OTP.create({...})` did, applying the OTP
// schema defaults (attempts:0, maxAttempts override, verified:false, etc.) and
// storing real BSON Dates for expiresAt/createdAt/updatedAt.
//
// type/purpose/identifier/hashedCode/ipAddress are required; userId optional.
void createOtpDoc(const std::optional<std::string>& userId,
                  const std::string& identifier, const std::string& type,
                  const std::string& purpose, const std::string& hashedCode,
                  const std::string& ipAddress, long long expiresInMs) {
  auto col = pulse::db::collection(pulse::models::otp::kCollection);

  bld::document doc{};
  // userId: { default: null } — store an ObjectId when provided, else null.
  if (userId && pulse::bsonjson::isValidOid(*userId)) {
    doc.append(kvp("userId", pulse::bsonjson::oid(*userId)));
  } else {
    doc.append(kvp("userId", bsoncxx::types::b_null{}));
  }
  doc.append(kvp("identifier", identifier));
  doc.append(kvp("type", type));
  doc.append(kvp("purpose", purpose));
  doc.append(kvp("hashedCode", hashedCode));
  doc.append(kvp("ipAddress", ipAddress));
  doc.append(kvp("expiresAt", nowPlusMs(expiresInMs)));
  // Schema defaults / required-with-default fields.
  doc.append(kvp("maxAttempts", 3));
  doc.append(kvp("attempts", 0));
  doc.append(kvp("verified", false));
  doc.append(kvp("verifiedAt", bsoncxx::types::b_null{}));
  doc.append(kvp("userAgent", ""));
  // timestamps: true
  doc.append(kvp("createdAt", nowDate()));
  doc.append(kvp("updatedAt", nowDate()));
  doc.append(kvp("__v", 0));

  col.insert_one(doc.view());
}

// Standard base64 (for HTTP Basic auth), via OpenSSL so we don't couple to a
// particular Drogon utils signature.
std::string base64Encode(const std::string& in) {
  if (in.empty()) return std::string();
  const int outLen = 4 * ((static_cast<int>(in.size()) + 2) / 3);
  std::string out(static_cast<size_t>(outLen), '\0');
  int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&out[0]),
                                reinterpret_cast<const unsigned char*>(in.data()),
                                static_cast<int>(in.size()));
  out.resize(static_cast<size_t>(written));
  return out;
}

// Resolve a hostname to an IPv4 dotted-quad string using the OS resolver
// (getaddrinfo). We do this ourselves because trantor's async resolver (c-ares)
// fails to load the Windows system DNS servers when initialized on a short-lived
// per-call event loop, surfacing as ReqResult::BadServerAddress. getaddrinfo
// reads the OS config synchronously and reliably. Returns empty on failure.
std::string resolveHostV4(const std::string& host) {
  struct addrinfo hints{};
  hints.ai_family = AF_INET;        // IPv4 (Brevo edge is dual-stack; v4 is fine)
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* res = nullptr;
  if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
    return {};
  }
  char buf[INET_ADDRSTRLEN] = {0};
  auto* sin = reinterpret_cast<sockaddr_in*>(res->ai_addr);
  ::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
  ::freeaddrinfo(res);
  return std::string(buf);
}

// One-shot synchronous HTTP call. Returns the response (null on transport
// failure).
//
// baseUrl is "https://host" (no path). The client is created with the full
// host string (so TLS SNI + certificate hostname validation use the real
// hostname) and run on Drogon's main app event loop. The app loop's c-ares
// resolver is initialized once at startup and reads the OS DNS config
// correctly — unlike a fresh per-call EventLoopThread, on which c-ares fails to
// load the Windows system DNS servers and returns ReqResult::BadServerAddress.
//
// Blocking on the future here is safe: this runs on an IO worker thread, while
// the request is driven by the main loop (a different thread), so there is no
// self-deadlock. We still pre-resolve via the OS as a fast, clear DNS fail-fast.
drogon::HttpResponsePtr sendHttpSync(const std::string& baseUrl,
                                     const drogon::HttpRequestPtr& req) {
  // Parse host out of baseUrl ("https://api.brevo.com") for the fail-fast check.
  std::string host = baseUrl;
  auto schemePos = host.find("://");
  if (schemePos != std::string::npos) host = host.substr(schemePos + 3);
  auto slash = host.find('/');
  if (slash != std::string::npos) host = host.substr(0, slash);
  auto colon = host.find(':');
  if (colon != std::string::npos) host = host.substr(0, colon);

  if (resolveHostV4(host).empty()) {
    pulse::log::error("sendHttpSync DNS resolution failed for host {}", host);
    return nullptr;
  }

  auto* loop = drogon::app().getLoop();  // main loop: clean c-ares resolver
  auto client = drogon::HttpClient::newHttpClient(
      baseUrl, loop, /*useOldTLS=*/false, /*validateCert=*/true);

  std::promise<std::pair<drogon::ReqResult, drogon::HttpResponsePtr>> prom;
  auto fut = prom.get_future();
  client->sendRequest(
      req, [&prom](drogon::ReqResult r, const drogon::HttpResponsePtr& resp) {
        prom.set_value({r, resp});
      }, 20.0);
  auto [result, resp] = fut.get();
  if (result != drogon::ReqResult::Ok) {
    const char* reason = "Unknown";
    switch (result) {
      case drogon::ReqResult::BadResponse:        reason = "BadResponse"; break;
      case drogon::ReqResult::NetworkFailure:     reason = "NetworkFailure"; break;
      case drogon::ReqResult::BadServerAddress:   reason = "BadServerAddress"; break;
      case drogon::ReqResult::Timeout:            reason = "Timeout"; break;
      case drogon::ReqResult::HandshakeError:     reason = "HandshakeError"; break;
      case drogon::ReqResult::InvalidCertificate: reason = "InvalidCertificate"; break;
      case drogon::ReqResult::EncryptionFailure:  reason = "EncryptionFailure"; break;
      default: break;
    }
    pulse::log::error("sendHttpSync transport failure to {}: ReqResult={}", baseUrl, reason);
    return nullptr;
  }
  return resp;
}

} // namespace

// -----------------------------------------------------------------------------
// Construction / singleton
// -----------------------------------------------------------------------------
CustomOTPService::CustomOTPService() {
  auto& cfg = config();

  // Email setup (Brevo via HTTP API) — check EMAIL_API_KEY.
  emailApiKey_ = cfg.env("EMAIL_API_KEY");
  if (!emailApiKey_.empty()) {
    emailConfigured_ = true;
    pulse::log::info("Email Service Ready (via Brevo HTTP)");
  } else {
    pulse::log::warn("EMAIL_API_KEY missing in .env - Email OTP disabled");
  }

  // SMS setup (Twilio) — require SID + token + from.
  twilioAccountSid_ = cfg.env("TWILIO_ACCOUNT_SID");
  twilioAuthToken_  = cfg.env("TWILIO_AUTH_TOKEN");
  twilioFrom_       = cfg.env("TWILIO_FROM");
  if (!twilioAccountSid_.empty() && !twilioAuthToken_.empty() && !twilioFrom_.empty()) {
    twilioConfigured_ = true;
    pulse::log::info("Twilio SMS service configured");
  } else {
    pulse::log::warn("Twilio credentials not configured - SMS OTP disabled");
  }
}

CustomOTPService& CustomOTPService::instance() {
  static CustomOTPService s;
  return s;
}

// -----------------------------------------------------------------------------
// _normalizePhoneToE164
// -----------------------------------------------------------------------------
std::string CustomOTPService::normalizePhoneToE164(const std::string& phone) {
  std::string digitsOnly;
  digitsOnly.reserve(phone.size());
  for (char c : phone) if (std::isdigit(static_cast<unsigned char>(c))) digitsOnly += c;

  if (digitsOnly.size() == 12 && digitsOnly.rfind("91", 0) == 0) {
    return "+" + digitsOnly;
  }
  if (digitsOnly.size() == 10) {
    return "+91" + digitsOnly;
  }
  // Already E.164 or something else: return as-is.
  return phone;
}

// -----------------------------------------------------------------------------
// generateOTP
// -----------------------------------------------------------------------------
std::string CustomOTPService::generateOTP(int length) {
  static const char* digits = "0123456789";
  std::string otp;
  otp.reserve(static_cast<size_t>(length));
  for (int i = 0; i < length; ++i) {
    // crypto.randomInt(0, 10) — unbiased pick of a digit via a CSPRNG.
    unsigned char b;
    unsigned idx;
    do {
      if (RAND_bytes(&b, 1) != 1) throw OtpError("Secure RNG failure");
      idx = static_cast<unsigned>(b);
    } while (idx >= 250);  // 250 = floor(256/10)*10; reject to avoid modulo bias
    otp += digits[idx % 10];
  }
  return otp;
}

// -----------------------------------------------------------------------------
// checkGlobalSmsBudget
// -----------------------------------------------------------------------------
long long CustomOTPService::checkGlobalSmsBudget() {
  long long dailyMax = config().envInt("SMS_GLOBAL_DAILY_MAX", 5000);
  if (dailyMax <= 0) dailyMax = 5000;  // parseInt(...) || 5000

  try {
    const std::string key = "sms_budget:" + utcDateYmd();  // per UTC day
    long long count = cache().incrementRateLimit(key, 24 * 60 * 60);
    if (count > dailyMax) {
      throw OtpError("Global SMS budget exhausted for today.");
    }
    return count;
  } catch (const OtpError& e) {
    std::string msg = e.what();
    if (msg.find("budget exhausted") != std::string::npos) throw;
    if (isProduction()) {
      pulse::log::error("SMS budget check failed - refusing send: {}", msg);
      throw OtpError("SMS service temporarily unavailable. Please try again shortly.");
    }
    return 1;  // dev: allow
  } catch (const std::exception& e) {
    if (isProduction()) {
      pulse::log::error("SMS budget check failed - refusing send: {}", e.what());
      throw OtpError("SMS service temporarily unavailable. Please try again shortly.");
    }
    return 1;  // dev: allow
  }
}

// -----------------------------------------------------------------------------
// checkRateLimit
// -----------------------------------------------------------------------------
long long CustomOTPService::checkRateLimit(const std::string& identifier,
                                           const std::string& type,
                                           int maxAttempts, long long windowMs) {
  try {
    const std::string key = "otp_rate_limit:" + type + ":" + identifier;
    long long attempts = cache().incrementRateLimit(key, windowMs / 1000);
    if (attempts > maxAttempts) {
      throw OtpError("Too many OTP requests. Please try again after " +
                     std::to_string(windowMs / 60000) + " minutes.");
    }
    return attempts;
  } catch (const OtpError& e) {
    std::string msg = e.what();
    if (msg.find("Too many") != std::string::npos) throw;
    // Redis failure: fail CLOSED in production.
    if (isProduction()) {
      pulse::log::error("OTP rate limit check failed - refusing send: {}", msg);
      throw OtpError("OTP service temporarily unavailable. Please try again shortly.");
    }
    pulse::log::warn("Rate limiting check failed, continuing (dev only): {}", msg);
    return 1;
  } catch (const std::exception& e) {
    if (isProduction()) {
      pulse::log::error("OTP rate limit check failed - refusing send: {}", e.what());
      throw OtpError("OTP service temporarily unavailable. Please try again shortly.");
    }
    pulse::log::warn("Rate limiting check failed, continuing (dev only): {}", e.what());
    return 1;
  }
}

// -----------------------------------------------------------------------------
// sendEmailOTP
// -----------------------------------------------------------------------------
Json::Value CustomOTPService::sendEmailOTP(const std::string& email,
                                           const std::string& purpose,
                                           const std::optional<std::string>& userId,
                                           const std::string& ipAddress) {
  try {
    if (!emailConfigured_) {
      throw OtpError("Email OTP not configured. Check EMAIL_API_KEY in .env file");
    }

    static const std::regex emailRegex(R"(^[^\s@]+@[^\s@]+\.[^\s@]+$)");
    if (!std::regex_match(email, emailRegex)) {
      throw OtpError("Please enter a valid email address");
    }

    checkRateLimit(email, "email");
    const std::string otp = generateOTP(6);
    const std::string hashedOTP = bcryptHash(otp);

    const std::string identifier = toLower(email);
    createOtpDoc(userId, identifier, "email", purpose, hashedOTP, ipAddress,
                 10 * 60 * 1000);  // 10 minutes

    const std::string subject = getEmailSubject(purpose);
    const std::string html = getEmailTemplate(purpose, otp);

    sendBrevoEmail(email, subject, html);

    pulse::log::info("Email OTP sent to {} for {}", email, purpose);

    Json::Value out(Json::objectValue);
    out["success"]   = true;
    out["identifier"] = identifier;
    out["type"]      = "email";
    out["purpose"]   = purpose;
    out["expiresIn"] = "10 minutes";
    out["message"]   = "OTP sent to " + email;
    return out;
  } catch (const std::exception& error) {
    pulse::log::error("Email OTP send error: {}", error.what());
    throw OtpError(std::string("Failed to send email OTP: ") + error.what());
  }
}

// -----------------------------------------------------------------------------
// sendSMSOTP
// -----------------------------------------------------------------------------
Json::Value CustomOTPService::sendSMSOTP(const std::string& phone,
                                         const std::string& purpose,
                                         const std::optional<std::string>& userId,
                                         const std::string& ipAddress) {
  try {
    if (!twilioConfigured_) {
      throw OtpError("SMS OTP not configured. Please set TWILIO credentials in .env file");
    }

    const std::string fullPhone = normalizePhoneToE164(phone);
    static const std::regex phoneRegex(R"(^\+91[6-9]\d{9}$)");
    if (!std::regex_match(fullPhone, phoneRegex)) {
      throw OtpError("Please enter a valid Indian mobile number.");
    }

    // Global spend circuit breaker first, then per-number rate limit.
    checkGlobalSmsBudget();
    checkRateLimit(fullPhone, "sms");
    const std::string otp = generateOTP(6);
    const std::string hashedOTP = bcryptHash(otp);

    createOtpDoc(userId, fullPhone, "sms", purpose, hashedOTP, ipAddress,
                 5 * 60 * 1000);  // 5 minutes

    const std::string smsBody = getSMSTemplate(purpose, otp);
    sendTwilioSms(fullPhone, smsBody);

    Json::Value out(Json::objectValue);
    out["success"]   = true;
    out["identifier"] = fullPhone;
    out["type"]      = "sms";
    out["purpose"]   = purpose;
    out["expiresIn"] = "5 minutes";
    out["message"]   = "OTP sent to " + fullPhone;
    return out;
  } catch (const std::exception& error) {
    pulse::log::error("SMS OTP send error: {}", error.what());
    throw OtpError(std::string("Failed to send SMS OTP: ") + error.what());
  }
}

// -----------------------------------------------------------------------------
// getEmailSubject
// -----------------------------------------------------------------------------
std::string CustomOTPService::getEmailSubject(const std::string& purpose) {
  if (purpose == "signup")         return "\xF0\x9F\x8E\x89 Welcome to Pulse - Verify Your Email";
  if (purpose == "login")          return "\xF0\x9F\x94\x90 Pulse Login Verification Code";
  if (purpose == "password_reset") return "\xF0\x9F\x94\x91 Reset Your Pulse Password";
  if (purpose == "verification")   return "\xE2\x9C\x89\xEF\xB8\x8F Verify Your Pulse Account";
  // default: login
  return "\xF0\x9F\x94\x90 Pulse Login Verification Code";
}

// -----------------------------------------------------------------------------
// getEmailTemplate
// -----------------------------------------------------------------------------
namespace {

// Current 4-digit year, matching `new Date().getFullYear()`.
std::string currentYear() {
  std::time_t t = std::chrono::system_clock::to_time_t(
      std::chrono::system_clock::now());
  std::tm tmv{};
#if defined(_WIN32)
  gmtime_s(&tmv, &t);
#else
  gmtime_r(&t, &tmv);
#endif
  return std::to_string(tmv.tm_year + 1900);
}

std::string emailSharedFooter() {
  return std::string(
      "            <div class=\"footer\">\n"
      "              <p class=\"footer-text\"><strong>Pulse</strong></p>\n"
      "              <p class=\"footer-text\">Connect. Share. Pulse.</p>\n"
      "              <p class=\"footer-text\" style=\"margin-top: 20px;\">\n"
      "                \xC2\xA9 ") + currentYear() +
      " Pulse. All rights reserved.\n"
      "              </p>\n"
      "            </div>";
}

} // namespace

std::string CustomOTPService::getEmailTemplate(const std::string& purpose,
                                               const std::string& otp) {
  const std::string footer = emailSharedFooter();

  // Shared <style> block used by signup / login / verification (blue/purple).
  const std::string styleBluePurple =
      "          <style>\n"
      "            body { margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, 'Helvetica Neue', Arial, sans-serif; }\n"
      "            .container { max-width: 600px; margin: 0 auto; background-color: #ffffff; }\n"
      "            .header { background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); padding: 40px 20px; text-align: center; }\n"
      "            .header h1 { color: #ffffff; margin: 0; font-size: 28px; font-weight: 600; }\n"
      "            .content { padding: 40px 30px; }\n"
      "            .greeting { font-size: 18px; color: #333333; margin-bottom: 20px; }\n"
      "            .message { font-size: 15px; color: #666666; line-height: 1.6; margin-bottom: 30px; }\n"
      "            .otp-container { background-color: #f8f9fa; border: 2px dashed #667eea; border-radius: 12px; padding: 30px; text-align: center; margin: 30px 0; }\n"
      "            .otp-label { font-size: 13px; color: #666666; text-transform: uppercase; letter-spacing: 1px; margin-bottom: 10px; font-weight: 600; }\n"
      "            .otp-code { font-size: 36px; font-weight: 700; color: #667eea; letter-spacing: 8px; font-family: 'Courier New', monospace; margin: 10px 0; }\n"
      "            .expiry { font-size: 13px; color: #999999; margin-top: 15px; }\n";

  if (purpose == "signup") {
    return std::string(
        "\n        <!DOCTYPE html>\n        <html>\n        <head>\n"
        "          <meta charset=\"UTF-8\">\n"
        "          <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n") +
        styleBluePurple +
        "            .warning { background-color: #fff3cd; border-left: 4px solid #ffc107; padding: 15px; margin: 20px 0; border-radius: 4px; }\n"
        "            .warning-text { font-size: 14px; color: #856404; margin: 0; }\n"
        "            .footer { background-color: #f8f9fa; padding: 30px; text-align: center; border-top: 1px solid #e9ecef; }\n"
        "            .footer-text { font-size: 13px; color: #6c757d; margin: 5px 0; }\n"
        "            .divider { height: 1px; background-color: #e9ecef; margin: 30px 0; }\n"
        "          </style>\n        </head>\n        <body>\n"
        "          <div class=\"container\">\n"
        "            <div class=\"header\">\n"
        "              <h1>\xF0\x9F\x8E\x89 Welcome to Pulse</h1>\n"
        "            </div>\n            \n"
        "            <div class=\"content\">\n"
        "              <p class=\"greeting\">Hello there!</p>\n              \n"
        "              <p class=\"message\">\n"
        "                Thank you for signing up with Pulse. We're excited to have you on board! \n"
        "                To complete your registration and verify your email address, please use the verification code below:\n"
        "              </p>\n              \n"
        "              <div class=\"otp-container\">\n"
        "                <div class=\"otp-label\">Your Verification Code</div>\n"
        "                <div class=\"otp-code\">" + otp + "</div>\n"
        "                <div class=\"expiry\">\xE2\x8F\xB1\xEF\xB8\x8F Valid for 10 minutes</div>\n"
        "              </div>\n              \n"
        "              <p class=\"message\">\n"
        "                Enter this code in the app to verify your account and get started with all the amazing features Pulse has to offer.\n"
        "              </p>\n              \n"
        "              <div class=\"warning\">\n"
        "                <p class=\"warning-text\">\n"
        "                  <strong>\xF0\x9F\x94\x92 Security Notice:</strong> Never share this code with anyone. \n"
        "                  Our team will never ask for your verification code via email, phone, or text message.\n"
        "                </p>\n"
        "              </div>\n              \n"
        "              <div class=\"divider\"></div>\n              \n"
        "              <p class=\"message\" style=\"font-size: 13px; color: #999999;\">\n"
        "                If you didn't request this code, please ignore this email. Your account remains secure.\n"
        "              </p>\n"
        "            </div>\n            \n" + footer +
        "\n          </div>\n        </body>\n        </html>\n      ";
  }

  if (purpose == "password_reset") {
    return std::string(
        "\n        <!DOCTYPE html>\n        <html>\n        <head>\n"
        "          <meta charset=\"UTF-8\">\n"
        "          <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
        "          <style>\n"
        "            body { margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, 'Helvetica Neue', Arial, sans-serif; }\n"
        "            .container { max-width: 600px; margin: 0 auto; background-color: #ffffff; }\n"
        "            .header { background: linear-gradient(135deg, #f093fb 0%, #f5576c 100%); padding: 40px 20px; text-align: center; }\n"
        "            .header h1 { color: #ffffff; margin: 0; font-size: 28px; font-weight: 600; }\n"
        "            .content { padding: 40px 30px; }\n"
        "            .greeting { font-size: 18px; color: #333333; margin-bottom: 20px; }\n"
        "            .message { font-size: 15px; color: #666666; line-height: 1.6; margin-bottom: 30px; }\n"
        "            .otp-container { background-color: #fff5f5; border: 2px dashed #f5576c; border-radius: 12px; padding: 30px; text-align: center; margin: 30px 0; }\n"
        "            .otp-label { font-size: 13px; color: #666666; text-transform: uppercase; letter-spacing: 1px; margin-bottom: 10px; font-weight: 600; }\n"
        "            .otp-code { font-size: 36px; font-weight: 700; color: #f5576c; letter-spacing: 8px; font-family: 'Courier New', monospace; margin: 10px 0; }\n"
        "            .expiry { font-size: 13px; color: #999999; margin-top: 15px; }\n"
        "            .warning { background-color: #f8d7da; border-left: 4px solid #dc3545; padding: 15px; margin: 20px 0; border-radius: 4px; }\n"
        "            .warning-text { font-size: 14px; color: #721c24; margin: 0; }\n"
        "            .footer { background-color: #f8f9fa; padding: 30px; text-align: center; border-top: 1px solid #e9ecef; }\n"
        "            .footer-text { font-size: 13px; color: #6c757d; margin: 5px 0; }\n"
        "            .divider { height: 1px; background-color: #e9ecef; margin: 30px 0; }\n"
        "          </style>\n        </head>\n        <body>\n"
        "          <div class=\"container\">\n"
        "            <div class=\"header\">\n"
        "              <h1>\xF0\x9F\x94\x91 Password Reset</h1>\n"
        "            </div>\n            \n"
        "            <div class=\"content\">\n"
        "              <p class=\"greeting\">Password Reset Request</p>\n              \n"
        "              <p class=\"message\">\n"
        "                We received a request to reset the password for your Pulse account. \n"
        "                Use the verification code below to proceed with resetting your password:\n"
        "              </p>\n              \n"
        "              <div class=\"otp-container\">\n"
        "                <div class=\"otp-label\">Password Reset Code</div>\n"
        "                <div class=\"otp-code\">" + otp + "</div>\n"
        "                <div class=\"expiry\">\xE2\x8F\xB1\xEF\xB8\x8F Valid for 10 minutes</div>\n"
        "              </div>\n              \n"
        "              <p class=\"message\">\n"
        "                Enter this code in the app to create a new password for your account.\n"
        "              </p>\n              \n"
        "              <div class=\"warning\">\n"
        "                <p class=\"warning-text\">\n"
        "                  <strong>\xE2\x9A\xA0\xEF\xB8\x8F Important:</strong> If you didn't request a password reset, \n"
        "                  please contact our support team immediately. Someone may be trying to access your account.\n"
        "                </p>\n"
        "              </div>\n              \n"
        "              <div class=\"divider\"></div>\n              \n"
        "              <p class=\"message\" style=\"font-size: 13px; color: #999999;\">\n"
        "                For security reasons, this code will expire in 10 minutes.\n"
        "              </p>\n"
        "            </div>\n            \n" + footer +
        "\n          </div>\n        </body>\n        </html>\n      ");
  }

  if (purpose == "verification") {
    return std::string(
        "\n        <!DOCTYPE html>\n        <html>\n        <head>\n"
        "          <meta charset=\"UTF-8\">\n"
        "          <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n") +
        styleBluePurple +
        "            .warning { background-color: #d1ecf1; border-left: 4px solid #17a2b8; padding: 15px; margin: 20px 0; border-radius: 4px; }\n"
        "            .warning-text { font-size: 14px; color: #0c5460; margin: 0; }\n"
        "            .footer { background-color: #f8f9fa; padding: 30px; text-align: center; border-top: 1px solid #e9ecef; }\n"
        "            .footer-text { font-size: 13px; color: #6c757d; margin: 5px 0; }\n"
        "            .divider { height: 1px; background-color: #e9ecef; margin: 30px 0; }\n"
        "          </style>\n        </head>\n        <body>\n"
        "          <div class=\"container\">\n"
        "            <div class=\"header\">\n"
        "              <h1>\xE2\x9C\x89\xEF\xB8\x8F Email Verification</h1>\n"
        "            </div>\n            \n"
        "            <div class=\"content\">\n"
        "              <p class=\"greeting\">Verify Your Email</p>\n              \n"
        "              <p class=\"message\">\n"
        "                To ensure the security of your Pulse account, please verify your email address \n"
        "                by entering the code below:\n"
        "              </p>\n              \n"
        "              <div class=\"otp-container\">\n"
        "                <div class=\"otp-label\">Verification Code</div>\n"
        "                <div class=\"otp-code\">" + otp + "</div>\n"
        "                <div class=\"expiry\">\xE2\x8F\xB1\xEF\xB8\x8F Valid for 10 minutes</div>\n"
        "              </div>\n              \n"
        "              <p class=\"message\">\n"
        "                Once verified, you'll have full access to all Pulse features.\n"
        "              </p>\n              \n"
        "              <div class=\"warning\">\n"
        "                <p class=\"warning-text\">\n"
        "                  <strong>\xE2\x84\xB9\xEF\xB8\x8F Note:</strong> This verification helps us ensure that you have access \n"
        "                  to this email address and can receive important account notifications.\n"
        "                </p>\n"
        "              </div>\n              \n"
        "              <div class=\"divider\"></div>\n              \n"
        "              <p class=\"message\" style=\"font-size: 13px; color: #999999;\">\n"
        "                If you didn't initiate this verification, no action is required.\n"
        "              </p>\n"
        "            </div>\n            \n" + footer +
        "\n          </div>\n        </body>\n        </html>\n      ";
  }

  // default / login
  return std::string(
      "\n        <!DOCTYPE html>\n        <html>\n        <head>\n"
      "          <meta charset=\"UTF-8\">\n"
      "          <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n") +
      styleBluePurple +
      "            .warning { background-color: #fff3cd; border-left: 4px solid #ffc107; padding: 15px; margin: 20px 0; border-radius: 4px; }\n"
      "            .warning-text { font-size: 14px; color: #856404; margin: 0; }\n"
      "            .footer { background-color: #f8f9fa; padding: 30px; text-align: center; border-top: 1px solid #e9ecef; }\n"
      "            .footer-text { font-size: 13px; color: #6c757d; margin: 5px 0; }\n"
      "            .divider { height: 1px; background-color: #e9ecef; margin: 30px 0; }\n"
      "          </style>\n        </head>\n        <body>\n"
      "          <div class=\"container\">\n"
      "            <div class=\"header\">\n"
      "              <h1>\xF0\x9F\x94\x90 Login Verification</h1>\n"
      "            </div>\n            \n"
      "            <div class=\"content\">\n"
      "              <p class=\"greeting\">Welcome back!</p>\n              \n"
      "              <p class=\"message\">\n"
      "                We received a login request for your Pulse account. To continue signing in, \n"
      "                please use the verification code below:\n"
      "              </p>\n              \n"
      "              <div class=\"otp-container\">\n"
      "                <div class=\"otp-label\">Your Login Code</div>\n"
      "                <div class=\"otp-code\">" + otp + "</div>\n"
      "                <div class=\"expiry\">\xE2\x8F\xB1\xEF\xB8\x8F Valid for 10 minutes</div>\n"
      "              </div>\n              \n"
      "              <p class=\"message\">\n"
      "                Enter this code in the app to complete your login and access your account.\n"
      "              </p>\n              \n"
      "              <div class=\"warning\">\n"
      "                <p class=\"warning-text\">\n"
      "                  <strong>\xF0\x9F\x94\x92 Security Alert:</strong> If you didn't attempt to log in, \n"
      "                  please ignore this email and consider changing your password immediately.\n"
      "                </p>\n"
      "              </div>\n              \n"
      "              <div class=\"divider\"></div>\n              \n"
      "              <p class=\"message\" style=\"font-size: 13px; color: #999999;\">\n"
      "                This is an automated security email. Please do not reply to this message.\n"
      "              </p>\n"
      "            </div>\n            \n" + footer +
      "\n          </div>\n        </body>\n        </html>\n      ";
}

// -----------------------------------------------------------------------------
// getSMSTemplate
// -----------------------------------------------------------------------------
std::string CustomOTPService::getSMSTemplate(const std::string& purpose,
                                             const std::string& otp) {
  if (purpose == "signup")
    return otp + " is your Pulse verification code. Valid for 5 minutes. Never share this code with anyone.";
  if (purpose == "login")
    return otp + " is your Pulse login code. Valid for 5 minutes. If you didn't request this, ignore this message.";
  if (purpose == "password_reset")
    return otp + " is your Pulse password reset code. Valid for 5 minutes. Don't share this code.";
  if (purpose == "verification")
    return otp + " is your Pulse verification code. Valid for 5 minutes.";
  // default: login
  return otp + " is your Pulse login code. Valid for 5 minutes. If you didn't request this, ignore this message.";
}

// -----------------------------------------------------------------------------
// verifyOTP
// -----------------------------------------------------------------------------
Json::Value CustomOTPService::verifyOTP(const std::string& identifier,
                                        const std::string& inputOTP,
                                        const std::string& purpose,
                                        const std::string& ipAddress) {
  try {
    std::string lookupIdentifier = identifier;

    // If not an email, assume a phone number and normalize.
    if (identifier.find('@') == std::string::npos) {
      lookupIdentifier = normalizePhoneToE164(identifier);
    }

    pulse::log::debug("Verifying OTP for original: \"{}\", normalized to: \"{}\"",
                      identifier, lookupIdentifier);

    auto otpRecord = pulse::models::otp::findValidOTP(lookupIdentifier, purpose);
    if (!otpRecord) {
      throw OtpError("Invalid or expired OTP");
    }
    const Json::Value& rec = *otpRecord;

    // isExpired(): expiresAt < now
    const std::string expiresAtIso = rec.get("expiresAt", "").asString();
    if (pulse::models::otp::isExpired(expiresAtIso)) {
      throw OtpError("OTP has expired");
    }

    // isMaxAttemptsReached(): attempts >= maxAttempts
    const int attempts    = rec.get("attempts", 0).asInt();
    const int maxAttempts = rec.get("maxAttempts", 3).asInt();
    if (pulse::models::otp::isMaxAttemptsReached(attempts, maxAttempts)) {
      throw OtpError("Maximum verification attempts exceeded");
    }

    const std::string hashedCode = rec.get("hashedCode", "").asString();
    const bool isValid = bcryptCompare(inputOTP, hashedCode);

    const std::string otpIdHex = rec.get("_id", "").asString();
    const std::string type     = rec.get("type", "").asString();

    if (!isValid) {
      int newAttempts = attempts;
      if (auto oid = pulse::bsonjson::tryOid(otpIdHex)) {
        if (auto inc = pulse::models::otp::incrementAttempts(*oid)) newAttempts = *inc;
      }
      int remainingAttempts = std::max(0, maxAttempts - newAttempts);
      throw OtpError("Invalid OTP. " + std::to_string(remainingAttempts) +
                     " attempts remaining.");
    }

    if (auto oid = pulse::bsonjson::tryOid(otpIdHex)) {
      pulse::models::otp::markAsVerified(*oid);
    }

    // Best-effort: clear the send rate-limit key.
    try {
      cache().del("otp_rate_limit:" + type + ":" + lookupIdentifier);
    } catch (const std::exception& cacheError) {
      pulse::log::warn("Cache deletion failed on OTP verify: {}", cacheError.what());
    }

    pulse::log::info("OTP verified successfully for {}", lookupIdentifier);

    Json::Value out(Json::objectValue);
    out["success"]    = true;
    out["otpId"]      = rec.get("_id", Json::Value(Json::nullValue));
    out["userId"]     = rec.get("userId", Json::Value(Json::nullValue));
    out["identifier"] = rec.get("identifier", Json::Value(Json::nullValue));
    out["type"]       = rec.get("type", Json::Value(Json::nullValue));
    out["purpose"]    = rec.get("purpose", Json::Value(Json::nullValue));
    // verifiedAt was just set; reflect "now" since markAsVerified updated it.
    out["verifiedAt"] = pulse::bsonjson::nowIso8601();
    return out;
  } catch (const OtpError& error) {
    pulse::log::error("OTP verification error: {}", error.what());
    throw;
  } catch (const std::exception& error) {
    pulse::log::error("OTP verification error: {}", error.what());
    throw OtpError(error.what());
  }
}

// -----------------------------------------------------------------------------
// resendOTP
// -----------------------------------------------------------------------------
Json::Value CustomOTPService::resendOTP(const std::string& identifier,
                                        const std::string& type,
                                        const std::string& purpose,
                                        const std::optional<std::string>& userId,
                                        const std::string& ipAddress) {
  try {
    std::string lookupIdentifier = identifier;
    if (type == "sms") {
      lookupIdentifier = normalizePhoneToE164(identifier);
    }

    // OTP.deleteMany({ identifier: lookupIdentifier, purpose, verified: false })
    {
      auto col = pulse::db::collection(pulse::models::otp::kCollection);
      col.delete_many(make_document(kvp("identifier", lookupIdentifier),
                                    kvp("purpose", purpose),
                                    kvp("verified", false)));
    }

    // NOTE: deliberately NOT clearing the rate-limit key here.

    if (type == "email") {
      return sendEmailOTP(identifier, purpose, userId, ipAddress);
    } else if (type == "sms") {
      return sendSMSOTP(identifier, purpose, userId, ipAddress);
    } else {
      throw OtpError("Invalid OTP type");
    }
  } catch (const OtpError& error) {
    pulse::log::error("Resend OTP error: {}", error.what());
    throw;
  } catch (const std::exception& error) {
    pulse::log::error("Resend OTP error: {}", error.what());
    throw OtpError(error.what());
  }
}

// -----------------------------------------------------------------------------
// cleanupExpiredOTPs
// -----------------------------------------------------------------------------
long long CustomOTPService::cleanupExpiredOTPs() {
  try {
    long long deleted = pulse::models::otp::cleanupExpired();
    if (deleted > 0) {
      pulse::log::info("Cleaned up {} expired OTPs", deleted);
    }
    return deleted;
  } catch (const std::exception& error) {
    pulse::log::error("OTP cleanup error: {}", error.what());
    return 0;
  }
}

// -----------------------------------------------------------------------------
// External sends (Drogon HttpClient)
// -----------------------------------------------------------------------------
void CustomOTPService::sendBrevoEmail(const std::string& toEmail,
                                      const std::string& subject,
                                      const std::string& html) {
  // Brevo (Sendinblue) transactional email API — the HTTP equivalent of the
  // nodemailer-brevo-transport SMTP/HTTP transport. POST /v3/smtp/email.
  Json::Value body(Json::objectValue);
  Json::Value sender(Json::objectValue);
  sender["name"]  = "Pulsee";
  sender["email"] = "rajveershekhawat626@gmail.com";  // MUST match verified sender
  body["sender"]  = sender;

  Json::Value toArr(Json::arrayValue);
  Json::Value toObj(Json::objectValue);
  toObj["email"] = toEmail;
  toArr.append(toObj);
  body["to"]          = toArr;
  body["subject"]     = subject;
  body["htmlContent"] = html;

  auto req = drogon::HttpRequest::newHttpJsonRequest(body);
  req->setMethod(drogon::Post);
  req->setPath("/v3/smtp/email");
  req->addHeader("api-key", emailApiKey_);
  req->addHeader("accept", "application/json");

  auto resp = sendHttpSync("https://api.brevo.com", req);
  if (!resp) {
    throw OtpError("Email transport failed (no response from Brevo)");
  }
  int status = static_cast<int>(resp->getStatusCode());
  if (status < 200 || status >= 300) {
    throw OtpError("Brevo email send failed with status " + std::to_string(status) +
                   ": " + std::string(resp->getBody()));
  }
}

void CustomOTPService::sendTwilioSms(const std::string& toE164,
                                     const std::string& body) {
  // Twilio Messages REST API: POST /2010-04-01/Accounts/{SID}/Messages.json
  // with form-urlencoded body and HTTP Basic auth (SID:authToken).
  auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(drogon::Post);
  req->setPath("/2010-04-01/Accounts/" + twilioAccountSid_ + "/Messages.json");
  req->setContentTypeCode(drogon::CT_APPLICATION_X_FORM);
  req->setParameter("To", toE164);
  req->setParameter("From", twilioFrom_);
  req->setParameter("Body", body);

  // Basic auth header: base64(SID:authToken).
  const std::string creds = twilioAccountSid_ + ":" + twilioAuthToken_;
  req->addHeader("Authorization", "Basic " + base64Encode(creds));

  auto resp = sendHttpSync("https://api.twilio.com", req);
  if (!resp) {
    throw OtpError("SMS transport failed (no response from Twilio)");
  }
  int status = static_cast<int>(resp->getStatusCode());
  if (status < 200 || status >= 300) {
    throw OtpError("Twilio SMS send failed with status " + std::to_string(status) +
                   ": " + std::string(resp->getBody()));
  }

  // Log the returned message SID for parity with the JS service.
  auto json = resp->getJsonObject();
  if (json && json->isMember("sid")) {
    pulse::log::info("Twilio message SID: {}", (*json)["sid"].asString());
  }
}

} // namespace pulse
