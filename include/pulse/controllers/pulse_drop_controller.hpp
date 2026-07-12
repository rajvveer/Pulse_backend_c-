// pulse_drop_controller.hpp — Drogon HttpController porting
// src/controllers/pulseDropController.js and its route group
// src/routes/pulseDropRoutes.js (mounted at /api/v1/pulse-drops).
//
// Express router:
//   router.use(verifyAccessToken);                 // every route is authed
//   router.get ('/',                 getActive);
//   router.get ('/:dropId',          getById);
//   router.post('/:dropId/join',     join);
//   router.post('/:dropId/respond',  createResponse);
//   router.get ('/:dropId/responses',getResponses);
//   router.post('/create',           requireAdmin, createDrop);
//
// Middleware -> Drogon filters (by class name, order preserved):
//   verifyAccessToken -> pulse::filters::AuthFilter
//   requireAdmin      -> pulse::filters::RequireAdminFilter
//
// Routes (full path = /api/v1/pulse-drops + sub-path):
//   GET  /api/v1/pulse-drops                    -> getActive       (AuthFilter)
//   GET  /api/v1/pulse-drops/{dropId}           -> getById         (AuthFilter)
//   POST /api/v1/pulse-drops/{dropId}/join      -> join            (AuthFilter)
//   POST /api/v1/pulse-drops/{dropId}/respond   -> createResponse  (AuthFilter)
//   GET  /api/v1/pulse-drops/{dropId}/responses -> getResponses    (AuthFilter)
//   POST /api/v1/pulse-drops/create             -> createDrop      (AuthFilter,
//                                                                    RequireAdminFilter)
//
// The cron handler exports.expireDrops is NOT an HTTP route — it lives in the
// jobs/scheduler, calling pulse::models::pulsedrop::expireOld(), so it is not
// ported here.
#pragma once
#include <drogon/HttpController.h>

namespace pulse::controllers {

using namespace drogon;

class PulseDropController : public drogon::HttpController<PulseDropController> {
public:
  METHOD_LIST_BEGIN
  // Get active drops.
  ADD_METHOD_TO(PulseDropController::getActive, "/api/v1/pulse-drops", Get,
                "pulse::filters::AuthFilter");

  // Admin: create a drop manually. Registered before the "/{1}" GET wildcard is
  // irrelevant (different method/path), but the requireAdmin filter is attached
  // after AuthFilter to mirror the Express chain order.
  ADD_METHOD_TO(PulseDropController::createDrop, "/api/v1/pulse-drops/create", Post,
                "pulse::filters::AuthFilter", "pulse::filters::RequireAdminFilter");

  // Get single drop.
  ADD_METHOD_TO(PulseDropController::getById, "/api/v1/pulse-drops/{1}", Get,
                "pulse::filters::AuthFilter");

  // Join a drop.
  ADD_METHOD_TO(PulseDropController::join, "/api/v1/pulse-drops/{1}/join", Post,
                "pulse::filters::AuthFilter");

  // Create a response post for a drop.
  ADD_METHOD_TO(PulseDropController::createResponse, "/api/v1/pulse-drops/{1}/respond",
                Post, "pulse::filters::AuthFilter");

  // Get drop responses.
  ADD_METHOD_TO(PulseDropController::getResponses, "/api/v1/pulse-drops/{1}/responses",
                Get, "pulse::filters::AuthFilter");
  METHOD_LIST_END

  // GET /api/v1/pulse-drops — list active drops with time remaining.
  void getActive(const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback);

  // GET /api/v1/pulse-drops/{dropId} — single drop with time remaining.
  void getById(const HttpRequestPtr& req,
               std::function<void(const HttpResponsePtr&)>&& callback,
               std::string dropId);

  // POST /api/v1/pulse-drops/{dropId}/join — join a drop.
  void join(const HttpRequestPtr& req,
            std::function<void(const HttpResponsePtr&)>&& callback,
            std::string dropId);

  // POST /api/v1/pulse-drops/{dropId}/respond — create a response post + join.
  void createResponse(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback,
                      std::string dropId);

  // GET /api/v1/pulse-drops/{dropId}/responses — paginated drop responses.
  void getResponses(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback,
                    std::string dropId);

  // POST /api/v1/pulse-drops/create — admin manual drop creation.
  void createDrop(const HttpRequestPtr& req,
                  std::function<void(const HttpResponsePtr&)>&& callback);
};

} // namespace pulse::controllers
