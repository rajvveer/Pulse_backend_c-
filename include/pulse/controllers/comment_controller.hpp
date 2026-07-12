// comment_controller.hpp — port of routes/comments.js (mounted at /api/v1/posts).
//
// The Express route group comments.js exposes a single inline test route:
//   GET /test -> { success, message, timestamp }
// (commentController.js is an empty stub; the handler was defined inline in the
// router, so it is ported here as a controller method for parity.)
#pragma once
#include <drogon/HttpController.h>

namespace pulse::controllers {

class CommentController : public drogon::HttpController<CommentController> {
 public:
  METHOD_LIST_BEGIN
  // router.get('/test', ...) mounted at /api/v1/posts
  ADD_METHOD_TO(CommentController::test, "/api/v1/posts/test", drogon::Get);
  METHOD_LIST_END

  void test(const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback);
};

}  // namespace pulse::controllers
