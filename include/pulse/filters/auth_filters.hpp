// auth_filters.hpp — Drogon filters porting src/middlewares/auth.js.
//
//   AuthFilter           ~ verifyAccessToken  (401 on missing/invalid/revoked)
//   OptionalAuthFilter   ~ optionalAuth       (never fails; sets user or null)
//   RequireVerifiedFilter~ requireVerified    (403 if !isVerified)
//   RequireAdminFilter   ~ requireAdmin       (403 if role != admin)
//
// On success the authenticated identity is stored on the request attributes
// under "user" as a Json::Value{ userId, username, email, isVerified } so
// controllers read it with req->getAttributes()->get<Json::Value>("user").
#pragma once
#include <drogon/HttpFilter.h>

namespace pulse::filters {

using namespace drogon;

class AuthFilter : public HttpFilter<AuthFilter> {
public:
  void doFilter(const HttpRequestPtr& req, FilterCallback&& fcb, FilterChainCallback&& fccb) override;
};

class OptionalAuthFilter : public HttpFilter<OptionalAuthFilter> {
public:
  void doFilter(const HttpRequestPtr& req, FilterCallback&& fcb, FilterChainCallback&& fccb) override;
};

class RequireVerifiedFilter : public HttpFilter<RequireVerifiedFilter> {
public:
  void doFilter(const HttpRequestPtr& req, FilterCallback&& fcb, FilterChainCallback&& fccb) override;
};

class RequireAdminFilter : public HttpFilter<RequireAdminFilter> {
public:
  void doFilter(const HttpRequestPtr& req, FilterCallback&& fcb, FilterChainCallback&& fccb) override;
};

} // namespace pulse::filters
