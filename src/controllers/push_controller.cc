// push_controller.cc — implementation of pulse::controllers::PushController.
//
// 1:1 port of the inline handlers in src/routes/pushRoutes.js. The push token
// storage / send logic lives in pulse::PushService (port of
// src/services/pushService.js) and is called — NOT reimplemented — here. The
// only inline DB access is getStatus, which the route handler performs directly
// via User.findById(userId).select('fcmTokens settings.pushNotifications').
//
// Each handler mirrors its Express counterpart exactly: read req.user.userId,
// parse the same body/query fields, run the same validation, and on any thrown
// error reply with res.status(500).json({ success:false, message:'<handler
// message>' }). These endpoints use Express's bespoke { success, message } /
// { success, data } JSON shapes rather than the standard { error, code }
// envelope, so responses are built with pulse::http::json directly.
#include "pulse/controllers/push_controller.hpp"

#include <exception>
#include <string>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <mongocxx/options/find.hpp>

#include "pulse/bson_json.hpp"
#include "pulse/db.hpp"
#include "pulse/http_response.hpp"
#include "pulse/logger.hpp"
#include "pulse/models/user.hpp"
#include "pulse/services/push_service.hpp"

using namespace pulse::controllers;
namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;

namespace {

// req.user.userId from the AuthFilter-populated attribute.
std::string authedUserId(const drogon::HttpRequestPtr& req) {
  const auto& user = req->getAttributes()->get<Json::Value>("user");
  return user["userId"].asString();
}

// res.status(code).json({ success:false, message }) — the Express failure shape.
pulse::http::HttpResponsePtr failure(drogon::HttpStatusCode code,
                                     const std::string& message) {
  Json::Value body(Json::objectValue);
  body["success"] = false;
  body["message"] = message;
  return pulse::http::json(code, std::move(body));
}

// res.status(code).json({ success:true, message }) — the Express success shape.
pulse::http::HttpResponsePtr successMsg(drogon::HttpStatusCode code,
                                        const std::string& message) {
  Json::Value body(Json::objectValue);
  body["success"] = true;
  body["message"] = message;
  return pulse::http::json(code, std::move(body));
}

}  // namespace

// ── POST /api/v1/push/register ────────────────────────────────────────────────
// const userId = req.user.userId;
// const { token, deviceId, platform } = req.body;
// if (!token || !deviceId) return res.status(400).json({ success:false,
//                              message:'Token and deviceId are required' });
// const result = await pushService.registerToken(userId, token, deviceId,
//                                                platform || 'android');
// if (result.success) res.status(200).json({ success:true,
//                            message:'Push notification token registered' });
// else res.status(500).json({ success:false,
//                            message: result.error || 'Failed to register token' });
void PushController::registerToken(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    const std::string userId = authedUserId(req);

    auto bodyPtr = req->getJsonObject();
    const Json::Value body = bodyPtr ? *bodyPtr : Json::Value(Json::objectValue);

    // const { token, deviceId, platform } = req.body;
    const std::string token =
        body.isMember("token") && body["token"].isString() ? body["token"].asString() : "";
    const std::string deviceId =
        body.isMember("deviceId") && body["deviceId"].isString() ? body["deviceId"].asString()
                                                                 : "";

    // if (!token || !deviceId) — falsy when empty/missing.
    if (token.empty() || deviceId.empty()) {
      callback(failure(drogon::k400BadRequest, "Token and deviceId are required"));
      return;
    }

    // platform || 'android' — falsy (missing/empty) falls back to 'android'.
    std::string platform =
        body.isMember("platform") && body["platform"].isString() ? body["platform"].asString()
                                                                 : "";
    if (platform.empty()) platform = "android";

    Json::Value result =
        pulse::PushService::instance().registerToken(userId, token, deviceId, platform);

    if (result.isObject() && result.isMember("success") && result["success"].asBool()) {
      callback(successMsg(drogon::k200OK, "Push notification token registered"));
    } else {
      // result.error || 'Failed to register token'
      const std::string err =
          (result.isObject() && result.isMember("error") && result["error"].isString())
              ? result["error"].asString()
              : "";
      callback(failure(drogon::k500InternalServerError,
                       err.empty() ? "Failed to register token" : err));
    }
  } catch (const std::exception& e) {
    pulse::log::error("Register push token error: {}", e.what());
    callback(failure(drogon::k500InternalServerError,
                     "Failed to register push notification token"));
  }
}

