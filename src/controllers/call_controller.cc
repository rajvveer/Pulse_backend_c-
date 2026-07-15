// call_controller.cc — implementation of pulse::controllers::CallController.
//
// See the header for the endpoint contract. The heavy lifting is delegated to
// LiveKitService (token minting) and PushService (incoming-call wake). DB access
// here is read-only: resolve the conversation and the peer participant.
#include "pulse/controllers/call_controller.hpp"

#include <algorithm>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <openssl/rand.h>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <mongocxx/options/find.hpp>

#include "pulse/bson_json.hpp"
#include "pulse/cache.hpp"
#include "pulse/db.hpp"
#include "pulse/http_response.hpp"
#include "pulse/logger.hpp"
#include "pulse/models/conversation.hpp"
#include "pulse/models/user.hpp"
#include "pulse/services/livekit_service.hpp"
#include "pulse/services/push_service.hpp"

using namespace pulse::controllers;
namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;
using bld::make_array;

namespace {

// req.user.userId from the AuthFilter-populated attribute.
std::string authedUserId(const drogon::HttpRequestPtr& req) {
  const auto& user = req->getAttributes()->get<Json::Value>("user");
  return user["userId"].asString();
}

std::string authedUsername(const drogon::HttpRequestPtr& req) {
  const auto& user = req->getAttributes()->get<Json::Value>("user");
  return user.isMember("username") && user["username"].isString()
             ? user["username"].asString() : "";
}

std::string str(const Json::Value& v, const char* key) {
  return v.isObject() && v.isMember(key) && v[key].isString()
             ? v[key].asString() : "";
}

// 16-byte random hex — opaque call id (the per-call room suffix + push key).
std::string randomCallId() {
  unsigned char bytes[16];
  if (RAND_bytes(bytes, static_cast<int>(sizeof(bytes))) != 1)
    throw std::runtime_error("Secure call ID generation failed");

  static const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(32);
  for (unsigned char byte : bytes) {
    out.push_back(hex[(byte >> 4) & 0x0f]);
    out.push_back(hex[byte & 0x0f]);
  }
  return out;
}

// LiveKit participant identity: "<userId>#<deviceId>" so the same user on two
// devices is two distinct participants. deviceId falls back to "default".
std::string identityFor(const std::string& userId, const std::string& deviceId) {
  return userId + "#" + (deviceId.empty() ? "default" : deviceId);
}

bool hasParticipant(const Json::Value& conversation, const std::string& userId) {
  if (!conversation.isMember("participants") ||
      !conversation["participants"].isArray()) return false;
  for (const auto& participant : conversation["participants"]) {
    if (participant.isString() && participant.asString() == userId) return true;
    if (participant.isObject() && participant.get("_id", "").asString() == userId)
      return true;
  }
  return false;
}

bool validCallId(const std::string& callId) {
  if (callId.size() != 32) return false;
  return std::all_of(callId.begin(), callId.end(), [](unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}

std::string callIdFromRoom(const std::string& room) {
  const auto separator = room.rfind('_');
  if (separator == std::string::npos) return "";
  const std::string callId = room.substr(separator + 1);
  return validCallId(callId) ? callId : "";
}

std::optional<Json::Value> loadCallState(const std::string& callId) {
  if (!validCallId(callId)) return std::nullopt;
  auto raw = pulse::cache().get("call:" + callId);
  if (!raw || raw->empty()) return std::nullopt;

  Json::Value state;
  Json::CharReaderBuilder builder;
  std::string errors;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  if (!reader->parse(raw->data(), raw->data() + raw->size(), &state, &errors) ||
      !state.isObject() || str(state, "callId") != callId) {
    return std::nullopt;
  }
  return state;
}

bool isCallParty(const Json::Value& state, const std::string& userId) {
  return str(state, "caller") == userId || str(state, "callee") == userId;
}

}  // namespace

// ── POST /api/v1/calls/initiate ───────────────────────────────────────────────
// Body: { conversationId, calleeId?, callType: 'audio'|'video', deviceId? }
//   - conversationId: the DM/group the call belongs to (used to authorize +
//     resolve the callee when calleeId is omitted in a 1:1).
//   - calleeId: explicit target (required for groups; optional for 1:1 DMs where
//     it's the other participant).
// Returns: { success, callId, room, token, wsUrl, callType, callee, caller }.
void CallController::initiate(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    if (!pulse::livekit().isConfigured()) {
      callback(pulse::http::error(drogon::k503ServiceUnavailable,
                                  "Calling is not configured on the server",
                                  "CALLING_NOT_CONFIGURED"));
      return;
    }

    const std::string userId = authedUserId(req);
    auto bodyPtr = req->getJsonObject();
    const Json::Value body = bodyPtr ? *bodyPtr : Json::Value(Json::objectValue);

    const std::string conversationId = str(body, "conversationId");
    std::string calleeId = str(body, "calleeId");
    std::string callType = str(body, "callType");
    if (callType != "video") callType = "audio";  // default audio
    const std::string deviceId = str(body, "deviceId");

    if (conversationId.empty() || !pulse::bsonjson::isValidOid(conversationId)) {
      callback(pulse::http::badRequest("A valid conversationId is required",
                                       "INVALID_CONVERSATION"));
      return;
    }

    // Authorize: caller must be a participant of the conversation. Load it so we
    // can also resolve the callee (the other participant) for a 1:1.
    auto convCol = pulse::db::collection(pulse::models::conversation::kCollection);
    auto convDoc = convCol.find_one(make_document(
        kvp("_id", pulse::bsonjson::oid(conversationId)),
        kvp("participants", pulse::bsonjson::oid(userId))));
    if (!convDoc) {
      callback(pulse::http::forbidden("Not a participant of this conversation",
                                      "NOT_A_PARTICIPANT"));
      return;
    }
    Json::Value conv = pulse::bsonjson::toJson(convDoc->view());

    // Resolve the callee. For a 1:1 (2 participants) with no explicit calleeId,
    // it's the participant that isn't the caller. For a group, calleeId is
    // required (we ring one specific member; group fan-out is a later concern).
    const bool isGroup = conv.get("type", "direct").asString() == "group";
    if (calleeId.empty() && isGroup) {
      callback(pulse::http::badRequest(
          "calleeId is required for group calls", "NO_CALLEE"));
      return;
    }
    if (calleeId.empty()) {
      if (conv.isMember("participants") && conv["participants"].isArray()) {
        for (const auto& p : conv["participants"]) {
          if (p.isString() && p.asString() != userId) {
            calleeId = p.asString();
            break;  // first non-self participant (correct for a 1:1)
          }
        }
      }
    }
    if (calleeId.empty() || !pulse::bsonjson::isValidOid(calleeId)) {
      callback(pulse::http::badRequest(
          "Could not resolve a callee; provide calleeId", "NO_CALLEE"));
      return;
    }
    if (calleeId == userId) {
      callback(pulse::http::badRequest("Cannot call yourself", "SELF_CALL"));
      return;
    }
    if (!hasParticipant(conv, calleeId)) {
      callback(pulse::http::forbidden(
          "Callee is not a participant of this conversation",
          "CALLEE_NOT_PARTICIPANT"));
      return;
    }

    // Allocate the call. Room name is stable + unguessable; both sides join it.
    const std::string callId = randomCallId();
    const std::string room = "call_" + conversationId + "_" + callId;

    // Mint the CALLER's join token.
    const std::string callerIdentity = identityFor(userId, deviceId);
    const std::string callerToken = pulse::livekit().mintToken(
        room, callerIdentity, authedUsername(req));
    if (callerToken.empty()) {
      callback(pulse::http::serverError("Failed to mint call token",
                                        "TOKEN_MINT_FAILED"));
      return;
    }

    // Stash minimal call state in Redis (TTL 2 min — a ring that isn't answered
    // by then is dead). Lets a later /calls/token validate the room belongs to a
    // real, recent call and lets /calls/end clear it.
    {
      Json::Value state(Json::objectValue);
      state["callId"] = callId;
      state["room"] = room;
      state["conversationId"] = conversationId;
      state["caller"] = userId;
      state["callee"] = calleeId;
      state["callType"] = callType;
      Json::StreamWriterBuilder w; w["indentation"] = "";
      if (!pulse::cache().set("call:" + callId, Json::writeString(w, state), 120)) {
        callback(pulse::http::serverError("Failed to create call state",
                                          "CALL_STATE_FAILED"));
        return;
      }
    }

    // Look up the caller's display profile for the push/ring (best-effort).
    Json::Value callerProfile(Json::objectValue);
    callerProfile["_id"] = userId;
    callerProfile["username"] = authedUsername(req);
    try {
      auto uCol = pulse::db::collection(pulse::models::user::kCollection);
      mongocxx::options::find opts;
      opts.projection(make_document(kvp("username", 1), kvp("name", 1),
                                    kvp("avatar", 1), kvp("profile.avatar", 1)));
      auto uDoc = uCol.find_one(
          make_document(kvp("_id", pulse::bsonjson::oid(userId))), opts);
      if (uDoc) {
        Json::Value u = pulse::bsonjson::toJson(uDoc->view());
        if (u.isMember("username")) callerProfile["username"] = u["username"];
        if (u.isMember("name")) callerProfile["name"] = u["name"];
        // avatar: top-level or profile.avatar
        if (u.isMember("avatar") && u["avatar"].isString() && !u["avatar"].asString().empty())
          callerProfile["avatar"] = u["avatar"];
        else if (u.isMember("profile") && u["profile"].isObject() &&
                 u["profile"].isMember("avatar"))
          callerProfile["avatar"] = u["profile"]["avatar"];
      }
    } catch (...) { /* best-effort */ }

    // Fire the incoming-call push to the callee (wakes a backgrounded app). The
    // ws call_invite (sent by the caller's client in parallel) covers the
    // foregrounded case. Data-only fields drive the IncomingCallScreen.
    const std::string callerName =
        callerProfile.isMember("username") && callerProfile["username"].isString() &&
                !callerProfile["username"].asString().empty()
            ? callerProfile["username"].asString()
            : (callerProfile.isMember("name") && callerProfile["name"].isString()
                   ? callerProfile["name"].asString() : "Someone");
    try {
      Json::Value notification(Json::objectValue);
      notification["title"] = callType == "video" ? "Incoming video call"
                                                   : "Incoming voice call";
      notification["body"] = callerName + " is calling…";

      Json::Value data(Json::objectValue);
      data["type"] = "incoming_call";        // PushNotificationHandler routes on this
      data["callId"] = callId;
      data["room"] = room;
      data["conversationId"] = conversationId;
      data["callType"] = callType;
      data["callerId"] = userId;
      data["callerName"] = callerName;
      if (callerProfile.isMember("avatar"))
        data["callerAvatar"] = callerProfile["avatar"];

      // Async-ish: this does a synchronous HTTP round-trip to FCM/Expo on the
      // Drogon worker thread. Acceptable per the project's blocking-IO convention;
      // the caller's UI doesn't wait on the push result (we already have the
      // token to return). Fire and (mostly) forget.
      pulse::PushService::instance().sendToUser(calleeId, notification, data);
    } catch (const std::exception& e) {
      pulse::log::error("Call push to callee failed: {}", e.what());
      // Non-fatal: the ws ring may still reach a foregrounded peer.
    }

    Json::Value out(Json::objectValue);
    out["callId"] = callId;
    out["room"] = room;
    out["token"] = callerToken;
    out["wsUrl"] = pulse::livekit().wsUrl();
    out["callType"] = callType;
    out["conversationId"] = conversationId;
    out["callee"] = calleeId;
    out["caller"] = callerProfile;
    out["identity"] = callerIdentity;
    callback(pulse::http::success(out));
  } catch (const std::exception& e) {
    pulse::log::error("Call initiate error: {}", e.what());
    callback(pulse::http::serverError("Failed to initiate call",
                                      "CALL_INITIATE_FAILED"));
  }
}

// ── POST /api/v1/calls/token ──────────────────────────────────────────────────
// Body: { room, callId?, deviceId? }
// Mint a join token for an already-known room (the callee uses this on accept).
// Returns: { success, token, wsUrl, identity, room }.
void CallController::token(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    if (!pulse::livekit().isConfigured()) {
      callback(pulse::http::error(drogon::k503ServiceUnavailable,
                                  "Calling is not configured on the server",
                                  "CALLING_NOT_CONFIGURED"));
      return;
    }

    const std::string userId = authedUserId(req);
    auto bodyPtr = req->getJsonObject();
    const Json::Value body = bodyPtr ? *bodyPtr : Json::Value(Json::objectValue);

    const std::string room = str(body, "room");
    std::string callId = str(body, "callId");
    const std::string deviceId = str(body, "deviceId");
    if (room.empty()) {
      callback(pulse::http::badRequest("room is required", "INVALID_ROOM"));
      return;
    }

    if (callId.empty()) callId = callIdFromRoom(room);
    if (room.rfind("call_", 0) != 0 || !validCallId(callId)) {
      callback(pulse::http::badRequest("Invalid room", "INVALID_ROOM"));
      return;
    }

    // Fail closed unless this exact room was allocated by /calls/initiate and
    // the requesting user is one of its two authorized parties.
    auto state = loadCallState(callId);
    if (!state || str(*state, "room") != room) {
      callback(pulse::http::error(drogon::k404NotFound,
                                  "Call not found or expired", "CALL_NOT_FOUND"));
      return;
    }
    if (!isCallParty(*state, userId)) {
      callback(pulse::http::forbidden("Not authorized for this call",
                                      "NOT_CALL_PARTICIPANT"));
      return;
    }

    // Re-check current conversation membership in case the user was removed
    // after the invitation was created.
    const std::string conversationId = str(*state, "conversationId");
    auto conversationOid = pulse::bsonjson::tryOid(conversationId);
    auto userOid = pulse::bsonjson::tryOid(userId);
    if (!conversationOid || !userOid) {
      callback(pulse::http::forbidden("Not authorized for this call",
                                      "NOT_CALL_PARTICIPANT"));
      return;
    }
    auto conversations = pulse::db::collection(
        pulse::models::conversation::kCollection);
    auto membership = conversations.find_one(make_document(
        kvp("_id", *conversationOid), kvp("participants", *userOid)));
    if (!membership) {
      callback(pulse::http::forbidden("Not authorized for this call",
                                      "NOT_CALL_PARTICIPANT"));
      return;
    }

    const std::string identity = identityFor(userId, deviceId);
    const std::string tk = pulse::livekit().mintToken(
        room, identity, authedUsername(req));
    if (tk.empty()) {
      callback(pulse::http::serverError("Failed to mint call token",
                                        "TOKEN_MINT_FAILED"));
      return;
    }

    Json::Value out(Json::objectValue);
    out["token"] = tk;
    out["wsUrl"] = pulse::livekit().wsUrl();
    out["identity"] = identity;
    out["room"] = room;
    out["callId"] = callId;
    callback(pulse::http::success(out));
  } catch (const std::exception& e) {
    pulse::log::error("Call token error: {}", e.what());
    callback(pulse::http::serverError("Failed to mint call token",
                                      "CALL_TOKEN_FAILED"));
  }
}

// ── POST /api/v1/calls/end ────────────────────────────────────────────────────
// Body: { callId? }
// Best-effort: clear the Redis call state. The ws call_end event handles peer
// notification; LiveKit tears the media down on client disconnect. Always 200.
void CallController::end(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    auto bodyPtr = req->getJsonObject();
    const Json::Value body = bodyPtr ? *bodyPtr : Json::Value(Json::objectValue);
    const std::string callId = str(body, "callId");
    if (!validCallId(callId)) {
      callback(pulse::http::badRequest("A valid callId is required",
                                       "INVALID_CALL"));
      return;
    }

    auto state = loadCallState(callId);
    if (!state) {
      callback(pulse::http::error(drogon::k404NotFound,
                                  "Call not found or expired", "CALL_NOT_FOUND"));
      return;
    }
    if (!isCallParty(*state, authedUserId(req))) {
      callback(pulse::http::forbidden("Not authorized for this call",
                                      "NOT_CALL_PARTICIPANT"));
      return;
    }

    pulse::cache().del("call:" + callId);
    callback(pulse::http::success());
  } catch (const std::exception& e) {
    pulse::log::error("Call end error: {}", e.what());
    callback(pulse::http::serverError("Failed to end call", "CALL_END_FAILED"));
  }
}
