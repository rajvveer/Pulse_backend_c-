// rate_limit_filters.cc — Redis fixed-window limiters. Ports middlewares/rateLimit.js.
#include "pulse/filters/rate_limit_filters.hpp"
#include "pulse/cache.hpp"
#include "pulse/config.hpp"
#include "pulse/http_response.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

namespace pulse::filters {

namespace {
bool isSafeIpText(const std::string& value) {
  if (value.empty() || value.size() > 64) return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return std::isxdigit(c) || c == '.' || c == ':';
  });
}

bool isPrivateOrLoopback(const std::string& ip) {
  if (ip == "::1" || ip == "127.0.0.1" || ip.rfind("127.", 0) == 0 ||
      ip.rfind("10.", 0) == 0 || ip.rfind("192.168.", 0) == 0 ||
      ip.rfind("fc", 0) == 0 || ip.rfind("fd", 0) == 0) {
    return true;
  }
  if (ip.rfind("172.", 0) == 0) {
    const auto dot = ip.find('.', 4);
    if (dot != std::string::npos) {
      try {
        const int second = std::stoi(ip.substr(4, dot - 4));
        return second >= 16 && second <= 31;
      } catch (...) {
      }
    }
  }
  // Docker may expose IPv4 peers as IPv4-mapped IPv6 addresses.
  if (ip.rfind("::ffff:", 0) == 0)
    return isPrivateOrLoopback(ip.substr(7));
  return false;
}

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t");
  if (first == std::string::npos) return "";
  const auto last = value.find_last_not_of(" \t");
  return value.substr(first, last - first + 1);
}

int requestLimit(const char* key, int64_t fallback) {
  return static_cast<int>(std::clamp<int64_t>(
      pulse::config().envInt(key, fallback), 1, 1000000));
}

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
  return prefix + "ip:" + clientIp(req);
}
} // namespace

std::string clientIp(const HttpRequestPtr& req) {
  const std::string peer = req->getPeerAddr().toIp();
  int64_t hops = pulse::config().envInt("TRUST_PROXY_HOPS", 0);
  if (hops <= 0 || hops > 8 || !isPrivateOrLoopback(peer)) return peer;

  const std::string forwarded = req->getHeader("x-forwarded-for");
  if (forwarded.empty()) return peer;
  std::vector<std::string> chain;
  std::stringstream ss(forwarded);
  std::string item;
  while (std::getline(ss, item, ',')) {
    item = trim(item);
    if (!isSafeIpText(item)) return peer;
    chain.push_back(item);
  }
  if (chain.size() < static_cast<size_t>(hops)) return peer;
  return chain[chain.size() - static_cast<size_t>(hops)];
}

bool rateLimited(const HttpRequestPtr& req, const std::string& prefix, int max,
                 bool keyByUser, const std::string& errMsg, const std::string& errCode,
                 HttpResponsePtr& resp) {
  std::string key = limiterKey(req, prefix, keyByUser);
  long long count;
  try {
    const int64_t windowMs = std::clamp<int64_t>(
        pulse::config().envInt("RATE_LIMIT_WINDOW_MS", 15 * 60 * 1000),
        1000, 24LL * 60 * 60 * 1000);
    const int windowSec = static_cast<int>(windowMs / 1000);
    count = pulse::cache().incrementRateLimit(key, windowSec);
  } catch (...) {
    return false; // passOnStoreError — fail open
  }
  const int boundedMax = std::clamp(max, 1, 1000000);
  if (count > boundedMax) {
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
  const int max = requestLimit("RATE_LIMIT_MAX_REQUESTS", 1000);
  HttpResponsePtr resp;
  if (rateLimited(req, "rl:global:", max, true,
                  "Too many requests. Please slow down and try again later.",
                  "RATE_LIMIT_EXCEEDED", resp))
    return fcb(resp);
  return fccb();
}

void AuthLimiter::doFilter(const HttpRequestPtr& req, FilterCallback&& fcb, FilterChainCallback&& fccb) {
  const int max = requestLimit("AUTH_RATE_LIMIT_MAX", 100);
  HttpResponsePtr resp;
  if (rateLimited(req, "rl:auth:", max, false,
                  "Too many authentication attempts. Please try again later.",
                  "AUTH_RATE_LIMIT_EXCEEDED", resp))
    return fcb(resp);
  return fccb();
}

void OtpLimiter::doFilter(const HttpRequestPtr& req, FilterCallback&& fcb, FilterChainCallback&& fccb) {
  const int max = requestLimit("OTP_RATE_LIMIT_MAX", 30);
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
  const int max = requestLimit("UPLOAD_RATE_LIMIT_MAX", 60);
  HttpResponsePtr resp;
  if (rateLimited(req, "rl:upload:", max, true,
                  "Too many uploads. Please try again later.",
                  "UPLOAD_RATE_LIMIT_EXCEEDED", resp))
    return fcb(resp);
  return fccb();
}

} // namespace pulse::filters