// ── DELETE /api/v1/push/unregister ────────────────────────────────────────────
// const userId = req.user.userId;
// const { deviceId } = req.body;
// if (!deviceId) return res.status(400).json({ success:false,
//                                            message:'deviceId is required' });
// const result = await pushService.unregisterToken(userId, deviceId);
// res.status(200).json({ success:true,
//                       message:'Push notification token unregistered' });
void PushController::unregisterToken(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    const std::string userId = authedUserId(req);

    auto bodyPtr = req->getJsonObject();
    const Json::Value body = bodyPtr ? *bodyPtr : Json::Value(Json::objectValue);

    // const { deviceId } = req.body;
    const std::string deviceId =
        body.isMember("deviceId") && body["deviceId"].isString() ? body["deviceId"].asString()
                                                                 : "";

    // if (!deviceId)
    if (deviceId.empty()) {
      callback(failure(drogon::k400BadRequest, "deviceId is required"));
      return;
    }

    // Result intentionally ignored, matching the JS (it always returns success).
    pulse::PushService::instance().unregisterToken(userId, deviceId);

    callback(successMsg(drogon::k200OK, "Push notification token unregistered"));
  } catch (const std::exception& e) {
    pulse::log::error("Unregister push token error: {}", e.what());
    callback(failure(drogon::k500InternalServerError,
                     "Failed to unregister push notification token"));
  }
}

// ── GET /api/v1/push/status ───────────────────────────────────────────────────
// const userId = req.user.userId;
// const user = await User.findById(userId)
//                        .select('fcmTokens settings.pushNotifications');
// res.status(200).json({ success:true, data: {
//   enabled: user?.settings?.pushNotifications !== false,
//   registeredDevices: user?.fcmTokens?.length || 0,
//   firebaseConfigured: pushService.isFirebaseReady() } });
void PushController::getStatus(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    const std::string userId = authedUserId(req);

    // User.findById(userId).select('fcmTokens settings.pushNotifications').
    // _id is an ObjectId in the schema; coerce a 24-hex string to ObjectId
    // (string fallback simply matches nothing, mirroring a bad id).
    bld::document filter;
    if (auto uid = pulse::bsonjson::tryOid(userId)) filter.append(kvp("_id", *uid));
    else                                            filter.append(kvp("_id", userId));

    mongocxx::options::find opts;
    opts.projection(make_document(kvp("fcmTokens", 1), kvp("settings.pushNotifications", 1)));

    auto doc =
        pulse::db::collection(pulse::models::user::kCollection).find_one(filter.view(), opts);

    Json::Value user(Json::nullValue);
    if (doc) user = pulse::bsonjson::toJson(doc->view());

    // enabled: user?.settings?.pushNotifications !== false
    // True unless the field is explicitly the boolean false (null user, missing
    // settings, or missing/non-false field => true).
    bool enabled = true;
    if (user.isObject() && user.isMember("settings") && user["settings"].isObject() &&
        user["settings"].isMember("pushNotifications") &&
        user["settings"]["pushNotifications"].isBool() &&
        user["settings"]["pushNotifications"].asBool() == false) {
      enabled = false;
    }

    // registeredDevices: user?.fcmTokens?.length || 0
    Json::ArrayIndex registeredDevices = 0;
    if (user.isObject() && user.isMember("fcmTokens") && user["fcmTokens"].isArray()) {
      registeredDevices = user["fcmTokens"].size();
    }

    Json::Value data(Json::objectValue);
    data["enabled"] = enabled;
    data["registeredDevices"] = registeredDevices;
    data["firebaseConfigured"] = pulse::PushService::instance().isFirebaseReady();

    Json::Value body(Json::objectValue);
    body["success"] = true;
    body["data"] = std::move(data);
    callback(pulse::http::json(drogon::k200OK, std::move(body)));
  } catch (const std::exception& e) {
    pulse::log::error("Get push status error: {}", e.what());
    callback(failure(drogon::k500InternalServerError,
                     "Failed to get push notification status"));
  }
}
