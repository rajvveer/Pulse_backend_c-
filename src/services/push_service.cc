// push_service.cc — implementation of pushService.js.
//
// Expo path  : POST exp.host/--/api/v2/push/send.
// FCM path   : the Node `firebase-admin` SDK's admin.messaging().send() is
//              reproduced over HTTP: a service-account JWT (RS256) is exchanged
//              for an OAuth2 access token at oauth2.googleapis.com/token, then
//              the message is POSTed to the FCM HTTP v1 endpoint
//              fcm.googleapis.com/v1/projects/{projectId}/messages:send.
//
// HTTP is done with Drogon's HttpClient (synchronous sendRequest on a worker
// thread, matching the conventions note that blocking DB/HTTP calls inside a
// handler are acceptable on Drogon's thread pool).
#include "pulse/services/push_service.hpp"
#include "pulse/services/http_client.hpp"

#include "pulse/config.hpp"
#include "pulse/logger.hpp"
#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/models/user.hpp"

#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpTypes.h>

// jwt-cpp built with JWT_DISABLE_PICOJSON in vcpkg; use the JsonCpp traits.
#include <jwt-cpp/traits/open-source-parsers-jsoncpp/defaults.h>

#include <json/json.h>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/oid.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/options/find.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

namespace pulse {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;

namespace {

// Serialize a Json::Value compactly (no extra whitespace / newlines), matching
// JSON.stringify output for request bodies.
std::string toCompactJson(const Json::Value& v) {
  Json::StreamWriterBuilder b;
  b["indentation"] = "";
  return Json::writeString(b, v);
}

Json::Value parseJson(const std::string& s) {
  Json::Value out;
  Json::CharReaderBuilder b;
  std::string errs;
  std::istringstream in(s);
  Json::parseFromStream(b, in, &out, &errs);
  return out;
}

// Replace literal "\n" sequences with real newlines (mirrors the JS
// privateKey.replace(/\\n/g, '\n')).
std::string unescapeNewlines(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '\\' && i + 1 < in.size() && in[i + 1] == 'n') {
      out.push_back('\n');
      ++i;
    } else {
      out.push_back(in[i]);
    }
  }
  return out;
}

long long nowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

constexpr std::size_t kMaxPushTokenBytes = 4096;
constexpr std::size_t kMaxDeviceIdBytes = 200;
constexpr std::int32_t kMaxRegisteredDevices = 20;

bool containsWhitespaceOrControl(const std::string& value) {
  for (const unsigned char ch : value) {
    if (std::isspace(ch) || std::iscntrl(ch)) return true;
  }
  return false;
}

bool isValidRegistrationToken(const std::string& token) {
  return !token.empty() && token.size() <= kMaxPushTokenBytes &&
         !containsWhitespaceOrControl(token);
}

bool isValidDeviceId(const std::string& deviceId) {
  if (deviceId.empty() || deviceId.size() > kMaxDeviceIdBytes) return false;
  return std::none_of(deviceId.begin(), deviceId.end(), [](unsigned char ch) {
    return std::iscntrl(ch) != 0;
  });
}

bool isValidPlatform(const std::string& platform) {
  return platform == "ios" || platform == "android" || platform == "web" ||
         platform == "desktop";
}

} // namespace

PushService& PushService::instance() {
  static PushService s;
  return s;
}

PushService::PushService() {
  // Initialize on construction, mirroring the JS "Initialize on module load".
  initializeFirebase();
}

