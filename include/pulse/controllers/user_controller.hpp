// user_controller.hpp — Drogon HttpController porting src/controllers/userController.js
// and the route group src/routes/users.js (mounted at /api/v1/users).
//
// 1:1 functional parity with the Express router: same sub-paths, HTTP methods,
// ordered middleware (as filters), request/response JSON shapes, status codes,
// and (where present) error `code` strings. The JS userController returns the
// bare shapes { success:true, data } / { success:true, message } and the error
// shape { success:false, message } (NO `error`/`code` fields), so those exact
// shapes are reproduced here rather than the {error,code} helper shape.
//
// Middleware -> filter mapping (preserved in order):
//   verifyAccessToken -> pulse::filters::AuthFilter
//   uploadLimiter     -> pulse::filters::UploadLimiter
// (upload.uploadGuard / upload.single('avatar') are the multipart middlewares;
//  the multipart body is read off the Drogon request in the uploadAvatar handler.)
//
// Route ORDER matters: literal /search, /onboarding/*, /me, /me/* are declared
// before the /{username} param routes so the param matcher does not swallow them.
#pragma once
#include <drogon/HttpController.h>

namespace pulse::controllers {

using namespace drogon;

class UserController : public drogon::HttpController<UserController> {
 public:
  METHOD_LIST_BEGIN
  // router.get('/search', verifyAccessToken, ...)  — MUST be first.
  ADD_METHOD_TO(UserController::searchUsers,
                "/api/v1/users/search", Get, "pulse::filters::AuthFilter");

  // Onboarding
  ADD_METHOD_TO(UserController::getOnboardingOptions,
                "/api/v1/users/onboarding/options", Get, "pulse::filters::AuthFilter");
  ADD_METHOD_TO(UserController::submitOnboarding,
                "/api/v1/users/onboarding", Post, "pulse::filters::AuthFilter");

  // Current user profile
  ADD_METHOD_TO(UserController::getCurrentUser,
                "/api/v1/users/me", Get, "pulse::filters::AuthFilter");

  // Account management — declared BEFORE /{username} routes.
  ADD_METHOD_TO(UserController::changePassword,
                "/api/v1/users/me/password", Patch, "pulse::filters::AuthFilter");
  ADD_METHOD_TO(UserController::deleteAccount,
                "/api/v1/users/me", Delete, "pulse::filters::AuthFilter");
  ADD_METHOD_TO(UserController::getBlockedUsers,
                "/api/v1/users/me/blocked", Get, "pulse::filters::AuthFilter");

  // Get user profile by username
  ADD_METHOD_TO(UserController::getUserByUsername,
                "/api/v1/users/{1}", Get, "pulse::filters::AuthFilter");

  // Get user posts
  ADD_METHOD_TO(UserController::getUserPosts,
                "/api/v1/users/{1}/posts", Get, "pulse::filters::AuthFilter");

  // Update current user profile
  ADD_METHOD_TO(UserController::updateProfile,
                "/api/v1/users/me", Patch, "pulse::filters::AuthFilter");

  // Upload avatar (uploadLimiter + multipart guard/single)
  ADD_METHOD_TO(UserController::uploadAvatar,
                "/api/v1/users/me/avatar", Post,
                "pulse::filters::AuthFilter", "pulse::filters::UploadLimiter");

  // Follow / Unfollow
  ADD_METHOD_TO(UserController::toggleFollow,
                "/api/v1/users/{1}/follow", Post, "pulse::filters::AuthFilter");

  // Block / Unblock
  ADD_METHOD_TO(UserController::blockUser,
                "/api/v1/users/{1}/block", Post, "pulse::filters::AuthFilter");
  ADD_METHOD_TO(UserController::unblockUser,
                "/api/v1/users/{1}/block", Delete, "pulse::filters::AuthFilter");

  // Lists
  ADD_METHOD_TO(UserController::getFollowers,
                "/api/v1/users/{1}/followers", Get, "pulse::filters::AuthFilter");
  ADD_METHOD_TO(UserController::getFollowing,
                "/api/v1/users/{1}/following", Get, "pulse::filters::AuthFilter");
  METHOD_LIST_END

  // ── Handlers ──────────────────────────────────────────────────────────────
  void searchUsers(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback);
  void getOnboardingOptions(const HttpRequestPtr& req,
                            std::function<void(const HttpResponsePtr&)>&& callback);
  void submitOnboarding(const HttpRequestPtr& req,
                        std::function<void(const HttpResponsePtr&)>&& callback);
  void getCurrentUser(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback);
  void changePassword(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback);
  void deleteAccount(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback);
  void getBlockedUsers(const HttpRequestPtr& req,
                       std::function<void(const HttpResponsePtr&)>&& callback);
  void getUserByUsername(const HttpRequestPtr& req,
                         std::function<void(const HttpResponsePtr&)>&& callback,
                         std::string username);
  void getUserPosts(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback,
                    std::string username);
  void updateProfile(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback);
  void uploadAvatar(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback);
  void toggleFollow(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback,
                    std::string username);
  void blockUser(const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback,
                 std::string username);
  void unblockUser(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback,
                   std::string username);
  void getFollowers(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback,
                    std::string username);
  void getFollowing(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback,
                    std::string username);
};

}  // namespace pulse::controllers
