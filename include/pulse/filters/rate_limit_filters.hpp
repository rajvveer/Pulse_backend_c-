// rate_limit_filters.hpp — Drogon filters porting src/middlewares/rateLimit.js.
//
// Redis-backed fixed-window limiters (INCR + EXPIRE on first hit), keyed per
// USER for authenticated traffic and per IP otherwise. Health endpoints are
// skipped by the global limiter. Each limiter mirrors a named JS export with
// its window/max and 429 {success,error,code} body. Fails OPEN on Redis error
// (passOnStoreError: true).
//
// Limiters (window = 15 min for all):
//   GlobalLimiter   max=RATE_LIMIT_MAX_REQUESTS(1000)  code RATE_LIMIT_EXCEEDED
//   AuthLimiter     max=AUTH_RATE_LIMIT_MAX(100)        code AUTH_RATE_LIMIT_EXCEEDED   (IP)
//   OtpLimiter      max=OTP_RATE_LIMIT_MAX(30)          code OTP_RATE_LIMIT_EXCEEDED    (IP)
//   RefreshLimiter  max=100                             code REFRESH_RATE_LIMIT_EXCEEDED(IP)
//   UploadLimiter   max=UPLOAD_RATE_LIMIT_MAX(60)       code UPLOAD_RATE_LIMIT_EXCEEDED (user/IP)
#pragma once
#include <drogon/HttpFilter.h>
#include <string>

namespace pulse::filters {

using namespace drogon;

// Shared core: returns true (and fills resp) if the request is over the limit.
bool rateLimited(const HttpRequestPtr& req, const std::string& prefix, int max,
                 bool keyByUser, const std::string& errMsg, const std::string& errCode,
                 HttpResponsePtr& resp);

class GlobalLimiter : public HttpFilter<GlobalLimiter> {
public:
  void doFilter(const HttpRequestPtr&, FilterCallback&&, FilterChainCallback&&) override;
};
class AuthLimiter : public HttpFilter<AuthLimiter> {
public:
  void doFilter(const HttpRequestPtr&, FilterCallback&&, FilterChainCallback&&) override;
};
class OtpLimiter : public HttpFilter<OtpLimiter> {
public:
  void doFilter(const HttpRequestPtr&, FilterCallback&&, FilterChainCallback&&) override;
};
class RefreshLimiter : public HttpFilter<RefreshLimiter> {
public:
  void doFilter(const HttpRequestPtr&, FilterCallback&&, FilterChainCallback&&) override;
};
class UploadLimiter : public HttpFilter<UploadLimiter> {
public:
  void doFilter(const HttpRequestPtr&, FilterCallback&&, FilterChainCallback&&) override;
};

} // namespace pulse::filters