void PushService::initializeFirebase() {
  if (firebaseInitialized_) return;

  try {
    auto& cfg = config();

    // Branch 1: a service account JSON file path (config.get('firebase.serviceAccountPath')).
    std::string serviceAccountPath = cfg.env("FIREBASE_SERVICE_ACCOUNT_PATH");
    std::string projectId          = cfg.env("FIREBASE_PROJECT_ID");

    if (!serviceAccountPath.empty()) {
      std::ifstream f(serviceAccountPath);
      if (f) {
        std::stringstream ss;
        ss << f.rdbuf();
        Json::Value sa = parseJson(ss.str());
        projectId_   = projectId.empty() ? sa.get("project_id", "").asString() : projectId;
        clientEmail_ = sa.get("client_email", "").asString();
        privateKey_  = sa.get("private_key", "").asString();
        if (!projectId_.empty() && !clientEmail_.empty() && !privateKey_.empty()) {
          firebaseInitialized_ = true;
          log::info("Firebase Admin initialized with service account");
          return;
        }
      }
      log::error("Firebase initialization error: could not load service account");
      return;
    }

    // Branch 2: environment variables.
    std::string envProjectId   = cfg.env("FIREBASE_PROJECT_ID");
    std::string envClientEmail  = cfg.env("FIREBASE_CLIENT_EMAIL");
    std::string envPrivateKey   = cfg.env("FIREBASE_PRIVATE_KEY");
    if (!envProjectId.empty() && !envClientEmail.empty() && !envPrivateKey.empty()) {
      projectId_   = envProjectId;
      clientEmail_ = envClientEmail;
      privateKey_  = unescapeNewlines(envPrivateKey);
      firebaseInitialized_ = true;
      log::info("Firebase Admin initialized with environment variables");
      return;
    }

    log::warn("Firebase not configured - push notifications disabled");
  } catch (const std::exception& e) {
    (void)e;
    log::error("Firebase initialization error");
  }
}

bool PushService::isFirebaseReady() const { return firebaseInitialized_; }

bool PushService::isExpoPushToken(const std::string& token) {
  return token.rfind("ExponentPushToken[", 0) == 0;
}

// ─────────────────────────────────────────────────────────────────────────
// Expo
// ─────────────────────────────────────────────────────────────────────────
Json::Value PushService::sendViaExpo(const std::string& expoPushToken,
                                     const Json::Value& notification,
                                     const Json::Value& data) {
  Json::Value result(Json::objectValue);
  try {
    Json::Value payload(Json::objectValue);
    payload["to"]        = expoPushToken;
    payload["sound"]     = "default";
    payload["title"]     = notification.get("title", Json::Value(Json::nullValue));
    payload["body"]      = notification.get("body", Json::Value(Json::nullValue));
    payload["data"]      = data;
    payload["priority"]  = "high";
    payload["channelId"] = "pulse_notifications";

    std::string body = toCompactJson(payload);

    auto client = drogon::HttpClient::newHttpClient("https://exp.host");
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setPath("/--/api/v2/push/send");
    req->addHeader("Accept", "application/json");
    req->setContentTypeString("application/json");
    req->setBody(body);

    auto [reqResult, resp] = client->sendRequest(
        req, pulse::services::outboundHttpTimeoutSeconds());
    if (reqResult != drogon::ReqResult::Ok || !resp) {
      log::error("Expo push request error");
      result["success"] = false;
      result["error"]   = "request failed";
      return result;
    }

    Json::Value parsed = parseJson(std::string(resp->getBody()));
    const Json::Value& rdata = parsed["data"];
    if (rdata.isObject() && rdata.get("status", "").asString() == "ok") {
      log::info("Expo push sent");
      result["success"]   = true;
      result["messageId"] = rdata.get("id", Json::Value(Json::nullValue));
    } else if (rdata.isObject() && rdata.get("status", "").asString() == "error") {
      std::string msg = rdata.get("message", "").asString();
      log::error("Expo push rejected by provider");
      bool isInvalid =
          rdata.isMember("details") && rdata["details"].isObject() &&
          rdata["details"].get("error", "").asString() == "DeviceNotRegistered";
      result["success"]      = false;
      result["invalidToken"] = isInvalid;
      result["error"]        = msg;
    } else {
      log::info("Expo push sent");
      result["success"] = true;
    }
    return result;
  } catch (const std::exception& e) {
    log::error("Expo push error");
    result["success"] = false;
    result["error"]   = std::string(e.what());
    return result;
  }
}

