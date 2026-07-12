// alter_ego_controller.hpp — Drogon HttpController porting
// src/controllers/alterEgoController.js and its route group
// src/routes/alterEgoRoutes.js (Express router mounted at /api/v1/alter-ego).
//
// Route group (every route requires auth — router.use(verifyAccessToken)):
//   GET  /me        -> getMyEgo
//   GET  /stats     -> getStats
//   PUT  /          -> update
//   POST /train     -> train
//   POST /toggle    -> toggle
//   POST /generate  -> generateResponse
//   POST /learn     -> learn
//   GET  /activity  -> getActivityLog   (Alter Ego 2.0)
//   POST /guess     -> recordGuess      (Alter Ego 2.0)
//   GET  /ai-status -> getAIStatus      (Alter Ego 2.0)
//
// Express middleware -> Drogon filter mapping:
//   verifyAccessToken -> pulse::filters::AuthFilter (applied to every route via
//                        router.use, so each ADD_METHOD_TO carries it).
//
// The authenticated user is read back from
// req->getAttributes()->get<Json::Value>("user") (fields
// userId/username/email/isVerified), exactly as the JS read req.user.
#pragma once
#include <drogon/HttpController.h>

#include <functional>

namespace pulse::controllers {

using namespace drogon;

class AlterEgoController : public drogon::HttpController<AlterEgoController> {
public:
  METHOD_LIST_BEGIN
  // router.get('/me', getMyEgo)
  ADD_METHOD_TO(AlterEgoController::getMyEgo, "/api/v1/alter-ego/me", Get,
                "pulse::filters::AuthFilter");

  // router.get('/stats', getStats)
  ADD_METHOD_TO(AlterEgoController::getStats, "/api/v1/alter-ego/stats", Get,
                "pulse::filters::AuthFilter");

  // router.put('/', update)
  ADD_METHOD_TO(AlterEgoController::update, "/api/v1/alter-ego", Put,
                "pulse::filters::AuthFilter");

  // router.post('/train', train)
  ADD_METHOD_TO(AlterEgoController::train, "/api/v1/alter-ego/train", Post,
                "pulse::filters::AuthFilter");

  // router.post('/toggle', toggle)
  ADD_METHOD_TO(AlterEgoController::toggle, "/api/v1/alter-ego/toggle", Post,
                "pulse::filters::AuthFilter");

  // router.post('/generate', generateResponse)
  ADD_METHOD_TO(AlterEgoController::generateResponse,
                "/api/v1/alter-ego/generate", Post,
                "pulse::filters::AuthFilter");

  // router.post('/learn', learn)
  ADD_METHOD_TO(AlterEgoController::learn, "/api/v1/alter-ego/learn", Post,
                "pulse::filters::AuthFilter");

  // router.get('/activity', getActivityLog)
  ADD_METHOD_TO(AlterEgoController::getActivityLog,
                "/api/v1/alter-ego/activity", Get,
                "pulse::filters::AuthFilter");

  // router.post('/guess', recordGuess)
  ADD_METHOD_TO(AlterEgoController::recordGuess, "/api/v1/alter-ego/guess", Post,
                "pulse::filters::AuthFilter");

  // router.get('/ai-status', getAIStatus)
  ADD_METHOD_TO(AlterEgoController::getAIStatus,
                "/api/v1/alter-ego/ai-status", Get,
                "pulse::filters::AuthFilter");
  METHOD_LIST_END

  // GET /api/v1/alter-ego/me
  void getMyEgo(const HttpRequestPtr& req,
                std::function<void(const HttpResponsePtr&)>&& callback);

  // GET /api/v1/alter-ego/stats
  void getStats(const HttpRequestPtr& req,
                std::function<void(const HttpResponsePtr&)>&& callback);

  // PUT /api/v1/alter-ego
  void update(const HttpRequestPtr& req,
              std::function<void(const HttpResponsePtr&)>&& callback);

  // POST /api/v1/alter-ego/train
  void train(const HttpRequestPtr& req,
             std::function<void(const HttpResponsePtr&)>&& callback);

  // POST /api/v1/alter-ego/toggle
  void toggle(const HttpRequestPtr& req,
              std::function<void(const HttpResponsePtr&)>&& callback);

  // POST /api/v1/alter-ego/generate
  void generateResponse(const HttpRequestPtr& req,
                        std::function<void(const HttpResponsePtr&)>&& callback);

  // POST /api/v1/alter-ego/learn
  void learn(const HttpRequestPtr& req,
             std::function<void(const HttpResponsePtr&)>&& callback);

  // GET /api/v1/alter-ego/activity
  void getActivityLog(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback);

  // POST /api/v1/alter-ego/guess
  void recordGuess(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback);

  // GET /api/v1/alter-ego/ai-status
  void getAIStatus(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback);
};

} // namespace pulse::controllers
