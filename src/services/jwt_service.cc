// jwt_service.cc — implementation using jwt-cpp. Ports src/services/jwtService.js.
#include "pulse/jwt_service.hpp"
#include "pulse/config.hpp"

// The vcpkg jwt-cpp port is built with JWT_DISABLE_PICOJSON and ships no
// picojson header. Use the JsonCpp traits instead (the whole project already
// uses JsonCpp); defaults.h provides jwt::create()/jwt::decode()/jwt::claim
// backed by Json::Value.
#include <jwt-cpp/traits/open-source-parsers-jsoncpp/defaults.h>
#include <json/json.h>
#include <random>
#include <chrono>

namespace pulse {

namespace {
constexpr const char* kIssuer   = "pulse-app";
constexpr const char* kAudience = "pulse-users";

// Parse a JS-style duration ("15m", "7d", "10m", "900s") to seconds.
std::chrono::seconds parseDuration(const std::string& s, long long fallbackSec) {
  if (s.empty()) return std::chrono::seconds(fallbackSec);
  char unit = s.back();
  long long n = 0;
  try { n = std::stoll(s.substr(0, s.size() - 1)); } catch (...) { return std::chrono::seconds(fallbackSec); }
  switch (unit) {
    case 's': return std::chrono::seconds(n);
    case 'm': return std::chrono::seconds(n * 60);
    case 'h': return std::chrono::seconds(n * 3600);
    case 'd': return std::chrono::seconds(n * 86400);
    default:
      try { return std::chrono::seconds(std::stoll(s)); } catch (...) { return std::chrono::seconds(fallbackSec); }
  }
}

std::string randomHex(size_t bytes) {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uniform_int_distribution<int> d(0, 15);
  static const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(bytes * 2);
  for (size_t i = 0; i < bytes * 2; ++i) out += hex[d(rng)];
  return out;
}
} // namespace

JwtService::JwtService() {
  auto& cfg = config();
  accessSecret_  = cfg.jwt.secret;
  refreshSecret_ = cfg.jwt.refreshSecret;
  tempSecret_    = cfg.jwt.tempSecret;
  accessTtl_     = cfg.jwt.expiresIn;
  refreshTtl_    = cfg.jwt.refreshExpiresIn;
  if (accessSecret_.empty() || refreshSecret_.empty() || tempSecret_.empty()) {
    throw JwtError("JWT secrets not configured properly in environment variables");
  }
}

JwtService& JwtService::instance() { static JwtService s; return s; }

std::string JwtService::generateAccessToken(const AccessClaims& c) {
  auto now = std::chrono::system_clock::now();
  return jwt::create()
      .set_issuer(kIssuer)
      .set_audience(kAudience)
      .set_issued_at(now)
      .set_expires_at(now + parseDuration(accessTtl_, 900))
      .set_payload_claim("userId", jwt::claim(c.userId))
      .set_payload_claim("username", jwt::claim(c.username))
      .set_payload_claim("email", jwt::claim(c.email))
      .set_payload_claim("isVerified", jwt::claim(Json::Value(c.isVerified)))
      .set_payload_claim("type", jwt::claim(std::string("access")))
      .sign(jwt::algorithm::hs256{accessSecret_});
}

std::string JwtService::generateRefreshToken(const RefreshClaims& cIn) {
  RefreshClaims c = cIn;
  if (c.tokenId.empty()) c.tokenId = randomHex(16);
  auto now = std::chrono::system_clock::now();
  return jwt::create()
      .set_issuer(kIssuer)
      .set_audience(kAudience)
      .set_issued_at(now)
      .set_expires_at(now + parseDuration(refreshTtl_, 604800))
      .set_payload_claim("userId", jwt::claim(c.userId))
      .set_payload_claim("deviceId", jwt::claim(c.deviceId))
      .set_payload_claim("type", jwt::claim(std::string("refresh")))
      .set_payload_claim("tokenId", jwt::claim(c.tokenId))
      .sign(jwt::algorithm::hs256{refreshSecret_});
}

std::string JwtService::generateTempToken(const TempClaims& cIn) {
  TempClaims c = cIn;
  if (c.purpose.empty()) c.purpose = "username_creation";
  auto now = std::chrono::system_clock::now();
  return jwt::create()
      .set_issuer(kIssuer)
      .set_audience(kAudience)
      .set_issued_at(now)
      .set_expires_at(now + std::chrono::minutes(10))
      .set_payload_claim("userId", jwt::claim(c.userId))
      .set_payload_claim("purpose", jwt::claim(c.purpose))
      .set_payload_claim("type", jwt::claim(std::string("temporary")))
      .sign(jwt::algorithm::hs256{tempSecret_});
}

namespace {
// Shared verify: pin alg/iss/aud, then check the `type` claim. Translates
// jwt-cpp errors into the JS-compatible messages the middleware string-matches.
jwt::decoded_jwt<jwt::traits::open_source_parsers_jsoncpp>
verifyWith(const std::string& token, const std::string& secret,
           const std::string& expectedType, const char* kind) {
  try {
    auto decoded = jwt::decode(token);
    auto verifier = jwt::verify<jwt::traits::open_source_parsers_jsoncpp>()
        .allow_algorithm(jwt::algorithm::hs256{secret})
        .with_issuer(kIssuer)
        .with_audience(kAudience);
    verifier.verify(decoded);
    std::string type = decoded.has_payload_claim("type")
        ? decoded.get_payload_claim("type").as_string() : "";
    if (type != expectedType) throw JwtError("Invalid token type");
    return decoded;
  } catch (const JwtError&) {
    throw;
  } catch (const jwt::error::token_verification_exception& e) {
    std::string msg = e.what();
    if (msg.find("expired") != std::string::npos)
      throw JwtError(std::string(kind) + " token expired");
    throw JwtError(std::string("Invalid ") + kind + " token");
  } catch (const std::exception&) {
    throw JwtError(std::string("Invalid ") + kind + " token");
  }
}
} // namespace

AccessClaims JwtService::verifyAccessToken(const std::string& token) {
  auto d = verifyWith(token, accessSecret_, "access", "access");
  AccessClaims c;
  c.userId   = d.has_payload_claim("userId")   ? d.get_payload_claim("userId").as_string()   : "";
  c.username = d.has_payload_claim("username") ? d.get_payload_claim("username").as_string() : "";
  c.email    = d.has_payload_claim("email")    ? d.get_payload_claim("email").as_string()    : "";
  if (d.has_payload_claim("isVerified")) {
    Json::Value v = d.get_payload_claim("isVerified").to_json();
    c.isVerified = v.isBool() ? v.asBool() : false;
  }
  return c;
}

RefreshClaims JwtService::verifyRefreshToken(const std::string& token) {
  auto d = verifyWith(token, refreshSecret_, "refresh", "refresh");
  RefreshClaims c;
  c.userId   = d.has_payload_claim("userId")   ? d.get_payload_claim("userId").as_string()   : "";
  c.deviceId = d.has_payload_claim("deviceId") ? d.get_payload_claim("deviceId").as_string() : "";
  c.tokenId  = d.has_payload_claim("tokenId")  ? d.get_payload_claim("tokenId").as_string()  : "";
  return c;
}

TempClaims JwtService::verifyTempToken(const std::string& token) {
  auto d = verifyWith(token, tempSecret_, "temporary", "temporary");
  TempClaims c;
  c.userId  = d.has_payload_claim("userId")  ? d.get_payload_claim("userId").as_string()  : "";
  c.purpose = d.has_payload_claim("purpose") ? d.get_payload_claim("purpose").as_string() : "";
  return c;
}

TokenPair JwtService::generateTokenPair(const std::string& userId, const std::string& username,
                                        const std::string& email, bool isVerified,
                                        const std::string& deviceId) {
  TokenPair p;
  p.accessToken  = generateAccessToken({userId, username, email, isVerified});
  p.refreshToken = generateRefreshToken({userId, deviceId, ""});
  return p;
}

std::string JwtService::extractUserId(const std::string& token) {
  try {
    auto d = jwt::decode(token);
    if (d.has_payload_claim("userId")) return d.get_payload_claim("userId").as_string();
  } catch (...) {}
  return "";
}

bool JwtService::isTokenExpired(const std::string& token) {
  try {
    auto d = jwt::decode(token);
    auto exp = d.get_expires_at();
    return exp < std::chrono::system_clock::now();
  } catch (...) {
    return true;
  }
}

} // namespace pulse