// ─────────────────────────────────────────────────────────────────────────
// FCM (admin.messaging().send() equivalent)
// ─────────────────────────────────────────────────────────────────────────
std::string PushService::getAccessToken() {
  // Serialize both the cache check and refresh. Besides eliminating data races
  // on the cached string/expiry, this prevents a refresh stampede when several
  // push sends notice an expired token at once.
  std::lock_guard<std::mutex> lock(accessTokenMutex_);

  long long now = nowSeconds();
  if (!cachedAccessToken_.empty() && now < accessTokenExpiry_ - 60) {
    return cachedAccessToken_;
  }
  if (clientEmail_.empty() || privateKey_.empty()) return "";

  try {
    const std::string scope = "https://www.googleapis.com/auth/firebase.messaging";
    const std::string aud   = "https://oauth2.googleapis.com/token";

    auto issued = std::chrono::system_clock::now();
    std::string assertion =
        jwt::create<jwt::traits::open_source_parsers_jsoncpp>()
            .set_issuer(clientEmail_)
            .set_audience(aud)
            .set_issued_at(issued)
            .set_expires_at(issued + std::chrono::hours(1))
            .set_payload_claim("scope", jwt::claim(scope))
            .sign(jwt::algorithm::rs256("", privateKey_, "", ""));

    // POST grant_type + assertion (form-urlencoded) to the token endpoint.
    std::string formBody =
        "grant_type=urn:ietf:params:oauth:grant-type:jwt-bearer&assertion=" +
        assertion;

    auto client = drogon::HttpClient::newHttpClient("https://oauth2.googleapis.com");
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setPath("/token");
    req->setContentTypeString("application/x-www-form-urlencoded");
    req->setBody(formBody);

    auto [reqResult, resp] = client->sendRequest(
        req, pulse::services::outboundHttpTimeoutSeconds());
    if (reqResult != drogon::ReqResult::Ok || !resp) {
      log::error("FCM token exchange request failed");
      return "";
    }
    Json::Value parsed = parseJson(std::string(resp->getBody()));
    std::string token = parsed.get("access_token", "").asString();
    long long expiresIn = parsed.get("expires_in", 3600).asInt64();
    if (token.empty()) {
      log::error("FCM token exchange returned no access token (status {})",
                 static_cast<int>(resp->getStatusCode()));
      return "";
    }
    cachedAccessToken_  = token;
    accessTokenExpiry_  = now + expiresIn;
    return token;
  } catch (const std::exception& e) {
    (void)e;
    log::error("FCM token exchange error");
    return "";
  }
}

