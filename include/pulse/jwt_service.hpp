// jwt_service.hpp — JWT layer, ports src/services/jwtService.js exactly.
//
// Three token kinds, each signed with its OWN secret (HS256):
//   access  (15m)  claims: userId, username, email, isVerified, type=access
//   refresh (7d)   claims: userId, deviceId, type=refresh, tokenId(random hex)
//   temp    (10m)  claims: userId, purpose, type=temporary
// All carry issuer="pulse-app", audience="pulse-users". Verification pins the
// algorithm (HS256), issuer, audience, and rejects a mismatched `type`.
#pragma once
#include <string>
#include <map>
#include <stdexcept>

namespace pulse {

struct AccessClaims  { std::string userId, username, email; bool isVerified = false; };
struct RefreshClaims { std::string userId, deviceId, tokenId; };
struct TempClaims    { std::string userId, purpose; };

struct TokenPair {
  std::string accessToken;
  std::string refreshToken;
  std::string tokenType = "Bearer";
  int expiresIn = 900;          // 15 min (seconds)
  int refreshExpiresIn = 604800; // 7 days (seconds)
};

// Thrown on verification failure with a JS-compatible message
// ("Access token expired", "Invalid access token", "Invalid token type", ...).
class JwtError : public std::runtime_error {
public:
  explicit JwtError(const std::string& m) : std::runtime_error(m) {}
};

class JwtService {
public:
  static JwtService& instance();

  std::string generateAccessToken(const AccessClaims& c);
  std::string generateRefreshToken(const RefreshClaims& c);   // tokenId auto-filled if empty
  std::string generateTempToken(const TempClaims& c);

  AccessClaims  verifyAccessToken(const std::string& token);   // throws JwtError
  RefreshClaims verifyRefreshToken(const std::string& token);
  TempClaims    verifyTempToken(const std::string& token);

  // Build access+refresh from a user + deviceId.
  TokenPair generateTokenPair(const std::string& userId,
                              const std::string& username,
                              const std::string& email,
                              bool isVerified,
                              const std::string& deviceId);

  // Best-effort, no verification (debug / extract userId / expiry check).
  std::string extractUserId(const std::string& token);
  bool isTokenExpired(const std::string& token);

private:
  JwtService();
  std::string accessSecret_, refreshSecret_, tempSecret_;
  std::string accessTtl_, refreshTtl_;  // e.g. "15m", "7d"
};

inline JwtService& jwt() { return JwtService::instance(); }

} // namespace pulse
