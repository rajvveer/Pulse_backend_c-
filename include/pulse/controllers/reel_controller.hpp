// reel_controller.hpp — Drogon HttpController port of src/controllers/reelController.js.
//
// Route group src/routes/reelRoutes.js, mounted at /api/v1/reels. Each Express
// route becomes an ADD_METHOD_TO entry whose full path is /api/v1/reels + the
// router sub-path, with the middleware chain attached as Drogon filters in the
// SAME order as the Express `router.<verb>(...)` call.
//
//   POST /create                              verifyAccessToken, uploadLimiter,
//                                             reelGuard           -> createReel
//   GET  /feed                                verifyAccessToken   -> getReelsFeed
//   POST /:reelId/like                        verifyAccessToken   -> toggleLike
//   POST /:reelId/view                        verifyAccessToken   -> trackView
//   POST /:reelId/share                       verifyAccessToken   -> shareReel
//   POST /:reelId/comments                    verifyAccessToken   -> addComment
//   GET  /:reelId/comments                    verifyAccessToken   -> getComments
//   POST /:reelId/comments/:commentId/like    verifyAccessToken   -> toggleCommentLike
//
// NOTE on the create route: the Express chain also had
//   upload.videoUpload.single('file')
// which is multer's multipart parsing, not an access-control guard. In Drogon
// the multipart body is parsed inside the handler (MultiPartParser), so it is
// not a filter; reelGuard (the in-flight-byte capacity guard) IS a filter and is
// kept. The `verifyAccessToken` + `uploadLimiter` + `reelGuard` order is preserved.
#pragma once
#include <drogon/HttpController.h>
#include <functional>
#include <string>

namespace pulse::controllers {

using namespace drogon;

class ReelController : public drogon::HttpController<ReelController> {
public:
  METHOD_LIST_BEGIN
  // 1. Create Reel (video — capacity-guarded). multer's videoUpload.single('file')
  //    is handled inline (multipart parse), not as a filter.
  ADD_METHOD_TO(ReelController::createReel, "/api/v1/reels/create", Post,
                "pulse::filters::AuthFilter", "pulse::filters::UploadLimiter",
                "pulse::filters::ReelGuard");

  // 2. Get Feed (supports ?type=foryou|following).
  ADD_METHOD_TO(ReelController::getReelsFeed, "/api/v1/reels/feed", Get,
                "pulse::filters::AuthFilter");

  // 3. Like/Unlike Reel.
  ADD_METHOD_TO(ReelController::toggleLike, "/api/v1/reels/{1}/like", Post,
                "pulse::filters::AuthFilter");

  // 4. Track View/Watch Time.
  ADD_METHOD_TO(ReelController::trackView, "/api/v1/reels/{1}/view", Post,
                "pulse::filters::AuthFilter");

  // 5. Share Reel.
  ADD_METHOD_TO(ReelController::shareReel, "/api/v1/reels/{1}/share", Post,
                "pulse::filters::AuthFilter");

  // 6. Add Comment (replies via body.parentCommentId).
  ADD_METHOD_TO(ReelController::addComment, "/api/v1/reels/{1}/comments", Post,
                "pulse::filters::AuthFilter");

  // 7. Get Comments (supports ?sort=best|top|new|controversial).
  ADD_METHOD_TO(ReelController::getComments, "/api/v1/reels/{1}/comments", Get,
                "pulse::filters::AuthFilter");

  // 8. Like/Unlike Comment.
  ADD_METHOD_TO(ReelController::toggleCommentLike,
                "/api/v1/reels/{1}/comments/{2}/like", Post,
                "pulse::filters::AuthFilter");
  METHOD_LIST_END

  // 1. POST /api/v1/reels/create
  void createReel(const HttpRequestPtr& req,
                  std::function<void(const HttpResponsePtr&)>&& callback);

  // 2. GET /api/v1/reels/feed
  void getReelsFeed(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback);

  // 3. POST /api/v1/reels/{reelId}/like
  void toggleLike(const HttpRequestPtr& req,
                  std::function<void(const HttpResponsePtr&)>&& callback,
                  std::string reelId);

  // 4. POST /api/v1/reels/{reelId}/view
  void trackView(const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback,
                 std::string reelId);

  // 5. POST /api/v1/reels/{reelId}/share
  void shareReel(const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback,
                 std::string reelId);

  // 6. POST /api/v1/reels/{reelId}/comments
  void addComment(const HttpRequestPtr& req,
                  std::function<void(const HttpResponsePtr&)>&& callback,
                  std::string reelId);

  // 7. GET /api/v1/reels/{reelId}/comments
  void getComments(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback,
                   std::string reelId);

  // 8. POST /api/v1/reels/{reelId}/comments/{commentId}/like
  void toggleCommentLike(const HttpRequestPtr& req,
                         std::function<void(const HttpResponsePtr&)>&& callback,
                         std::string reelId, std::string commentId);
};

} // namespace pulse::controllers
