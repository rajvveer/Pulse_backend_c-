// whisper_controller.hpp — Drogon HttpController port of
// src/controllers/whisperController.js + src/routes/whisperRoutes.js
// (Express router mounted at /api/v1/whispers).
//
// Route group (all routes require auth — router.use(verifyAccessToken)):
//   GET  /nearby                 -> getNearby
//   POST /                       -> create
//   POST /:whisperId/vote        -> vote
//   POST /:whisperId/reply       -> reply
//   POST /:whisperId/report      -> report
//
// Express middleware -> Drogon filter mapping:
//   verifyAccessToken -> pulse::filters::AuthFilter (applied to every route).
//
// The authenticated user is read back from req->getAttributes()->get<Json::Value>
// ("user") (fields userId/username/email/isVerified), exactly as the JS read
// req.user.
#pragma once
#include <drogon/HttpController.h>

#include <functional>
#include <string>

namespace pulse::controllers {

class WhisperController : public drogon::HttpController<WhisperController> {
public:
  METHOD_LIST_BEGIN
  // router.get('/nearby', getNearby)
  ADD_METHOD_TO(WhisperController::getNearby,
                "/api/v1/whispers/nearby", drogon::Get,
                "pulse::filters::AuthFilter");

  // router.post('/', create)
  ADD_METHOD_TO(WhisperController::create,
                "/api/v1/whispers", drogon::Post,
                "pulse::filters::AuthFilter");

  // router.post('/:whisperId/vote', vote)
  ADD_METHOD_TO(WhisperController::vote,
                "/api/v1/whispers/{1}/vote", drogon::Post,
                "pulse::filters::AuthFilter");

  // router.post('/:whisperId/reply', reply)
  ADD_METHOD_TO(WhisperController::reply,
                "/api/v1/whispers/{1}/reply", drogon::Post,
                "pulse::filters::AuthFilter");

  // router.post('/:whisperId/report', report)
  ADD_METHOD_TO(WhisperController::report,
                "/api/v1/whispers/{1}/report", drogon::Post,
                "pulse::filters::AuthFilter");
  METHOD_LIST_END

  // GET /api/v1/whispers/nearby
  void getNearby(const drogon::HttpRequestPtr& req,
                 std::function<void(const drogon::HttpResponsePtr&)>&& callback);

  // POST /api/v1/whispers
  void create(const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& callback);

  // POST /api/v1/whispers/:whisperId/vote
  void vote(const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback,
            std::string whisperId);

  // POST /api/v1/whispers/:whisperId/reply
  void reply(const drogon::HttpRequestPtr& req,
             std::function<void(const drogon::HttpResponsePtr&)>&& callback,
             std::string whisperId);

  // POST /api/v1/whispers/:whisperId/report
  void report(const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& callback,
              std::string whisperId);
};

} // namespace pulse::controllers
