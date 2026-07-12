// notification_controller.cc — implementation of
// pulse::controllers::NotificationController.
//
// 1:1 port of src/controllers/notificationController.js. The DB query logic
// lives in pulse::models::notification (getNotifications / getUnreadCountByType /
// markAsRead / markAllAsRead) and is called — NOT reimplemented — here. The only
// inline DB access is deleteNotification, which the JS controller performs
// directly via Notification.findOneAndDelete({ _id: id, recipient: userId }).
//
// Each handler mirrors its Express counterpart exactly: read req.user.userId,
// parse the same query/path params, and on any thrown error reply with
// res.status(500).json({ success:false, message:'<handler message>' }). These
// endpoints use Express's bespoke { success, message } JSON shapes rather than
// the standard { error, code } envelope, so responses are built with
// pulse::http::json directly.
#include "pulse/controllers/notification_controller.hpp"

#include <cctype>
#include <exception>
#include <optional>
#include <string>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>

#include "pulse/bson_json.hpp"
#include "pulse/db.hpp"
#include "pulse/http_response.hpp"
#include "pulse/logger.hpp"
#include "pulse/models/notification.hpp"

using namespace pulse::controllers;
namespace model = pulse::models::notification;
namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;

namespace {

// ── JS parseInt() — parse a leading optional-sign integer; NaN on no digits ────
// Mirrors `parseInt(str, 10)`: trims leading whitespace, reads an optional sign
// then digits, stops at the first non-digit, returns nullopt (NaN) when no digit
// was consumed.
std::optional<long> jsParseInt(const std::string& s) {
  size_t i = 0;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  bool neg = false;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
    neg = (s[i] == '-');
    ++i;
  }
  size_t start = i;
  long value = 0;
  while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
    value = value * 10 + (s[i] - '0');
    ++i;
  }
  if (i == start) return std::nullopt;  // no digits -> NaN
  return neg ? -value : value;
}

// req.user.userId from the AuthFilter-populated attribute.
std::string authedUserId(const drogon::HttpRequestPtr& req) {
  const auto& user = req->getAttributes()->get<Json::Value>("user");
  return user["userId"].asString();
}

// res.status(500).json({ success:false, message }) — the Express catch shape.
pulse::http::HttpResponsePtr failure(const std::string& message) {
  Json::Value body(Json::objectValue);
  body["success"] = false;
  body["message"] = message;
  return pulse::http::json(drogon::k500InternalServerError, std::move(body));
}

// res.status(404).json({ success:false, message }) — the "not found" shape.
pulse::http::HttpResponsePtr notFoundMsg(const std::string& message) {
  Json::Value body(Json::objectValue);
  body["success"] = false;
  body["message"] = message;
  return pulse::http::json(drogon::k404NotFound, std::move(body));
}

}  // namespace

// ── GET /api/v1/notifications ─────────────────────────────────────────────────
// const { page = 1, limit = 20, type, unreadOnly } = req.query;
// const result = await Notification.getNotifications(userId, {
//   page: parseInt(page), limit: parseInt(limit),
//   type: type || null, unreadOnly: unreadOnly === 'true' });
// res.status(200).json({ success:true, data: result.notifications,
//                        pagination: result.pagination });
void NotificationController::getNotifications(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    const std::string userId = authedUserId(req);

    model::GetNotificationsOptions options;

    // Destructured defaults (page=1, limit=20) apply only when the query key is
    // absent; when present the value is parseInt'd. Drogon returns "" for an
    // absent param, matching JS `undefined` -> use the destructured default.
    const std::string pageRaw = req->getParameter("page");
    options.page = pageRaw.empty()
                       ? 1
                       : static_cast<int>(jsParseInt(pageRaw).value_or(1));
    const std::string limitRaw = req->getParameter("limit");
    options.limit = limitRaw.empty()
                        ? 20
                        : static_cast<int>(jsParseInt(limitRaw).value_or(20));

    // type: type || null — empty/absent query string is falsy -> no type filter.
    const std::string type = req->getParameter("type");
    if (!type.empty()) options.type = type;

    // unreadOnly: unreadOnly === 'true' — strict string compare.
    options.unreadOnly = (req->getParameter("unreadOnly") == "true");

    Json::Value result = model::getNotifications(userId, options);

    Json::Value body(Json::objectValue);
    body["success"] = true;
    body["data"] = result["notifications"];
    body["pagination"] = result["pagination"];
    callback(pulse::http::json(drogon::k200OK, std::move(body)));
  } catch (const std::exception& e) {
    pulse::log::error("Get Notifications Error: {}", e.what());
    callback(failure("Failed to fetch notifications"));
  }
}

