// feed_controller.hpp — Drogon HttpController porting src/controllers/feedController.js
// and the route group src/routes/feed.js (mounted at /api/v1/feed).
//
// Every endpoint requires verifyAccessToken (-> pulse::filters::AuthFilter).
// The handlers are thin: they parse the query params / authed user, then delegate
// to pulse::services::feed (which owns caching, candidate generation, ranking,
// pagination, and post-processing — verbatim from the JS module). On any thrown
// error they reply with res.status(500).json({ success:false, message:'Failed to
// load feed' }), matching the Express try/catch.
//
// Routes (full path = /api/v1/feed + sub-path from feed.js):
//   GET /api/v1/feed/foryou     -> getForYouFeed
//   GET /api/v1/feed/following  -> getFollowingFeed
//   GET /api/v1/feed/global     -> getGlobalFeed
//   GET /api/v1/feed/home       -> getHomeFeed
//   GET /api/v1/feed/trending   -> getTrendingPosts
//   GET /api/v1/feed/nearby     -> getNearbyPosts
#pragma once
#include <drogon/HttpController.h>

namespace pulse::controllers {

class FeedController : public drogon::HttpController<FeedController> {
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(FeedController::getForYouFeed, "/api/v1/feed/foryou", drogon::Get,
                "pulse::filters::AuthFilter");
  ADD_METHOD_TO(FeedController::getFollowingFeed, "/api/v1/feed/following", drogon::Get,
                "pulse::filters::AuthFilter");
  ADD_METHOD_TO(FeedController::getGlobalFeed, "/api/v1/feed/global", drogon::Get,
                "pulse::filters::AuthFilter");
  ADD_METHOD_TO(FeedController::getHomeFeed, "/api/v1/feed/home", drogon::Get,
                "pulse::filters::AuthFilter");
  ADD_METHOD_TO(FeedController::getTrendingPosts, "/api/v1/feed/trending", drogon::Get,
                "pulse::filters::AuthFilter");
  ADD_METHOD_TO(FeedController::getNearbyPosts, "/api/v1/feed/nearby", drogon::Get,
                "pulse::filters::AuthFilter");
  METHOD_LIST_END

  // GET /api/v1/feed/foryou — personalized discovery feed.
  void getForYouFeed(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback);

  // GET /api/v1/feed/following — chronological posts from followed users.
  void getFollowingFeed(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

  // GET /api/v1/feed/global — all public posts with light ranking.
  void getGlobalFeed(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback);

  // GET /api/v1/feed/home — following + own posts, algorithm-ranked.
  void getHomeFeed(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& callback);

  // GET /api/v1/feed/trending — velocity-based trending posts.
  void getTrendingPosts(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

  // GET /api/v1/feed/nearby — location-based posts.
  void getNearbyPosts(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback);
};

} // namespace pulse::controllers