Json::Value PushService::sendViaFcm(const std::string& token,
                                    const Json::Value& notification,
                                    const Json::Value& data) {
  Json::Value result(Json::objectValue);
  try {
    // Build the message body matching the firebase-admin message object,
    // expressed in the FCM HTTP v1 JSON shape.
    Json::Value message(Json::objectValue);
    message["token"] = token;

    Json::Value notif(Json::objectValue);
    notif["title"] = notification.get("title", Json::Value(Json::nullValue));
    notif["body"]  = notification.get("body", Json::Value(Json::nullValue));
    if (notification.isMember("imageUrl") && !notification["imageUrl"].isNull() &&
        !(notification["imageUrl"].isString() && notification["imageUrl"].asString().empty())) {
      notif["image"] = notification["imageUrl"];  // FCM v1 field name is "image".
    }
    message["notification"] = notif;

    // data: { ...data, click_action: 'FLUTTER_NOTIFICATION_CLICK' }.
    // FCM v1 requires all data values to be strings.
    Json::Value outData(Json::objectValue);
    if (data.isObject()) {
      for (const auto& key : data.getMemberNames()) {
        const Json::Value& v = data[key];
        outData[key] = v.isString() ? v.asString() : toCompactJson(v);
      }
    }
    outData["click_action"] = "FLUTTER_NOTIFICATION_CLICK";
    message["data"] = outData;

    Json::Value android(Json::objectValue);
    android["priority"] = "high";
    Json::Value androidNotif(Json::objectValue);
    androidNotif["channel_id"] = "pulse_notifications";
    androidNotif["notification_priority"] = "PRIORITY_HIGH";
    androidNotif["sound"] = "default";
    android["notification"] = androidNotif;
    message["android"] = android;

    Json::Value apns(Json::objectValue);
    Json::Value apnsPayload(Json::objectValue);
    Json::Value aps(Json::objectValue);
    aps["sound"] = "default";
    aps["badge"] = 1;
    apnsPayload["aps"] = aps;
    apns["payload"] = apnsPayload;
    message["apns"] = apns;

    Json::Value envelope(Json::objectValue);
    envelope["message"] = message;

    std::string accessToken = getAccessToken();
    if (accessToken.empty()) {
      log::error("FCM push notification error: no access token");
      result["success"] = false;
      result["error"]   = "Failed to obtain FCM access token";
      return result;
    }

    auto client = drogon::HttpClient::newHttpClient("https://fcm.googleapis.com");
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setPath("/v1/projects/" + projectId_ + "/messages:send");
    req->setContentTypeString("application/json");
    req->addHeader("Authorization", "Bearer " + accessToken);
    req->setBody(toCompactJson(envelope));

    auto [reqResult, resp] = client->sendRequest(
        req, pulse::services::outboundHttpTimeoutSeconds());
    if (reqResult != drogon::ReqResult::Ok || !resp) {
      log::error("FCM push notification error: request failed");
      result["success"] = false;
      result["error"]   = "request failed";
      return result;
    }

    Json::Value parsed = parseJson(std::string(resp->getBody()));
    int status = resp->getStatusCode();
    if (status >= 200 && status < 300 && parsed.isMember("name")) {
      std::string name = parsed["name"].asString();
      log::info("FCM push notification sent");
      result["success"]   = true;
      result["messageId"] = name;
      return result;
    }

    // Error: map FCM v1 error codes to the firebase-admin invalid-token codes.
    std::string errorMessage = "FCM send failed";
    std::string errorStatus;
    if (parsed.isMember("error") && parsed["error"].isObject()) {
      const Json::Value& err = parsed["error"];
      errorMessage = err.get("message", errorMessage).asString();
      errorStatus  = err.get("status", "").asString();
      // FCM v1 surfaces invalid tokens as UNREGISTERED / INVALID_ARGUMENT,
      // equivalent to firebase-admin's
      // 'messaging/registration-token-not-registered' /
      // 'messaging/invalid-registration-token'.
      if (err.isMember("details") && err["details"].isArray()) {
        for (const auto& d : err["details"]) {
          if (d.isObject() && d.get("errorCode", "").asString() == "UNREGISTERED") {
            errorStatus = "UNREGISTERED";
          }
        }
      }
    }
    log::error("FCM push notification rejected (status {})", status);
    if (errorStatus == "UNREGISTERED" || errorStatus == "INVALID_ARGUMENT" ||
        errorStatus == "NOT_FOUND") {
      result["success"]      = false;
      result["invalidToken"] = true;
      result["error"]        = errorMessage;
      return result;
    }
    result["success"] = false;
    result["error"]   = errorMessage;
    return result;
  } catch (const std::exception& e) {
    log::error("FCM push notification error");
    result["success"] = false;
    result["error"]   = std::string(e.what());
    return result;
  }
}

