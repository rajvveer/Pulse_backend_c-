// comment_controller.cc — port of routes/comments.js (mounted at /api/v1/posts).
#include "pulse/controllers/comment_controller.hpp"

#include "pulse/bson_json.hpp"
#include "pulse/http_response.hpp"

namespace pulse::controllers {

// GET /api/v1/posts/test
// Express:
//   router.get('/test', (req, res) => {
//     res.json({
//       success: true,
//       message: 'Comments API working!',
//       timestamp: new Date().toISOString()
//     });
//   });
void CommentController::test(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  (void)req;
  Json::Value body(Json::objectValue);
  body["message"] = "Comments API working!";
  body["timestamp"] = pulse::bsonjson::nowIso8601();
  // pulse::http::success merges these fields under { success: true, ... }.
  callback(pulse::http::success(std::move(body)));
}

}  // namespace pulse::controllers
