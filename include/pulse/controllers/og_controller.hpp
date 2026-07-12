// og_controller.hpp — Drogon HttpController porting src/controllers/ogController.js
// and the route group src/routes/ogRoutes.js (mounted at /share).
//
// These are PUBLIC, unauthenticated routes (crawlers & social-media bots need
// access), so NO filters are attached — matching the Express router which wraps
// the handlers with no middleware. Each handler renders an HTML page carrying
// Open Graph / Twitter Card meta tags plus a deep-link redirect, exactly as the
// JS controller did (responses are text/html or plain-text, NOT the JSON API
// contract):
//   GET /share/post/:postId       -> sharePost
//   GET /share/profile/:username  -> shareProfile
//   GET /share/reel/:reelId       -> shareReel
#pragma once
#include <drogon/HttpController.h>

#include <functional>
#include <string>

namespace pulse::controllers {

using namespace drogon;

class OgController : public drogon::HttpController<OgController> {
 public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(OgController::sharePost, "/share/post/{1}", Get);
  ADD_METHOD_TO(OgController::shareProfile, "/share/profile/{1}", Get);
  ADD_METHOD_TO(OgController::shareReel, "/share/reel/{1}", Get);
  METHOD_LIST_END

  // GET /share/post/:postId — OG page for a public, active post.
  void sharePost(const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback,
                 std::string postId);

  // GET /share/profile/:username — OG page for an active user profile.
  void shareProfile(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback,
                    std::string username);

  // GET /share/reel/:reelId — OG page for a reel.
  void shareReel(const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback,
                 std::string reelId);
};

}  // namespace pulse::controllers
