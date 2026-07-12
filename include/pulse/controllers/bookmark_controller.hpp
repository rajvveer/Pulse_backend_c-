// bookmark_controller.hpp — Drogon HttpController port of
// src/controllers/bookmarkController.js.
//
// Route group src/routes/bookmarkRoutes.js, mounted at /api/v1/bookmarks. The
// router applies `router.use(verifyAccessToken)` to the whole group, so every
// endpoint carries the AuthFilter. Each Express route becomes an ADD_METHOD_TO
// entry whose full path is /api/v1/bookmarks + the router sub-path.
//
//   POST /                 verifyAccessToken -> toggleBookmark
//   GET  /                 verifyAccessToken -> getBookmarks
//   GET  /check/:itemId    verifyAccessToken -> checkBookmark
//
// Drogon registers the root sub-path ('/') as the bare mount path
// "/api/v1/bookmarks" (no trailing slash); Drogon matches a request to
// "/api/v1/bookmarks/" against this path as well.
#pragma once
#include <drogon/HttpController.h>
#include <functional>
#include <string>

namespace pulse::controllers {

using namespace drogon;

class BookmarkController : public drogon::HttpController<BookmarkController> {
 public:
  METHOD_LIST_BEGIN
  // router.post('/', bookmarkController.toggleBookmark)
  ADD_METHOD_TO(BookmarkController::toggleBookmark, "/api/v1/bookmarks", Post,
                "pulse::filters::AuthFilter");

  // router.get('/', bookmarkController.getBookmarks)
  ADD_METHOD_TO(BookmarkController::getBookmarks, "/api/v1/bookmarks", Get,
                "pulse::filters::AuthFilter");

  // router.get('/check/:itemId', bookmarkController.checkBookmark)
  ADD_METHOD_TO(BookmarkController::checkBookmark,
                "/api/v1/bookmarks/check/{1}", Get,
                "pulse::filters::AuthFilter");
  METHOD_LIST_END

  // POST /api/v1/bookmarks
  void toggleBookmark(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback);

  // GET /api/v1/bookmarks?type=post|reel
  void getBookmarks(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback);

  // GET /api/v1/bookmarks/check/{itemId}
  void checkBookmark(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback,
                     std::string itemId);
};

}  // namespace pulse::controllers
