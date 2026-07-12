// auth_controller.hpp — Drogon HttpController porting src/controllers/authController.js
// and the route group src/routes/auth.js (mounted at /api/v1/auth).
//
// 1:1 functional parity with the Express router: same sub-paths, HTTP methods,
// ordered middleware (as filters), request/response JSON shapes, status codes,
// and error `code` strings.
//
// Middleware -> filter mapping (preserved in order):
//   authRateLimit (authLimiter) -> pulse::filters::AuthLimiter
//   otpRateLimit  (otpLimiter)  -> pulse::filters::OtpLimiter
//   refreshLimiter              -> pulse::filters::RefreshLimiter
//   verifyAccessToken           -> pulse::filters::AuthFilter
#pragma once
#include <drogon/HttpController.h>

namespace pulse::controllers {

using namespace drogon;

class AuthController : public drogon::HttpController<AuthController> {
public:
  METHOD_LIST_BEGIN
  // Public routes
  ADD_METHOD_TO(AuthController::initiateAuth,
                "/api/v1/auth/initiate", Post, "pulse::filters::AuthLimiter");
  ADD_METHOD_TO(AuthController::verifyOTP,
                "/api/v1/auth/verify-otp", Post, "pulse::filters::AuthLimiter");
  ADD_METHOD_TO(AuthController::createUsername,
                "/api/v1/auth/create-username", Post, "pulse::filters::AuthLimiter");
  ADD_METHOD_TO(AuthController::refreshToken,
                "/api/v1/auth/refresh-token", Post, "pulse::filters::RefreshLimiter");
  ADD_METHOD_TO(AuthController::resendOTP,
                "/api/v1/auth/resend-otp", Post, "pulse::filters::OtpLimiter");
  ADD_METHOD_TO(AuthController::checkUsername,
                "/api/v1/auth/check-username", Get);

  // Firebase Login Route
  ADD_METHOD_TO(AuthController::firebaseLogin,
                "/api/v1/auth/firebase-login", Post, "pulse::filters::AuthLimiter");

  // Protected routes
  ADD_METHOD_TO(AuthController::getCurrentUser,
                "/api/v1/auth/me", Get, "pulse::filters::AuthFilter");
  ADD_METHOD_TO(AuthController::logout,
                "/api/v1/auth/logout", Post, "pulse::filters::AuthFilter");

  // Test route (inline handler in the Express router)
  ADD_METHOD_TO(AuthController::test,
                "/api/v1/auth/test", Get);
  METHOD_LIST_END

  void initiateAuth(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback);
  void verifyOTP(const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback);
  void createUsername(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback);
  void refreshToken(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback);
  void resendOTP(const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback);
  void checkUsername(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback);
  void firebaseLogin(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback);
  void getCurrentUser(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback);
  void logout(const HttpRequestPtr& req,
              std::function<void(const HttpResponsePtr&)>&& callback);
  void test(const HttpRequestPtr& req,
            std::function<void(const HttpResponsePtr&)>&& callback);
};

}  // namespace pulse::controllers
