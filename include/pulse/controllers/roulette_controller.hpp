// roulette_controller.hpp — Drogon HttpController porting
// src/controllers/rouletteController.js + src/routes/rouletteRoutes.js
// (mounted at /api/v1/roulette).
//
// 1:1 functional parity with the Express roulette route group. The JS router
// applies `router.use(auth.verifyAccessToken)` to EVERY route, so every handler
// here is guarded by pulse::filters::AuthFilter. The authenticated user is read
// back from the request attributes ("user") inside each handler, exactly as the
// JS read req.user.userId.
//
// Route map (FULL path = /api/v1/roulette + sub-path from rouletteRoutes.js):
//   POST /api/v1/roulette/join    -> joinQueue     (verifyAccessToken)
//   GET  /api/v1/roulette/status  -> checkStatus   (verifyAccessToken)
//   POST /api/v1/roulette/message -> sendMessage   (verifyAccessToken)
//   POST /api/v1/roulette/decide  -> decide        (verifyAccessToken)
//   POST /api/v1/roulette/leave   -> leave         (verifyAccessToken)
//   GET  /api/v1/roulette/history -> getHistory    (verifyAccessToken)
#pragma once
#include <drogon/HttpController.h>
#include <functional>

namespace pulse::controllers {

using namespace drogon;

class RouletteController : public drogon::HttpController<RouletteController> {
public:
  METHOD_LIST_BEGIN
  // POST /api/v1/roulette/join  (verifyAccessToken)
  ADD_METHOD_TO(RouletteController::joinQueue,
                "/api/v1/roulette/join", Post,
                "pulse::filters::AuthFilter");

  // GET /api/v1/roulette/status  (verifyAccessToken)
  ADD_METHOD_TO(RouletteController::checkStatus,
                "/api/v1/roulette/status", Get,
                "pulse::filters::AuthFilter");

  // POST /api/v1/roulette/message  (verifyAccessToken)
  ADD_METHOD_TO(RouletteController::sendMessage,
                "/api/v1/roulette/message", Post,
                "pulse::filters::AuthFilter");

  // POST /api/v1/roulette/decide  (verifyAccessToken)
  ADD_METHOD_TO(RouletteController::decide,
                "/api/v1/roulette/decide", Post,
                "pulse::filters::AuthFilter");

  // POST /api/v1/roulette/leave  (verifyAccessToken)
  ADD_METHOD_TO(RouletteController::leave,
                "/api/v1/roulette/leave", Post,
                "pulse::filters::AuthFilter");

  // GET /api/v1/roulette/history  (verifyAccessToken)
  ADD_METHOD_TO(RouletteController::getHistory,
                "/api/v1/roulette/history", Get,
                "pulse::filters::AuthFilter");
  METHOD_LIST_END

  // POST /api/v1/roulette/join
  void joinQueue(const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback);

  // GET /api/v1/roulette/status
  void checkStatus(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback);

  // POST /api/v1/roulette/message
  void sendMessage(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback);

  // POST /api/v1/roulette/decide
  void decide(const HttpRequestPtr& req,
              std::function<void(const HttpResponsePtr&)>&& callback);

  // POST /api/v1/roulette/leave
  void leave(const HttpRequestPtr& req,
             std::function<void(const HttpResponsePtr&)>&& callback);

  // GET /api/v1/roulette/history
  void getHistory(const HttpRequestPtr& req,
                  std::function<void(const HttpResponsePtr&)>&& callback);
};

} // namespace pulse::controllers
