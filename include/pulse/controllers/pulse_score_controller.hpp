// pulse_score_controller.hpp — Drogon HttpController porting
// src/controllers/pulseScoreController.js and its route group
// src/routes/pulseScoreRoutes.js (mounted at /api/v1/pulse-score).
//
// The Express router gates EVERY route with `router.use(auth.verifyAccessToken)`,
// so each handler carries pulse::filters::AuthFilter. The handlers are thin: they
// read req.user / query / path params and delegate to the model statics +
// instance helpers in pulse::models::pulsescore (getOrCreate / getDisplayData /
// getLeaderboard / getUserRank) — NOT reimplemented here. On any thrown error
// each replies with res.status(500).json({ success:false, error:'<message>' }),
// matching the Express try/catch. Note these handlers return Express's bespoke
// { success, error } shape (NO `code` field) for errors, and { success, data }
// for successes.
//
// Routes (full path = /api/v1/pulse-score + sub-path from pulseScoreRoutes.js):
//   GET /api/v1/pulse-score/me              -> getMyScore
//   GET /api/v1/pulse-score/breakdown       -> getBreakdown
//   GET /api/v1/pulse-score/achievements    -> getAchievements
//   GET /api/v1/pulse-score/history         -> getHistory
//   GET /api/v1/pulse-score/leaderboard     -> getLeaderboard
//   GET /api/v1/pulse-score/user/:userId    -> getUserScore
#pragma once
#include <drogon/HttpController.h>

namespace pulse::controllers {

using namespace drogon;

class PulseScoreController : public drogon::HttpController<PulseScoreController> {
public:
  METHOD_LIST_BEGIN
  // GET /api/v1/pulse-score/me — requester's own display data.
  ADD_METHOD_TO(PulseScoreController::getMyScore, "/api/v1/pulse-score/me", Get,
                "pulse::filters::AuthFilter");

  // GET /api/v1/pulse-score/breakdown — display data + metrics/history/achievements.
  ADD_METHOD_TO(PulseScoreController::getBreakdown, "/api/v1/pulse-score/breakdown", Get,
                "pulse::filters::AuthFilter");

  // GET /api/v1/pulse-score/achievements — requester's achievements array.
  ADD_METHOD_TO(PulseScoreController::getAchievements, "/api/v1/pulse-score/achievements", Get,
                "pulse::filters::AuthFilter");

  // GET /api/v1/pulse-score/history — last `days` history entries (default 30).
  ADD_METHOD_TO(PulseScoreController::getHistory, "/api/v1/pulse-score/history", Get,
                "pulse::filters::AuthFilter");

  // GET /api/v1/pulse-score/leaderboard — top users + requester's rank.
  ADD_METHOD_TO(PulseScoreController::getLeaderboard, "/api/v1/pulse-score/leaderboard", Get,
                "pulse::filters::AuthFilter");

  // GET /api/v1/pulse-score/user/:userId — another user's public score.
  ADD_METHOD_TO(PulseScoreController::getUserScore, "/api/v1/pulse-score/user/{1}", Get,
                "pulse::filters::AuthFilter");
  METHOD_LIST_END

  // GET /api/v1/pulse-score/me — PulseScore.getOrCreate(userId).getDisplayData().
  void getMyScore(const HttpRequestPtr& req,
                  std::function<void(const HttpResponsePtr&)>&& callback);

  // GET /api/v1/pulse-score/breakdown — getDisplayData + metrics/history(-30)/achievements.
  void getBreakdown(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback);

  // GET /api/v1/pulse-score/achievements — ps.achievements.
  void getAchievements(const HttpRequestPtr& req,
                       std::function<void(const HttpResponsePtr&)>&& callback);

  // GET /api/v1/pulse-score/history — ps.history.slice(-days).
  void getHistory(const HttpRequestPtr& req,
                  std::function<void(const HttpResponsePtr&)>&& callback);

  // GET /api/v1/pulse-score/leaderboard — getLeaderboard(limit) + getUserRank(userId).
  void getLeaderboard(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback);

  // GET /api/v1/pulse-score/user/:userId — findOne({user}) -> getDisplayData() or default.
  void getUserScore(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback,
                    std::string userId);
};

} // namespace pulse::controllers
