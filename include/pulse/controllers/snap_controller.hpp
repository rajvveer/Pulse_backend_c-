// snap_controller.hpp — Drogon HttpController porting src/controllers/snapController.js
// and its route group src/routes/snapRoutes.js (mounted at /api/v1/snaps).
//
// Routes (Express -> Drogon), middleware chains preserved by class name:
//   POST   /                 verifyAccessToken, uploadLimiter, reelGuard, single('file') -> createSnap
//   GET    /rail             verifyAccessToken                                            -> getStoryRail
//   GET    /direct           verifyAccessToken                                            -> getDirectInbox
//   POST   /:snapId/view     verifyAccessToken                                            -> viewSnap
//   POST   /:snapId/react    verifyAccessToken                                            -> reactSnap
//   GET    /:snapId/viewers  verifyAccessToken                                            -> getViewers
//   DELETE /:snapId          verifyAccessToken                                            -> deleteSnap
//
// The multer memory-storage + reelGuard byte budget is enforced by UploadLimiter
// + Drogon's request-size limits; the file itself is read via the multipart parser.
#pragma once
#include <drogon/HttpController.h>

namespace pulse::controllers {

using namespace drogon;

class SnapController : public drogon::HttpController<SnapController> {
public:
  METHOD_LIST_BEGIN
  // Create a snap (story or direct). Multipart: file + fields.
  ADD_METHOD_TO(SnapController::createSnap, "/api/v1/snaps", Post,
                "pulse::filters::AuthFilter", "pulse::filters::UploadLimiter");

  // Story rail (own + followed authors' active stories, grouped by author).
  ADD_METHOD_TO(SnapController::getStoryRail, "/api/v1/snaps/rail", Get,
                "pulse::filters::AuthFilter");

  // Direct snap inbox (disappearing snaps sent to me).
  ADD_METHOD_TO(SnapController::getDirectInbox, "/api/v1/snaps/direct", Get,
                "pulse::filters::AuthFilter");

  // Mark viewed / react / viewers / delete.
  ADD_METHOD_TO(SnapController::viewSnap, "/api/v1/snaps/{1}/view", Post,
                "pulse::filters::AuthFilter");
  ADD_METHOD_TO(SnapController::reactSnap, "/api/v1/snaps/{1}/react", Post,
                "pulse::filters::AuthFilter");
  ADD_METHOD_TO(SnapController::getViewers, "/api/v1/snaps/{1}/viewers", Get,
                "pulse::filters::AuthFilter");
  ADD_METHOD_TO(SnapController::deleteSnap, "/api/v1/snaps/{1}", Delete,
                "pulse::filters::AuthFilter");
  METHOD_LIST_END

  void createSnap(const HttpRequestPtr& req,
                  std::function<void(const HttpResponsePtr&)>&& callback);
  void getStoryRail(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback);
  void getDirectInbox(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback);
  void viewSnap(const HttpRequestPtr& req,
                std::function<void(const HttpResponsePtr&)>&& callback,
                std::string snapId);
  void reactSnap(const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback,
                 std::string snapId);
  void getViewers(const HttpRequestPtr& req,
                  std::function<void(const HttpResponsePtr&)>&& callback,
                  std::string snapId);
  void deleteSnap(const HttpRequestPtr& req,
                  std::function<void(const HttpResponsePtr&)>&& callback,
                  std::string snapId);
};

} // namespace pulse::controllers
