// gif_controller.hpp — Drogon HttpController porting src/controllers/gifController.js
// and the route group src/routes/gifs.js (mounted at /api/v1/gifs).
//
// All three routes are GET and require authentication (the router does
// `router.use(verifyAccessToken)` before declaring any route), so every method
// carries the "pulse::filters::AuthFilter" filter. The endpoints proxy Tenor's
// public GIF API (search / featured / categories) exactly as the JS did:
//   GET /api/v1/gifs/search      -> searchGifs
//   GET /api/v1/gifs/trending    -> getTrendingGifs
//   GET /api/v1/gifs/categories  -> getCategories
//
// Response shapes mirror the JS res.json 1:1 (note: the GIF endpoints use a
// `message` field rather than the standard error `error`/`code` contract, so the
// implementation builds those bodies directly with pulse::http::json).
#pragma once
#include <drogon/HttpController.h>

namespace pulse::controllers {

using namespace drogon;

class GifController : public drogon::HttpController<GifController> {
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(GifController::searchGifs, "/api/v1/gifs/search", Get,
                "pulse::filters::AuthFilter");
  ADD_METHOD_TO(GifController::getTrendingGifs, "/api/v1/gifs/trending", Get,
                "pulse::filters::AuthFilter");
  ADD_METHOD_TO(GifController::getCategories, "/api/v1/gifs/categories", Get,
                "pulse::filters::AuthFilter");
  METHOD_LIST_END

  // GET /api/v1/gifs/search — search GIFs via Tenor /search.
  void searchGifs(const HttpRequestPtr& req,
                  std::function<void(const HttpResponsePtr&)>&& callback);

  // GET /api/v1/gifs/trending — trending/featured GIFs via Tenor /featured.
  void getTrendingGifs(const HttpRequestPtr& req,
                       std::function<void(const HttpResponsePtr&)>&& callback);

  // GET /api/v1/gifs/categories — GIF categories via Tenor /categories.
  void getCategories(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback);
};

}  // namespace pulse::controllers
