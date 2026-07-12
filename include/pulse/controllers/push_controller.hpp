// push_controller.hpp — Drogon HttpController porting the inline PUSH route
// group in src/routes/pushRoutes.js (mounted at /api/v1/push).
//
// pushRoutes.js has no dedicated controller file; the push logic is written
// inline in the route handlers and delegates to src/services/pushService.js
// (ported here to pulse::PushService). The whole router is gated by
// `router.use(verifyAccessToken)`, so every handler carries
// pulse::filters::AuthFilter.
//
// These handlers return Express's bespoke { success, message } / { success,
// data } JSON shapes (NOT the standard { error, code } envelope), matching the
// res.status(x).json({...}) calls in the route file exactly.
//
// Routes (full path = /api/v1/push + sub-path from pushRoutes.js):
//   POST   /api/v1/push/register    -> registerToken
//   DELETE /api/v1/push/unregister  -> unregisterToken
//   GET    /api/v1/push/status      -> getStatus
#pragma once
#include <drogon/HttpController.h>

namespace pulse::controllers {

using namespace drogon;

class PushController : public drogon::HttpController<PushController> {
public:
  METHOD_LIST_BEGIN
  // POST /api/v1/push/register — register an FCM/Expo token for the user's device.
  ADD_METHOD_TO(PushController::registerToken, "/api/v1/push/register", Post,
                "pulse::filters::AuthFilter");

  // DELETE /api/v1/push/unregister — unregister the device's token (on logout).
  ADD_METHOD_TO(PushController::unregisterToken, "/api/v1/push/unregister", Delete,
                "pulse::filters::AuthFilter");

  // GET /api/v1/push/status — whether push notifications are enabled for the user.
  ADD_METHOD_TO(PushController::getStatus, "/api/v1/push/status", Get,
                "pulse::filters::AuthFilter");
  METHOD_LIST_END

  // POST /api/v1/push/register — pushService.registerToken(userId, token,
  // deviceId, platform || 'android').
  void registerToken(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback);

  // DELETE /api/v1/push/unregister — pushService.unregisterToken(userId, deviceId).
  void unregisterToken(const HttpRequestPtr& req,
                       std::function<void(const HttpResponsePtr&)>&& callback);

  // GET /api/v1/push/status — User.findById(...).select('fcmTokens
  // settings.pushNotifications') + pushService.isFirebaseReady().
  void getStatus(const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback);
};

} // namespace pulse::controllers
