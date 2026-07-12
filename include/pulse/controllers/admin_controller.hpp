// admin_controller.hpp — Drogon HttpController port of src/controllers/adminController.js
// mounted at /api/v1/admin (src/routes/adminRoutes.js).
//
// Route group (Express -> Drogon):
//   POST   /login                       authLimiter                      -> login
//   --- below all require verifyAccessToken + requireAdmin ---
//   GET    /stats                                                        -> getStats
//   GET    /users                                                        -> listUsers
//   PATCH  /users/:userId/status                                         -> updateUserStatus
//   PATCH  /users/:userId/role                                           -> updateUserRole
//   GET    /posts                                                        -> listPosts
//   PATCH  /posts/:postId/status                                         -> updatePostStatus
//   GET    /reels                                                        -> listReels
//   DELETE /reels/:reelId                                                -> deleteReel
//   GET    /drops                                                        -> listDrops
//   PATCH  /drops/:dropId/expire                                         -> expireDrop
//
// NOTE: POST /drops is handled by pulseDropController.createDrop (a different
// controller) and is therefore registered by the PulseDrop controller, not here,
// matching how the JS route file delegates that one path.
#pragma once
#include <drogon/HttpController.h>

namespace pulse::controllers {

using namespace drogon;

class AdminController : public drogon::HttpController<AdminController> {
public:
  METHOD_LIST_BEGIN
  // Public: admin login (rate-limited like other auth endpoints).
  ADD_METHOD_TO(AdminController::login, "/api/v1/admin/login", Post,
                "pulse::filters::AuthLimiter");

  // Dashboard.
  ADD_METHOD_TO(AdminController::getStats, "/api/v1/admin/stats", Get,
                "pulse::filters::AuthFilter", "pulse::filters::RequireAdminFilter");

  // Users.
  ADD_METHOD_TO(AdminController::listUsers, "/api/v1/admin/users", Get,
                "pulse::filters::AuthFilter", "pulse::filters::RequireAdminFilter");
  ADD_METHOD_TO(AdminController::getUser, "/api/v1/admin/users/{1}", Get,
                "pulse::filters::AuthFilter", "pulse::filters::RequireAdminFilter");
  ADD_METHOD_TO(AdminController::updateUserStatus, "/api/v1/admin/users/{1}/status", Patch,
                "pulse::filters::AuthFilter", "pulse::filters::RequireAdminFilter");
  ADD_METHOD_TO(AdminController::updateUserRole, "/api/v1/admin/users/{1}/role", Patch,
                "pulse::filters::AuthFilter", "pulse::filters::RequireAdminFilter");

  // Posts.
  ADD_METHOD_TO(AdminController::listPosts, "/api/v1/admin/posts", Get,
                "pulse::filters::AuthFilter", "pulse::filters::RequireAdminFilter");
  ADD_METHOD_TO(AdminController::getPost, "/api/v1/admin/posts/{1}", Get,
                "pulse::filters::AuthFilter", "pulse::filters::RequireAdminFilter");
  ADD_METHOD_TO(AdminController::updatePostStatus, "/api/v1/admin/posts/{1}/status", Patch,
                "pulse::filters::AuthFilter", "pulse::filters::RequireAdminFilter");

  // Reels.
  ADD_METHOD_TO(AdminController::listReels, "/api/v1/admin/reels", Get,
                "pulse::filters::AuthFilter", "pulse::filters::RequireAdminFilter");
  ADD_METHOD_TO(AdminController::getReel, "/api/v1/admin/reels/{1}", Get,
                "pulse::filters::AuthFilter", "pulse::filters::RequireAdminFilter");
  ADD_METHOD_TO(AdminController::deleteReel, "/api/v1/admin/reels/{1}", Delete,
                "pulse::filters::AuthFilter", "pulse::filters::RequireAdminFilter");

  // Pulse Drops.
  ADD_METHOD_TO(AdminController::listDrops, "/api/v1/admin/drops", Get,
                "pulse::filters::AuthFilter", "pulse::filters::RequireAdminFilter");
  ADD_METHOD_TO(AdminController::getDrop, "/api/v1/admin/drops/{1}", Get,
                "pulse::filters::AuthFilter", "pulse::filters::RequireAdminFilter");
  ADD_METHOD_TO(AdminController::expireDrop, "/api/v1/admin/drops/{1}/expire", Patch,
                "pulse::filters::AuthFilter", "pulse::filters::RequireAdminFilter");
  METHOD_LIST_END

  void login(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);
  void getStats(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);

  void listUsers(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);
  void getUser(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback,
               std::string userId);
  void updateUserStatus(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback,
                        std::string userId);
  void updateUserRole(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback,
                      std::string userId);

  void listPosts(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);
  void getPost(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback,
               std::string postId);
  void updatePostStatus(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback,
                        std::string postId);

  void listReels(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);
  void getReel(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback,
               std::string reelId);
  void deleteReel(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback,
                  std::string reelId);

  void listDrops(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback);
  void getDrop(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback,
               std::string dropId);
  void expireDrop(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback,
                  std::string dropId);
};

} // namespace pulse::controllers
