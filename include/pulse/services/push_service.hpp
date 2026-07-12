// push_service.hpp — port of src/services/pushService.js.
//
// Push notifications to user devices. Auto-detects Expo push tokens
// ("ExponentPushToken[...]") vs raw FCM tokens:
//   - Expo tokens go to Expo's push API (exp.host/--/api/v2/push/send).
//   - FCM tokens go through the Firebase Cloud Messaging HTTP v1 API
//     (fcm.googleapis.com/v1/projects/{projectId}/messages:send), which the
//     Node `firebase-admin` SDK used under the hood. The service-account
//     credentials (projectId / clientEmail / privateKey) are read from env at
//     construction, a short-lived OAuth2 access token is minted from a signed
//     service-account JWT, and FCM messages are POSTed with it.
//
// Token storage lives on User.fcmTokens (array of { token, deviceId, platform,
// lastUsed }); invalid tokens are pruned after a send. Every response field
// name, payload shape, and Mongo query mirrors the JS source 1:1.
//
// Exposed as a class with a singleton accessor (mirroring the JS module
// singleton). `initializeFirebase()` runs from the constructor, matching the
// JS "Initialize on module load" call.
#pragma once
#include <string>
#include <optional>
#include <json/json.h>

namespace pulse {

class PushService {
public:
  // Singleton accessor (the JS module exported a single object).
  static PushService& instance();

  // Re-run Firebase init (idempotent). Init also runs from the constructor.
  void initializeFirebase();

  // Whether Firebase Admin is configured and ready.
  bool isFirebaseReady() const;

  // isExpoPushToken(token) — token starts with "ExponentPushToken[".
  static bool isExpoPushToken(const std::string& token);

  // sendViaExpo(expoPushToken, notification, data) — POST to Expo push API.
  // notification: { title, body, imageUrl? } ; data: arbitrary object.
  // Returns { success, messageId?, invalidToken?, error? }.
  Json::Value sendViaExpo(const std::string& expoPushToken,
                          const Json::Value& notification,
                          const Json::Value& data = Json::Value(Json::objectValue));

  // sendToToken(token, notification, data) — auto-detects Expo vs FCM.
  // Returns the send-result object, OR null (objectValue isNull) when the token
  // is not an Expo token and Firebase is not initialized (JS returns `null`).
  Json::Value sendToToken(const std::string& token,
                          const Json::Value& notification,
                          const Json::Value& data = Json::Value(Json::objectValue));

  // sendToUser(userId, notification, data) — fan out to all of a user's
  // fcmTokens, prune invalid ones. Returns
  // { success, sent, failed, reason?, error?, results? }.
  Json::Value sendToUser(const std::string& userId,
                         const Json::Value& notification,
                         const Json::Value& data = Json::Value(Json::objectValue));

  // registerToken(userId, token, deviceId, platform) — token transfer + upsert
  // for the device. Returns { success } / { success:false, error }.
  Json::Value registerToken(const std::string& userId,
                            const std::string& token,
                            const std::string& deviceId,
                            const std::string& platform);

  // unregisterToken(userId, deviceId) — pull the device's token (on logout).
  // Returns { success } / { success:false, error }.
  Json::Value unregisterToken(const std::string& userId,
                              const std::string& deviceId);

private:
  PushService();

  // Send one message via the FCM HTTP v1 API. Returns the same result shape as
  // sendToToken's FCM branch.
  Json::Value sendViaFcm(const std::string& token,
                         const Json::Value& notification,
                         const Json::Value& data);

  // Mint / return a cached OAuth2 access token for the FCM scope. Empty on
  // failure.
  std::string getAccessToken();

  bool firebaseInitialized_ = false;

  // Service-account credentials (from FIREBASE_* env / service account file).
  std::string projectId_;
  std::string clientEmail_;
  std::string privateKey_;   // PEM, with real newlines

  // Cached OAuth2 access token + its expiry (epoch seconds).
  std::string cachedAccessToken_;
  long long   accessTokenExpiry_ = 0;
};

// Convenience free function mirroring the JS singleton usage.
inline PushService& pushService() { return PushService::instance(); }

} // namespace pulse
