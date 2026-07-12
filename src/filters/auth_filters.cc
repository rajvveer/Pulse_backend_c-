// auth_filters.cc — implementation of the auth filters. Ports middlewares/auth.js.
#include "pulse/filters/auth_filters.hpp"
#include "pulse/jwt_service.hpp"
#include "pulse/cache.hpp"
#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/http_response.hpp"
#include "pulse/logger.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <openssl/sha.h>
#include <iomanip>
#include <sstream>

// The HTTP status-code constants (k401Unauthorized, k403Forbidden, …) live in
// the drogon namespace; bring them in so the handlers can use them unqualified.
using namespace drogon;

namespace pulse::filters {

namespace {
// sha256 hex of a token — used for the revoked-token denylist key, matching
// middlewares/auth.js hashToken.
std::string sha256Hex(const std::string& in) {
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(in.data()), in.size(), hash);
  std::ostringstream os;
  for (unsigned char c : hash) os << std::hex << std::setw(2) << std::setfill('0') << (int)c;
  return os.str();
}

std::string bearerToken(const HttpRequestPtr& req) {
  std::string h = req->getHeader("authorization");
  if (h.rfind("Bearer ", 0) != 0) return "";
  return h.substr(7);
}

// Resolve isActive for a userId with a Redis cache (key auth_user:<id>, 5min
// ±jitter), single-flight via getOrSet. Mirrors resolveUserActive in auth.js.
bool resolveUserActive(const std::string& userId) {
  std::string key = "auth_user:" + userId;
  if (auto cached = pulse::cache().get(key); cached && !cached->empty()) {
    return *cached == "true";
  }
  std::string val = pulse::cache().getOrSet(key, [&]() -> std::string {
    try {
      auto col = pulse::db::collection("users");
      auto oid = pulse::bsonjson::tryOid(userId);
      if (!oid) return "false";
      auto doc = col.find_one(bsoncxx::builder::basic::make_document(
          bsoncxx::builder::basic::kvp("_id", *oid)));
      bool active = false;
      if (doc) {
        auto v = doc->view();
        auto it = v.find("isActive");
        active = (it != v.end() && it->type() == bsoncxx::type::k_bool) ? it->get_bool().value : false;
      }
      return active ? "true" : "false";
    } catch (...) { return "false"; }
  }, 330); // 300 + ~jitter
  return val == "true";
}

void setUser(const HttpRequestPtr& req, const std::string& userId, const std::string& username,
             const std::string& email, bool isVerified) {
  Json::Value u(Json::objectValue);
  u["userId"] = userId;
  u["username"] = username;
  u["email"] = email;
  u["isVerified"] = isVerified;
  req->getAttributes()->insert("user", u);
}
} // namespace

void AuthFilter::doFilter(const HttpRequestPtr& req, FilterCallback&& fcb, FilterChainCallback&& fccb) {
  std::string h = req->getHeader("authorization");
  if (h.rfind("Bearer ", 0) != 0) {
    return fcb(pulse::http::error(k401Unauthorized, "Access token required", "MISSING_ACCESS_TOKEN"));
  }
  std::string token = h.substr(7);

  try {
    auto claims = pulse::jwt().verifyAccessToken(token);

    // Revoked-token denylist (best-effort; fail open if Redis is down).
    try {
      auto rev = pulse::cache().get("revoked_token:" + sha256Hex(token));
      if (rev && !rev->empty()) {
        return fcb(pulse::http::error(k401Unauthorized, "Access token revoked", "TOKEN_REVOKED"));
      }
    } catch (...) {}

    if (!resolveUserActive(claims.userId)) {
      return fcb(pulse::http::error(k401Unauthorized, "User not found or inactive", "USER_NOT_FOUND"));
    }

    setUser(req, claims.userId, claims.username, claims.email, claims.isVerified);
    return fccb();
  } catch (const pulse::JwtError& e) {
    std::string m = e.what();
    if (m.find("expired") != std::string::npos)
      return fcb(pulse::http::error(k401Unauthorized, "Access token expired", "TOKEN_EXPIRED"));
    if (m.find("Invalid") != std::string::npos)
      return fcb(pulse::http::error(k401Unauthorized, "Invalid access token", "INVALID_TOKEN"));
    return fcb(pulse::http::error(k401Unauthorized, "Authentication failed", "AUTH_FAILED"));
  } catch (...) {
    return fcb(pulse::http::error(k401Unauthorized, "Authentication failed", "AUTH_FAILED"));
  }
}

void OptionalAuthFilter::doFilter(const HttpRequestPtr& req, FilterCallback&& fcb, FilterChainCallback&& fccb) {
  std::string token = bearerToken(req);
  if (token.empty()) return fccb();
  try {
    auto claims = pulse::jwt().verifyAccessToken(token);
    if (resolveUserActive(claims.userId)) {
      setUser(req, claims.userId, claims.username, claims.email, claims.isVerified);
    }
  } catch (...) { /* silent — optional */ }
  return fccb();
}

void RequireVerifiedFilter::doFilter(const HttpRequestPtr& req, FilterCallback&& fcb, FilterChainCallback&& fccb) {
  auto attrs = req->getAttributes();
  if (!attrs->find("user")) {
    return fcb(pulse::http::error(k401Unauthorized, "Authentication required", "AUTH_REQUIRED"));
  }
  auto u = attrs->get<Json::Value>("user");
  if (!u.get("isVerified", false).asBool()) {
    return fcb(pulse::http::error(k403Forbidden, "Account verification required", "VERIFICATION_REQUIRED"));
  }
  return fccb();
}

void RequireAdminFilter::doFilter(const HttpRequestPtr& req, FilterCallback&& fcb, FilterChainCallback&& fccb) {
  auto attrs = req->getAttributes();
  if (!attrs->find("user")) {
    return fcb(pulse::http::error(k401Unauthorized, "Authentication required", "AUTH_REQUIRED"));
  }
  auto u = attrs->get<Json::Value>("user");
  try {
    auto col = pulse::db::collection("users");
    auto oid = pulse::bsonjson::tryOid(u["userId"].asString());
    if (!oid) return fcb(pulse::http::error(k403Forbidden, "Admin access required", "ADMIN_REQUIRED"));
    auto doc = col.find_one(bsoncxx::builder::basic::make_document(
        bsoncxx::builder::basic::kvp("_id", *oid)));
    std::string role;
    if (doc) {
      auto it = doc->view().find("role");
      if (it != doc->view().end() && it->type() == bsoncxx::type::k_string)
        role = std::string(it->get_string().value);
    }
    if (role != "admin") {
      return fcb(pulse::http::error(k403Forbidden, "Admin access required", "ADMIN_REQUIRED"));
    }
    return fccb();
  } catch (...) {
    return fcb(pulse::http::error(k500InternalServerError, "Authorization check failed", "AUTH_CHECK_FAILED"));
  }
}

} // namespace pulse::filters
