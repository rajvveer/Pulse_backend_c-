// rate_limit_filters.cc — Redis fixed-window limiters. Ports middlewares/rateLimit.js.
#include "pulse/filters/rate_limit_filters.hpp"
#include "pulse/cache.hpp"
#include "pulse/config.hpp"
#include "pulse/http_response.hpp"

namespace pulse::filters {

namespace {
constexpr int kWindowSec = 15 * 60; // 15 minutes

// Key: prefix + (u:<userId> | ip:<peerAddr>). Mirrors keyByUserOrIp.
std::string limiterKey(const HttpRequestPtr& req, const std::string& prefix, bool keyByUser) {
  if (keyByUser) {
    auto attrs = req->getAttributes();
    if (attrs->find("user")) {
      auto u = attrs->get<Json::Value>("user");
      std::string id = u.get("userId", "").asString();
      if (!id.empty()) return prefix + "u:" + id;
    }
  }
  return prefix + "ip:" + req->getPeerAddr().toIp();
}
} // namespace

bool rateLimited(const HttpRequestPtr& req, const std::string& prefix, int max,
                 bool keyByUser, const std::string& errMsg, const std::string& errCode,
                 HttpResponsePtr& resp) {
  std::string key = limiterKey(req, prefix, keyByUser);
  long long count;
  try {
    count = pulse::cache().incrementRateLimit(key, kWindowSec);
  } catch (...) {
    return false; // passOnStoreError — fail open
  }
  if (count > max) {
    resp = pulse::http::error(drogon::k429TooManyRequests, errMsg, errCode);
    return true;
  }
  return false;
}

static bool skipHealth(const HttpRequestPtr& req) {
  const std::string& p = req->path();
  return p == "/health" || p == "/health/ready" || p == "/health/detailed" || p == "/status";
}

void GlobalLimiter::doFilter(const HttpRequestPtr& req, FilterCallback&& fcb, FilterChainCallback&& fccb) {
  if (skipHealth(req)) return fccb();
  int max = (int)pulse::config().envInt("RATE_LIMIT_MAX_REQUESTS", 1000);
  HttpResponsePtr resp;
  if (rateLimited(req, "rl:global:", max, true,
                  "Too many requests. Please slow down and try again later.",
                  "RATE_LIMIT_EXCEEDED", resp))
    return fcb(resp);
  return fccb();
}

void AuthLimiter::doFilter(const HttpRequestPtr& req, FilterCallback&& fcb, FilterChainCallback&& fccb) {
  int max = (int)pulse::config().envInt("AUTH_RATE_LIMIT_MAX", 100);
  HttpResponsePtr resp;
  if (rateLimited(req, "rl:auth:", max, false,
                  "Too many authentication attempts. Please try again later.",
                  "AUTH_RATE_LIMIT_EXCEEDED", resp))
    return fcb(resp);
  return fccb();
}

void OtpLimiter::doFilter(const HttpRequestPtr& req, FilterCallback&& fcb, FilterChainCallback&& fccb) {
  int max = (int)pulse::config().envInt("OTP_RATE_LIMIT_MAX", 30);
  HttpResponsePtr resp;
  if (rateLimited(req, "rl:otp:", max, false,
                  "Too many OTP requests, please try again later",
                  "OTP_RATE_LIMIT_EXCEEDED", resp))
    return fcb(resp);
  return fccb();
}

void RefreshLimiter::doFilter(const HttpRequestPtr& req, FilterCallback&& fcb, FilterChainCallback&& fccb) {
  HttpResponsePtr resp;
  if (rateLimited(req, "rl:refresh:", 100, false,
                  "Too many token refresh attempts. Please try again later.",
                  "REFRESH_RATE_LIMIT_EXCEEDED", resp))
    return fcb(resp);
  return fccb();
}

void UploadLimiter::doFilter(const HttpRequestPtr& req, FilterCallback&& fcb, FilterChainCallback&& fccb) {
  int max = (int)pulse::config().envInt("UPLOAD_RATE_LIMIT_MAX", 60);
  HttpResponsePtr resp;
  if (rateLimited(req, "rl:upload:", max, true,
                  "Too many uploads. Please try again later.",
                  "UPLOAD_RATE_LIMIT_EXCEEDED", resp))
    return fcb(resp);
  return fccb();
}

} // namespace pulse::filters