// ── GET /api/v1/notifications/count ───────────────────────────────────────────
// const counts = await Notification.getUnreadCountByType(userId);
// res.status(200).json({ success:true, data: counts });
void NotificationController::getUnreadCount(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    const std::string userId = authedUserId(req);
    Json::Value counts = model::getUnreadCountByType(userId);
    callback(pulse::http::ok(std::move(counts)));  // { success:true, data: counts }
  } catch (const std::exception& e) {
    pulse::log::error("Get Unread Count Error: {}", e.what());
    callback(failure("Failed to get unread count"));
  }
}

// ── PATCH /api/v1/notifications/read-all ──────────────────────────────────────
// const { type } = req.query;
// await Notification.markAllAsRead(userId, type || null);
// res.status(200).json({ success:true, message:'All notifications marked as read' });
void NotificationController::markAllAsRead(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    const std::string userId = authedUserId(req);

    // type || null — empty/absent query string is falsy -> no type filter.
    const std::string type = req->getParameter("type");
    std::optional<std::string> typeOpt;
    if (!type.empty()) typeOpt = type;

    model::markAllAsRead(userId, typeOpt);

    Json::Value body(Json::objectValue);
    body["success"] = true;
    body["message"] = "All notifications marked as read";
    callback(pulse::http::json(drogon::k200OK, std::move(body)));
  } catch (const std::exception& e) {
    pulse::log::error("Mark All As Read Error: {}", e.what());
    callback(failure("Failed to mark notifications as read"));
  }
}

// ── PATCH /api/v1/notifications/:id/read ──────────────────────────────────────
// const { id } = req.params;
// const notification = await Notification.markAsRead(id, userId);
// if (!notification) return res.status(404).json({ success:false,
//                                  message:'Notification not found' });
// res.status(200).json({ success:true, data: notification });
void NotificationController::markAsRead(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id) {
  try {
    const std::string userId = authedUserId(req);

    std::optional<Json::Value> notification = model::markAsRead(id, userId);
    if (!notification) {
      callback(notFoundMsg("Notification not found"));
      return;
    }

    callback(pulse::http::ok(std::move(*notification)));  // { success:true, data }
  } catch (const std::exception& e) {
    pulse::log::error("Mark As Read Error: {}", e.what());
    callback(failure("Failed to mark notification as read"));
  }
}

// ── DELETE /api/v1/notifications/:id ──────────────────────────────────────────
// const { id } = req.params;
// const result = await Notification.findOneAndDelete({ _id: id, recipient: userId });
// if (!result) return res.status(404).json({ success:false,
//                              message:'Notification not found' });
// res.status(200).json({ success:true, message:'Notification deleted' });
void NotificationController::deleteNotification(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id) {
  try {
    const std::string userId = authedUserId(req);

    // findOneAndDelete({ _id: id, recipient: userId }). _id and recipient are
    // ObjectIds in the schema; coerce 24-hex strings to ObjectId (string
    // fallback mirrors the model statics so a non-oid simply matches nothing).
    bld::document filter;
    if (auto nid = pulse::bsonjson::tryOid(id)) filter.append(kvp("_id", *nid));
    else                                        filter.append(kvp("_id", id));
    if (auto rid = pulse::bsonjson::tryOid(userId)) filter.append(kvp("recipient", *rid));
    else                                            filter.append(kvp("recipient", userId));

    auto result =
        pulse::db::collection(model::kCollection).find_one_and_delete(filter.view());

    if (!result) {
      callback(notFoundMsg("Notification not found"));
      return;
    }

    Json::Value body(Json::objectValue);
    body["success"] = true;
    body["message"] = "Notification deleted";
    callback(pulse::http::json(drogon::k200OK, std::move(body)));
  } catch (const std::exception& e) {
    pulse::log::error("Delete Notification Error: {}", e.what());
    callback(failure("Failed to delete notification"));
  }
}