Json::Value PushService::sendToToken(const std::string& token,
                                     const Json::Value& notification,
                                     const Json::Value& data) {
  // Expo token -> Expo API (no Firebase needed).
  if (isExpoPushToken(token)) {
    return sendViaExpo(token, notification, data);
  }

  // Otherwise FCM. If not initialized, JS returns null.
  if (!firebaseInitialized_) {
    log::warn("Firebase not initialized and token is not Expo - skipping push");
    return Json::Value(Json::nullValue);
  }
  return sendViaFcm(token, notification, data);
}

// ─────────────────────────────────────────────────────────────────────────
// sendToUser
// ─────────────────────────────────────────────────────────────────────────
Json::Value PushService::sendToUser(const std::string& userId,
                                    const Json::Value& notification,
                                    const Json::Value& data) {
  Json::Value out(Json::objectValue);
  try {
    auto id = bsonjson::tryOid(userId);
    auto col = db::collection(models::user::kCollection);

    // User.findById(userId).select('fcmTokens settings.pushNotifications')
    mongocxx::options::find opts;
    opts.projection(make_document(kvp("fcmTokens", 1),
                                  kvp("settings.pushNotifications", 1)));

    bsoncxx::v_noabi::stdx::optional<bsoncxx::document::value> userDocOpt;
    if (id) {
      userDocOpt = col.find_one(make_document(kvp("_id", *id)).view(), opts);
    }
    if (!userDocOpt) {
      out["success"] = false;
      out["reason"]  = "User not found";
      return out;
    }

    Json::Value user = bsonjson::toJson(userDocOpt->view());

    // user.settings.pushNotifications === false
    if (user.isMember("settings") && user["settings"].isObject() &&
        user["settings"].isMember("pushNotifications") &&
        user["settings"]["pushNotifications"].isBool() &&
        user["settings"]["pushNotifications"].asBool() == false) {
      out["success"] = false;
      out["reason"]  = "User disabled push notifications";
      return out;
    }

    // const tokens = user.fcmTokens || []
    Json::Value tokens = user.isMember("fcmTokens") && user["fcmTokens"].isArray()
                             ? user["fcmTokens"]
                             : Json::Value(Json::arrayValue);
    if (tokens.empty()) {
      out["success"] = false;
      out["reason"]  = "No FCM tokens registered";
      return out;
    }

    // Send to all of the user's devices.
    Json::Value results(Json::arrayValue);
    std::vector<std::string> invalidTokens;
    int successCount = 0;

    for (const auto& tokenData : tokens) {
      std::string tk  = tokenData.get("token", "").asString();
      Json::Value res = sendToToken(tk, notification, data);

      // { token, deviceId, ...result }
      Json::Value entry(Json::objectValue);
      entry["token"]    = tokenData.get("token", Json::Value(Json::nullValue));
      entry["deviceId"] = tokenData.get("deviceId", Json::Value(Json::nullValue));
      if (res.isObject()) {
        for (const auto& k : res.getMemberNames()) entry[k] = res[k];
      }
      results.append(entry);

      if (entry.get("invalidToken", false).asBool()) invalidTokens.push_back(tk);
      if (entry.get("success", false).asBool()) ++successCount;
    }

    // Remove invalid tokens:
    // User.findByIdAndUpdate(userId, { $pull: { fcmTokens: { token: { $in: invalidTokens } } } })
    if (!invalidTokens.empty() && id) {
      bld::array inArr;
      for (const auto& t : invalidTokens) inArr.append(t);
      auto update = make_document(kvp(
          "$pull",
          make_document(kvp(
              "fcmTokens",
              make_document(kvp("token", make_document(kvp("$in", inArr))))))));
      col.update_one(make_document(kvp("_id", *id)).view(), update.view());
      log::info("Removed {} invalid FCM tokens", invalidTokens.size());
    }

    out["success"] = successCount > 0;
    out["sent"]    = successCount;
    out["failed"]  = static_cast<int>(results.size()) - successCount;
    out["results"] = results;
    return out;
  } catch (const std::exception& e) {
    log::error("Send to user error");
    out["success"] = false;
    out["error"]   = std::string(e.what());
    return out;
  }
}

