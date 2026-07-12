// post_controller.hpp — Drogon HttpController port of src/controllers/postController.js
// and its route group src/routes/posts.js (mounted at /api/v1/posts).
//
// 1:1 functional parity with the Express controller: same sub-paths, same HTTP
// methods, same ordered middleware chain (verifyAccessToken -> AuthFilter on
// every route), same request/response JSON shapes, status codes, and error
// messages. Business logic delegates to the already-ported models/services:
//   pulse::models::{post,comment,like,follow,bookmark,user,notification,
//                   userbehavior,userengagement,pulsescore}
//   pulse::FeedbackService (feedbackService.recordEngagement)
//   pulse::cache() (cacheService.getOrSet for trending hashtags)
//
// Route table (Express posts.js, in declaration order — order matters because
// /search and /trending must be matched before /:postId):
//   POST   /                                  createPost
//   GET    /search                            searchPosts
//   GET    /trending                          getTrendingHashtags
//   GET    /:postId                           getPost
//   PATCH  /:postId                           updatePost
//   DELETE /:postId                           deletePost
//   GET    /me/posts                          getMyPosts
//   GET    /user/:username                    getUserPosts
//   POST   /:postId/like                      toggleLike
//   POST   /:postId/comments                  addComment
//   GET    /:postId/comments                  getComments
//   POST   /:postId/comments/:commentId/like  toggleCommentLike
#pragma once
#include <drogon/HttpController.h>

namespace pulse::controllers {

using namespace drogon;

class PostController : public drogon::HttpController<PostController> {
public:
  METHOD_LIST_BEGIN
  // router.post('/', verifyAccessToken, createPost)
  ADD_METHOD_TO(PostController::createPost,
                "/api/v1/posts", Post, "pulse::filters::AuthFilter");

  // router.get('/search', verifyAccessToken, searchPosts)
  ADD_METHOD_TO(PostController::searchPosts,
                "/api/v1/posts/search", Get, "pulse::filters::AuthFilter");

  // router.get('/trending', verifyAccessToken, getTrendingHashtags)
  ADD_METHOD_TO(PostController::getTrendingHashtags,
                "/api/v1/posts/trending", Get, "pulse::filters::AuthFilter");

  // router.get('/me/posts', verifyAccessToken, getMyPosts)
  // (declared before /:postId so the static segment wins the route match)
  ADD_METHOD_TO(PostController::getMyPosts,
                "/api/v1/posts/me/posts", Get, "pulse::filters::AuthFilter");

  // router.get('/user/:username', verifyAccessToken, getUserPosts)
  ADD_METHOD_TO(PostController::getUserPosts,
                "/api/v1/posts/user/{1}", Get, "pulse::filters::AuthFilter");

  // router.post('/:postId/like', verifyAccessToken, toggleLike)
  ADD_METHOD_TO(PostController::toggleLike,
                "/api/v1/posts/{1}/like", Post, "pulse::filters::AuthFilter");

  // router.post('/:postId/comments', verifyAccessToken, addComment)
  ADD_METHOD_TO(PostController::addComment,
                "/api/v1/posts/{1}/comments", Post, "pulse::filters::AuthFilter");

  // router.get('/:postId/comments', verifyAccessToken, getComments)
  ADD_METHOD_TO(PostController::getComments,
                "/api/v1/posts/{1}/comments", Get, "pulse::filters::AuthFilter");

  // router.post('/:postId/comments/:commentId/like', verifyAccessToken, toggleCommentLike)
  ADD_METHOD_TO(PostController::toggleCommentLike,
                "/api/v1/posts/{1}/comments/{2}/like", Post, "pulse::filters::AuthFilter");

  // router.get('/:postId', verifyAccessToken, getPost)
  ADD_METHOD_TO(PostController::getPost,
                "/api/v1/posts/{1}", Get, "pulse::filters::AuthFilter");

  // router.patch('/:postId', verifyAccessToken, updatePost)
  ADD_METHOD_TO(PostController::updatePost,
                "/api/v1/posts/{1}", Patch, "pulse::filters::AuthFilter");

  // router.delete('/:postId', verifyAccessToken, deletePost)
  ADD_METHOD_TO(PostController::deletePost,
                "/api/v1/posts/{1}", Delete, "pulse::filters::AuthFilter");
  METHOD_LIST_END

  // POST   /api/v1/posts
  void createPost(const HttpRequestPtr& req,
                  std::function<void(const HttpResponsePtr&)>&& callback);

  // GET    /api/v1/posts/search
  void searchPosts(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback);

  // GET    /api/v1/posts/trending
  void getTrendingHashtags(const HttpRequestPtr& req,
                           std::function<void(const HttpResponsePtr&)>&& callback);

  // GET    /api/v1/posts/me/posts
  void getMyPosts(const HttpRequestPtr& req,
                  std::function<void(const HttpResponsePtr&)>&& callback);

  // GET    /api/v1/posts/user/:username
  void getUserPosts(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback,
                    std::string username);

  // POST   /api/v1/posts/:postId/like
  void toggleLike(const HttpRequestPtr& req,
                  std::function<void(const HttpResponsePtr&)>&& callback,
                  std::string postId);

  // POST   /api/v1/posts/:postId/comments
  void addComment(const HttpRequestPtr& req,
                  std::function<void(const HttpResponsePtr&)>&& callback,
                  std::string postId);

  // GET    /api/v1/posts/:postId/comments
  void getComments(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback,
                   std::string postId);

  // POST   /api/v1/posts/:postId/comments/:commentId/like
  void toggleCommentLike(const HttpRequestPtr& req,
                         std::function<void(const HttpResponsePtr&)>&& callback,
                         std::string postId, std::string commentId);

  // GET    /api/v1/posts/:postId
  void getPost(const HttpRequestPtr& req,
               std::function<void(const HttpResponsePtr&)>&& callback,
               std::string postId);

  // PATCH  /api/v1/posts/:postId
  void updatePost(const HttpRequestPtr& req,
                  std::function<void(const HttpResponsePtr&)>&& callback,
                  std::string postId);

  // DELETE /api/v1/posts/:postId
  void deletePost(const HttpRequestPtr& req,
                  std::function<void(const HttpResponsePtr&)>&& callback,
                  std::string postId);
};

} // namespace pulse::controllers
