// notification_controller.hpp — Drogon HttpController porting
// src/controllers/notificationController.js and its route group
// src/routes/notificationRoutes.js (mounted at /api/v1/notifications).
//
// Every route in the Express router is gated by `router.use(verifyAccessToken)`,
// so each handler carries pulse::filters::AuthFilter. The handlers are thin:
// they read req.user / query / path params and delegate to the model statics in
// pulse::models::notification (which own the DB query logic) — NOT reimplemented
// here. On any thrown error each replies with res.status(500).json({ success:
// false, message: '<handler-specific message>' }), matching the Express
// try/catch. Note these handlers return Express's bespoke { success, message }
// shapes (NOT the standard { error, code } error envelope).
//
// Routes (full path = /api/v1/notifications + sub-path from notificationRoutes.js):
//   GET    /api/v1/notifications            -> getNotifications
//   GET    /api/v1/notifications/count      -> getUnreadCount
//   PATCH  /api/v1/notifications/read-all   -> markAllAsRead
//   PATCH  /api/v1/notifications/:id/read   -> markAsRead
//   DELETE /api/v1/notifications/:id        -> deleteNotification
#pragma once
#include <drogon/HttpController.h>

namespace pulse::controllers {

using namespace drogon;

class NotificationController : public drogon::HttpController<NotificationController> {
public:
  METHOD_LIST_BEGIN
  // GET /api/v1/notifications — paginated list (optional type/unreadOnly filters).
  ADD_METHOD_TO(NotificationController::getNotifications, "/api/v1/notifications", Get,
                "pulse::filters::AuthFilter");

  // GET /api/v1/notifications/count — unread count grouped by type.
  ADD_METHOD_TO(NotificationController::getUnreadCount, "/api/v1/notifications/count", Get,
                "pulse::filters::AuthFilter");

  // PATCH /api/v1/notifications/read-all — mark all (optionally of one type) read.
  // Declared before the parameterized /{1}/read so it is not shadowed.
  ADD_METHOD_TO(NotificationController::markAllAsRead, "/api/v1/notifications/read-all", Patch,
                "pulse::filters::AuthFilter");

  // PATCH /api/v1/notifications/:id/read — mark a single notification read.
  ADD_METHOD_TO(NotificationController::markAsRead, "/api/v1/notifications/{1}/read", Patch,
                "pulse::filters::AuthFilter");

  // DELETE /api/v1/notifications/:id — delete a notification.
  ADD_METHOD_TO(NotificationController::deleteNotification, "/api/v1/notifications/{1}", Delete,
                "pulse::filters::AuthFilter");
  METHOD_LIST_END

  // GET /api/v1/notifications — Notification.getNotifications(userId, opts).
  void getNotifications(const HttpRequestPtr& req,
                        std::function<void(const HttpResponsePtr&)>&& callback);

  // GET /api/v1/notifications/count — Notification.getUnreadCountByType(userId).
  void getUnreadCount(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback);

  // PATCH /api/v1/notifications/read-all — Notification.markAllAsRead(userId, type).
  void markAllAsRead(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback);

  // PATCH /api/v1/notifications/:id/read — Notification.markAsRead(id, userId).
  void markAsRead(const HttpRequestPtr& req,
                  std::function<void(const HttpResponsePtr&)>&& callback,
                  std::string id);

  // DELETE /api/v1/notifications/:id — findOneAndDelete({_id:id, recipient:userId}).
  void deleteNotification(const HttpRequestPtr& req,
                          std::function<void(const HttpResponsePtr&)>&& callback,
                          std::string id);
};

} // namespace pulse::controllers