// ─────────────────────────────────────────────────────────────────────────
// registerToken
// ─────────────────────────────────────────────────────────────────────────
Json::Value PushService::registerToken(const std::string& userId,
                                       const std::string& token,
                                       const std::string& deviceId,
                                       const std::string& platform) {
  Json::Value out(Json::objectValue);
  if (!isValidRegistrationToken(token) || !isValidDeviceId(deviceId) ||
      !isValidPlatform(platform)) {
    out["success"] = false;
    out["error"] = "Invalid push token registration";
    return out;
  }

  try {
    auto id = bsonjson::oid(userId);  // throws if invalid (caught below)
    auto col = db::collection(models::user::kCollection);

    // 1) Remove this token from any other user (token transfer on login):
    // User.updateMany({ _id: { $ne: userId }, 'fcmTokens.token': token },
    //                 { $pull: { fcmTokens: { token } } })
    {
      auto filter = make_document(
          kvp("_id", make_document(kvp("$ne", id))),
          kvp("fcmTokens.token", token));
      auto update = make_document(
          kvp("$pull", make_document(kvp("fcmTokens",
                                         make_document(kvp("token", token))))));
      col.update_many(filter.view(), update.view());
    }

    // 2) Remove old token for this device from the current user:
    // User.findByIdAndUpdate(userId, { $pull: { fcmTokens: { deviceId } } })
    {
      auto update = make_document(
          kvp("$pull", make_document(kvp("fcmTokens",
                                         make_document(kvp("deviceId", deviceId))))));
      col.update_one(make_document(kvp("_id", id)).view(), update.view());
    }

    // 3) Add the new token at the front and cap retained devices. `$slice`
    // executes as part of the atomic push, so concurrent registrations cannot
    // grow the array beyond the configured bound.
    {
      bsoncxx::types::b_date now{std::chrono::milliseconds(bsonjson::nowMillis())};
      bld::array each;
      each.append(make_document(kvp("token", token), kvp("deviceId", deviceId),
                                kvp("platform", platform), kvp("lastUsed", now)));
      auto update = make_document(kvp(
          "$push",
          make_document(kvp(
              "fcmTokens",
              make_document(kvp("$each", each), kvp("$position", 0),
                            kvp("$slice", kMaxRegisteredDevices))))));
      auto result = col.update_one(make_document(kvp("_id", id)).view(), update.view());
      if (!result || result->matched_count() != 1) {
        out["success"] = false;
        out["error"] = "User not found";
        return out;
      }
    }

    log::info("Push token registered");
    out["success"] = true;
    return out;
  } catch (const std::exception&) {
    log::error("Register token error");
    out["success"] = false;
    out["error"]   = "Failed to register token";
    return out;
  }
}

// ─────────────────────────────────────────────────────────────────────────
// unregisterToken
// ─────────────────────────────────────────────────────────────────────────
Json::Value PushService::unregisterToken(const std::string& userId,
                                         const std::string& deviceId) {
  Json::Value out(Json::objectValue);
  if (!isValidDeviceId(deviceId)) {
    out["success"] = false;
    out["error"] = "Invalid deviceId";
    return out;
  }

  try {
    auto id = bsonjson::oid(userId);
    auto col = db::collection(models::user::kCollection);

    // User.findByIdAndUpdate(userId, { $pull: { fcmTokens: { deviceId } } })
    auto update = make_document(
        kvp("$pull", make_document(kvp("fcmTokens",
                                       make_document(kvp("deviceId", deviceId))))));
    col.update_one(make_document(kvp("_id", id)).view(), update.view());

    log::info("Push token unregistered");
    out["success"] = true;
    return out;
  } catch (const std::exception&) {
    log::error("Unregister token error");
    out["success"] = false;
    out["error"]   = "Failed to unregister token";
    return out;
  }
}

} // namespace pulse
