// realtime_controller.cc — implementation of the realtime chat WebSocket
// endpoint. Ports src/sockets/realtime.js + the Socket.IO handshake/connection
// wiring in src/server.js (lines 120-245).
//
// Parity contract (do NOT change without changing the mobile client):
//   * Every handler is registered under ALL incoming event aliases (underscored
//     AND hyphenated), and every outgoing event is emitted under ALL aliases —
//     because an installed APK cannot be updated, so old + new clients must meet
//     here. The exact alias lists below are copied from realtime.js.
//   * Payload field names / shapes are byte-for-byte identical to realtime.js.
//   * Membership checks, Redis presence keys, preview-text emojis, the per-socket
//     token-bucket rate limiter, and the fire-and-forget conversation-summary
//     write are all reproduced 1:1.
//   * Cross-instance fan-out (the Socket.IO Redis adapter) is reproduced with a
//     Redis pub/sub channel per room.
#include "pulse/sockets/realtime_controller.hpp"

#include "pulse/jwt_service.hpp"
#include "pulse/cache.hpp"
#include "pulse/services/presence_service.hpp"
#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"
#include "pulse/models/message.hpp"
#include "pulse/models/conversation.hpp"
#include "pulse/models/user.hpp"
#include "pulse/filters/auth_filters.hpp"
#include "pulse/config.hpp"

#include <sw/redis++/redis++.h>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/oid.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/find_one_and_update.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace drogon;

namespace pulse::sockets {

std::atomic<RealtimeController*> RealtimeController::activeInstance_{nullptr};

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;
using bld::make_array;

namespace {

// ── time / json helpers ──────────────────────────────────────────────────────
std::int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bsoncxx::types::b_date nowDate() {
  return bsoncxx::types::b_date{std::chrono::system_clock::now()};
}

// Compact JSON (no whitespace), matching Socket.IO's JSON wire form.
std::string toCompactJson(const Json::Value& v) {
  Json::StreamWriterBuilder b;
  b["indentation"] = "";
  return Json::writeString(b, v);
}

bool parseJson(const std::string& s, Json::Value& out) {
  Json::CharReaderBuilder b;
  std::string errs;
  std::unique_ptr<Json::CharReader> reader(b.newCharReader());
  return reader->parse(s.data(), s.data() + s.size(), &out, &errs);
}

// Read a string field ("" when absent/null/non-string).
std::string str(const Json::Value& v, const char* key) {
  return v.isObject() && v.isMember(key) && v[key].isString()
             ? v[key].asString()
             : "";
}

// realtime.js accepts either a raw string id or { conversationId } object.
//   const conversationId = typeof data === 'string' ? data : data?.conversationId
std::string conversationIdFrom(const Json::Value& data) {
  if (data.isString()) return data.asString();
  if (data.isObject() && data.isMember("conversationId") &&
      data["conversationId"].isString())
    return data["conversationId"].asString();
  return "";
}

// mongoose.isValidObjectId
bool isValidOid(const std::string& hex) { return pulse::bsonjson::isValidOid(hex); }

bool isValidCallId(const std::string& value) {
  return value.size() == 32 &&
         std::all_of(value.begin(), value.end(), [](unsigned char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

bool hasParticipant(const Json::Value& conversation, const std::string& userId) {
  if (!conversation.isObject() || !conversation["participants"].isArray())
    return false;
  for (const auto& participant : conversation["participants"]) {
    if (participant.isString() && participant.asString() == userId) return true;
    if (participant.isObject() &&
        participant.get("_id", "").asString() == userId) return true;
  }
  return false;
}

// ── Multi-user safety caps ──
// Max concurrent sockets per user (a handful of devices/tabs is plenty; beyond
// this is abuse). Bounds per-user rate budget + memory.
int maxConnectionsPerUser() {
  static const int value = static_cast<int>(std::clamp<std::int64_t>(
      pulse::config().envInt("WS_MAX_CONNECTIONS_PER_USER", 10), 1, 100));
  return value;
}

// Bound allocation and JSON parsing before database-backed auth revalidation.
std::size_t maxInboundFrameBytes() {
  static const auto value = static_cast<std::size_t>(std::clamp<std::int64_t>(
      pulse::config().envInt("WS_MAX_FRAME_BYTES", 64 * 1024),
      1024, 1024 * 1024));
  return value;
}
// Max rooms a single connection may join (conversations + location). Stops a
// client from growing rooms_ without bound via join-room/join-location.
constexpr size_t kMaxRoomsPerConn = 200;
// Max persisted message content length (chars). Real chat messages are tiny;
// media goes through a separate upload path. Rejects oversized-frame DB bloat.
constexpr size_t kMaxMessageContentLen = 8192;

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  RealtimeLimiter — ports socketRateLimit.js (per-socket token bucket).
// ─────────────────────────────────────────────────────────────────────────────
bool RealtimeLimiter::allow(const std::string& category, std::int64_t now) {
  // LIMITS table from socketRateLimit.js (ratePerSec, burst).
  double ratePerSec, burst;
  if (category == "message")       { ratePerSec = 5;  burst = 10; }
  else if (category == "reaction") { ratePerSec = 8;  burst = 16; }
  else if (category == "typing")   { ratePerSec = 4;  burst = 8;  }
  else if (category == "presence") { ratePerSec = 1;  burst = 3;  }
  else                             { ratePerSec = 10; burst = 20; } // default

  auto it = buckets_.find(category);
  if (it == buckets_.end()) {
    Bucket b;
    b.ratePerSec = ratePerSec;
    b.burst = burst;
    b.tokens = burst;
    b.last = now;
    it = buckets_.emplace(category, b).first;
  }
  Bucket& b = it->second;

  // TokenBucket.tryRemove(now)
  double elapsed = (now - b.last) / 1000.0;
  if (elapsed > 0) {
    b.tokens = std::min(b.burst, b.tokens + elapsed * b.ratePerSec);
    b.last = now;
  }
  warnNow_ = false;
  if (b.tokens >= 1) {
    b.tokens -= 1;
    return true;
  }
  // Throttled notice every 20th drop (warned % 20 === 0).
  if (warned_ % 20 == 0) warnNow_ = true;
  warned_++;
  return false;
}

bool RealtimeLimiter::consumeWarn() {
  bool w = warnNow_;
  warnNow_ = false;
  return w;
}

// ─────────────────────────────────────────────────────────────────────────────
//  RealtimeController
// ─────────────────────────────────────────────────────────────────────────────
RealtimeController::RealtimeController() {
  activeInstance_.store(this);
  // A process-stable instance id so our own published frames are not delivered
  // back to us a second time (we already delivered them locally).
  std::ostringstream os;
  os << "inst-" << static_cast<const void*>(this) << "-" << nowMs();
  instanceId_ = os.str();

  try {
    pub_ = pulse::cache().createClient();
  } catch (const std::exception& e) {
    pulse::log::error("Realtime: failed to create Redis publisher: {}", e.what());
  }
  startSubscriber();
  startAuthMonitor();
}

RealtimeController::~RealtimeController() {
  stopAuthMonitor();
  stopSubscriber();
  RealtimeController* expected = this;
  activeInstance_.compare_exchange_strong(expected, nullptr);
}

void RealtimeController::shutdownInfrastructure() {
  RealtimeController* self = activeInstance_.load();
  if (!self) return;

  std::vector<WebSocketConnectionPtr> sockets;
  {
    std::lock_guard<std::mutex> lock(self->connMu_);
    sockets.reserve(self->conns_.size());
    for (const auto& entry : self->conns_) sockets.push_back(entry.second);
  }
  for (const auto& socket : sockets) {
    if (socket && socket->connected())
      socket->shutdown(CloseCode::kEndpointGone, "Server shutting down");
  }
  self->stopAuthMonitor();
  self->stopSubscriber();
}

void RealtimeController::startAuthMonitor() {
  if (authRunning_.exchange(true)) return;
  authThread_ = std::thread([this] {
    const auto interval = std::chrono::seconds(std::max<int64_t>(
        5, std::min<int64_t>(300,
            pulse::config().envInt("WS_AUTH_RECHECK_SEC", 30))));
    std::unique_lock<std::mutex> waitLock(authWaitMu_);
    while (authRunning_) {
      if (authWaitCv_.wait_for(waitLock, interval,
                               [this] { return !authRunning_.load(); }))
        break;
      waitLock.unlock();

      std::vector<WebSocketConnectionPtr> sockets;
      {
        std::lock_guard<std::mutex> lock(connMu_);
        sockets.reserve(conns_.size());
        for (const auto& [id, socket] : conns_) sockets.push_back(socket);
      }
      for (const auto& socket : sockets) {
        auto st = state(socket);
        if (!st) continue;
        pulse::AccessClaims claims;
        if (pulse::filters::validateAccessToken(st->accessToken, claims) !=
                pulse::filters::AccessTokenStatus::Valid ||
            claims.userId != st->userId) {
          socket->shutdown(CloseCode::kViolation,
                           "Authentication expired or revoked");
        } else if (st->presenceRegistered.load()) {
          pulse::presence().touch(st->userId);
        }
      }
      waitLock.lock();
    }
  });
}

void RealtimeController::stopAuthMonitor() {
  if (!authRunning_.exchange(false)) return;
  authWaitCv_.notify_all();
  if (authThread_.joinable()) authThread_.join();
}

std::shared_ptr<ConnState> RealtimeController::state(
    const WebSocketConnectionPtr& conn) {
  return conn->getContext<ConnState>();
}

std::string RealtimeController::connId(const WebSocketConnectionPtr& conn) {
  // Use the process-unique id assigned at connect (stored in ConnState), NOT
  // the raw pointer value — pointer addresses get reused after free and also
  // collide across processes, which corrupted conns_ keys and pub/sub
  // sender-exclusion (silent message loss / self-echo). Fall back to the
  // pointer only if state isn't attached yet (pre-auth).
  if (auto st = conn ? conn->getContext<ConnState>() : nullptr) {
    if (!st->id.empty()) return st->id;
  }
  std::ostringstream os;
  os << "p" << static_cast<const void*>(conn.get());
  return os.str();
}

// ── Handshake auth (server.js io.use) + user_<id> room join (server.js) ──────
void RealtimeController::handleNewConnection(const HttpRequestPtr& req,
                                             const WebSocketConnectionPtr& conn) {
  // Prefer Authorization so credentials do not enter proxy/access logs. Keep
  // the query parameter only as a legacy-client fallback.
  // (server.js read socket.handshake.auth.token; on raw WS the auth payload
  // travels as a query param or the standard Authorization header.)
  std::string token;
  const std::string auth = req->getHeader("authorization");
  if (auth.rfind("Bearer ", 0) == 0) token = auth.substr(7);
  if (token.empty()) token = req->getParameter("token");

  if (token.empty()) {
    pulse::log::warn("\xE2\x9B\x94 Socket connection rejected \xE2\x80\x94 no auth token provided");
    conn->forceClose();
    return;
  }

  pulse::AccessClaims claims;
  if (pulse::filters::validateAccessToken(token, claims) !=
      pulse::filters::AccessTokenStatus::Valid) {
    pulse::log::warn(
        "Socket connection rejected - invalid/revoked token or inactive session");
    conn->forceClose();
    return;
  }

  auto st = std::make_shared<ConnState>();
  st->userId = claims.userId;
  st->accessToken = token;
  // Process-unique connection id (used for conns_ keys + pub/sub exclusion).
  st->id = instanceId_ + ":" + std::to_string(connSeq_.fetch_add(1) + 1);
  // socket.user — lightweight identity (server.js).
  st->user["_id"] = claims.userId;
  st->user["username"] = claims.username;
  st->user["name"] = claims.username;
  st->user["isVerified"] = claims.isVerified;
  conn->setContext(st);

  // Per-user connection cap: reject (close) if this user already holds the max
  // concurrent sockets, so one user (or a stolen token) cannot open thousands of
  // connections to multiply their rate-limit budget and exhaust shared memory.
  {
    std::lock_guard<std::mutex> lk(connMu_);
    int& n = userConnCount_[st->userId];
    const int cap = maxConnectionsPerUser();
    if (n >= cap) {
      pulse::log::warn("Socket rejected — user {} at connection cap ({})",
                       st->userId, cap);
      // Drop the context so handleConnectionClosed doesn't double-decrement.
      conn->setContext(nullptr);
      conn->forceClose();
      return;
    }
    n += 1;
    conns_[st->id] = conn;
  }

  // Always join the user's own room (server.js: socket.join(`user_${userId}`)).
  if (!st->userId.empty()) joinRoom(conn, "user_" + st->userId);

  // Register presence on EVERY authenticated connection (paired with the
  // unconditional removeConnection in handleDisconnect). The mobile client does
  // not reliably emit an explicit 'user_online' event, so relying on that for
  // the INCR drove presence:count negative on the first disconnect and broke
  // presence for everyone. INCR here keeps the counter symmetric per socket.
  if (!st->userId.empty()) {
    try {
      bool registered = false;
      bool justCameOnline =
          pulse::presence().addConnection(st->userId, registered);
      st->presenceRegistered.store(registered);
      if (justCameOnline) notifyConversationPeers(conn, true);
    } catch (const std::exception& e) {
      pulse::log::error("presence addConnection error: {}", e.what());
    }
  }
}

// ── Frame dispatch (Socket.IO event multiplexing over one connection) ────────
void RealtimeController::handleNewMessage(const WebSocketConnectionPtr& conn,
                                          std::string&& message,
                                          const WebSocketMessageType& type) {
  auto st = state(conn);
  if (!st) return;

  // Control frames are handled by the transport and require no DB lookup.
  if (type == WebSocketMessageType::Ping ||
      type == WebSocketMessageType::Pong ||
      type == WebSocketMessageType::Close)
    return;

  if (type != WebSocketMessageType::Text) {
    conn->shutdown(CloseCode::kInvalidMessage, "Text frames required");
    return;
  }
  if (message.size() > maxInboundFrameBytes()) {
    conn->shutdown(CloseCode::kMessageTooBig, "Frame too large");
    return;
  }

  // Stop floods in memory before the DB-backed session revalidation below.
  if (!st->limiter.allow("default", nowMs())) {
    if (st->limiter.consumeWarn()) {
      Json::Value limited;
      limited["category"] = "default";
      emitTo(conn, "rate_limited", limited);
    }
    return;
  }

  pulse::AccessClaims currentClaims;
  if (pulse::filters::validateAccessToken(st->accessToken, currentClaims) !=
          pulse::filters::AccessTokenStatus::Valid ||
      currentClaims.userId != st->userId) {
    conn->shutdown(CloseCode::kViolation, "Authentication expired or revoked");
    return;
  }

  Json::Value frame;
  if (!parseJson(message, frame) || !frame.isObject()) return;

  const std::string event = str(frame, "event");
  if (event.empty()) return;
  const Json::Value data = frame.isMember("data") ? frame["data"] : Json::Value();
  std::int64_t ackId = -1;
  if (frame.isMember("ack") && frame["ack"].isIntegral()) ackId = frame["ack"].asInt64();

  // Incoming aliases mirror realtime.js onAny([...]) registrations exactly.
  if (event == "join_conversation" || event == "join-conversation") {
    handleJoin(conn, data);
  } else if (event == "leave_conversation" || event == "leave-conversation") {
    handleLeave(conn, data);
  } else if (event == "send_message" || event == "send-message") {
    handleSendMessage(conn, data, ackId);
  } else if (event == "mark_seen" || event == "mark-seen" ||
             event == "message_seen" || event == "message-seen") {
    handleMarkSeen(conn, data);
  } else if (event == "add_reaction" || event == "add-reaction") {
    handleAddReaction(conn, data);
  } else if (event == "remove_reaction" || event == "remove-reaction") {
    handleRemoveReaction(conn, data);
  } else if (event == "typing_start" || event == "typing-start" ||
             event == "typing") {
    handleTypingStart(conn, data);
  } else if (event == "typing_stop" || event == "typing-stop" ||
             event == "stop_typing" || event == "stop-typing") {
    handleTypingStop(conn, data);
  } else if (event == "delete_message" || event == "delete-message") {
    handleDeleteMessage(conn, data, ackId);
  } else if (event == "call_invite" || event == "call-invite") {
    handleCallInvite(conn, data, ackId);
  } else if (event == "call_accept" || event == "call-accept") {
    handleCallAccept(conn, data);
  } else if (event == "call_reject" || event == "call-reject" ||
             event == "call_decline" || event == "call-decline") {
    handleCallReject(conn, data);
  } else if (event == "call_cancel" || event == "call-cancel") {
    handleCallCancel(conn, data);
  } else if (event == "call_busy" || event == "call-busy") {
    handleCallBusy(conn, data);
  } else if (event == "call_end" || event == "call-end" ||
             event == "call_hangup" || event == "call-hangup") {
    handleCallEnd(conn, data);
  } else if (event == "user_online" || event == "user-online") {
    handleUserOnline(conn);
  } else if (event == "join-room") {
    // server.js: only well-known public room patterns (location_/user_self).
    if (data.isString()) {
      const std::string room = data.asString();
      const std::string self = "user_" + st->userId;
      bool ok = room == self;
      if (!ok) {
        // /^location_-?\d+_-?\d+$/
        static const std::string pfx = "location_";
        if (room.rfind(pfx, 0) == 0) {
          const std::string rest = room.substr(pfx.size());
          // crude validation of  -?\d+_-?\d+
          size_t us = rest.find('_');
          auto isInt = [](const std::string& s) {
            if (s.empty()) return false;
            size_t i = (s[0] == '-') ? 1 : 0;
            if (i >= s.size()) return false;
            for (; i < s.size(); ++i) if (!std::isdigit((unsigned char)s[i])) return false;
            return true;
          };
          if (us != std::string::npos &&
              isInt(rest.substr(0, us)) && isInt(rest.substr(us + 1)))
            ok = true;
        }
      }
      if (ok) joinRoom(conn, room);
    }
  } else if (event == "leave-room") {
    if (data.isString()) leaveRoom(conn, data.asString());
  } else if (event == "join-location") {
    // server.js: locationRoom = `location_${round(lat*1000)}_${round(lng*1000)}`
    // Throttle + validate the coordinate range so a client can't loop over
    // millions of distinct rooms to exhaust server memory.
    if (!st->limiter.allow("presence", nowMs())) {
      if (st->limiter.consumeWarn()) { Json::Value rl; rl["category"] = "presence"; emitTo(conn, "rate_limited", rl); }
      return;
    }
    if (data.isObject() && data.isMember("lat") && data.isMember("lng") &&
        data["lat"].isNumeric() && data["lng"].isNumeric()) {
      double latD = data["lat"].asDouble();
      double lngD = data["lng"].asDouble();
      if (latD >= -90.0 && latD <= 90.0 && lngD >= -180.0 && lngD <= 180.0) {
        long lat = std::lround(latD * 1000.0);
        long lng = std::lround(lngD * 1000.0);
        joinRoom(conn, "location_" + std::to_string(lat) + "_" + std::to_string(lng));
      }
    }
  }
  // Unknown events are ignored, like Socket.IO with no matching listener.
}

void RealtimeController::handleConnectionClosed(
    const WebSocketConnectionPtr& conn) {
  auto st = state(conn);
  // A connection rejected at the per-user cap (or pre-auth) has no ConnState:
  // it never INCR'd presence or userConnCount_ and was never put in conns_, so
  // skip all teardown to avoid a spurious decrement / offline broadcast.
  if (!st) return;
  if (st->closed.exchange(true)) return;

  // socket.on('disconnect') in realtime.js.
  handleDisconnect(conn);
  leaveAllRooms(conn);
  {
    std::lock_guard<std::mutex> lk(connMu_);
    conns_.erase(st->id);
    auto it = userConnCount_.find(st->userId);
    if (it != userConnCount_.end() && --it->second <= 0) userConnCount_.erase(it);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Room registry + emit (Socket.IO rooms / io.to / socket.to analogues)
// ─────────────────────────────────────────────────────────────────────────────
void RealtimeController::joinRoom(const WebSocketConnectionPtr& conn,
                                  const std::string& room) {
  auto st = state(conn);
  if (st) {
    // Cap rooms per connection so a client can't grow rooms_ / st->rooms without
    // bound. The user_<id> room and already-joined rooms are always allowed.
    std::lock_guard<std::mutex> lk(st->mu);
    if (st->rooms.find(room) == st->rooms.end() &&
        st->rooms.size() >= kMaxRoomsPerConn &&
        room != ("user_" + st->userId)) {
      return; // at cap — ignore the join
    }
    st->rooms.insert(room);
  }
  {
    std::lock_guard<std::mutex> lk(roomsMu_);
    rooms_[room].insert(conn);
  }
}

void RealtimeController::leaveRoom(const WebSocketConnectionPtr& conn,
                                   const std::string& room) {
  {
    std::lock_guard<std::mutex> lk(roomsMu_);
    auto it = rooms_.find(room);
    if (it != rooms_.end()) {
      it->second.erase(conn);
      if (it->second.empty()) rooms_.erase(it);
    }
  }
  auto st = state(conn);
  if (st) {
    std::lock_guard<std::mutex> lk(st->mu);
    st->rooms.erase(room);
  }
}

void RealtimeController::leaveAllRooms(const WebSocketConnectionPtr& conn) {
  std::set<std::string> rooms;
  if (auto st = state(conn)) {
    std::lock_guard<std::mutex> lk(st->mu);
    rooms = st->rooms;
  }
  for (const auto& r : rooms) leaveRoom(conn, r);
}

void RealtimeController::deliverLocal(const std::string& room,
                                      const std::string& frame,
                                      const WebSocketConnectionPtr& exclude) {
  std::vector<WebSocketConnectionPtr> targets;
  {
    std::lock_guard<std::mutex> lk(roomsMu_);
    auto it = rooms_.find(room);
    if (it == rooms_.end()) return;
    targets.reserve(it->second.size());
    for (const auto& c : it->second) {
      if (exclude && c == exclude) continue;
      targets.push_back(c);
    }
  }
  for (const auto& c : targets) {
    if (c && c->connected()) c->send(frame);
  }
}

void RealtimeController::emitTo(const WebSocketConnectionPtr& conn,
                                const std::string& event,
                                const Json::Value& payload) {
  Json::Value frame(Json::objectValue);
  frame["event"] = event;
  frame["data"] = payload;
  if (conn && conn->connected()) conn->send(toCompactJson(frame));
}

void RealtimeController::ack(const WebSocketConnectionPtr& conn,
                             std::int64_t ackId, const Json::Value& payload) {
  if (ackId < 0) return; // no callback supplied by the client
  Json::Value frame(Json::objectValue);
  frame["event"] = "ack";
  frame["ack"] = static_cast<Json::Int64>(ackId);
  frame["data"] = payload;
  if (conn && conn->connected()) conn->send(toCompactJson(frame));
}

// io.to(room).emit(event, payload) for each alias — includes the sender.
void RealtimeController::emitRoom(const std::string& room,
                                  const std::vector<std::string>& events,
                                  const Json::Value& payload) {
  for (const auto& event : events) {
    Json::Value frame(Json::objectValue);
    frame["event"] = event;
    frame["data"] = payload;
    const std::string wire = toCompactJson(frame);
    deliverLocal(room, wire, nullptr);
    publishRoom(room, wire, /*excludeId=*/"");
  }
}

// socket.to(room).emit(event, payload) for each alias — excludes the sender.
void RealtimeController::emitOthers(const WebSocketConnectionPtr& self,
                                    const std::string& room,
                                    const std::vector<std::string>& events,
                                    const Json::Value& payload) {
  const std::string selfId = self ? connId(self) : "";
  for (const auto& event : events) {
    Json::Value frame(Json::objectValue);
    frame["event"] = event;
    frame["data"] = payload;
    const std::string wire = toCompactJson(frame);
    deliverLocal(room, wire, self);
    publishRoom(room, wire, selfId);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Redis pub/sub (the Socket.IO Redis adapter analogue).
//  Channel  socketio:room:<room>  carries  { frame, exclude } JSON envelopes.
// ─────────────────────────────────────────────────────────────────────────────
void RealtimeController::publishRoom(const std::string& room,
                                     const std::string& frame,
                                     const std::string& excludeId) {
  if (!pub_) return;
  Json::Value env(Json::objectValue);
  env["origin"] = instanceId_;       // so we can drop our own echo
  env["frame"] = frame;              // already-serialized event frame
  env["exclude"] = excludeId;        // connId to skip on the receiving side
  try {
    pub_->publish("socketio:room:" + room, toCompactJson(env));
  } catch (const std::exception& e) {
    pulse::log::error("Realtime: publishRoom failed: {}", e.what());
  }
}

void RealtimeController::onRedisRoomMessage(const std::string& channel,
                                            const std::string& msg) {
  // channel = socketio:room:<room>
  static const std::string pfx = "socketio:room:";
  if (channel.rfind(pfx, 0) != 0) return;
  const std::string room = channel.substr(pfx.size());

  Json::Value env;
  if (!parseJson(msg, env) || !env.isObject()) return;
  // Skip frames this instance published (already delivered locally).
  if (str(env, "origin") == instanceId_) return;

  const std::string frame = str(env, "frame");
  const std::string excludeId = str(env, "exclude");
  if (frame.empty()) return;

  // Deliver to local members of the room, skipping the excluded connection if it
  // happens to live on THIS instance (it normally lives on the origin instance).
  WebSocketConnectionPtr exclude;
  if (!excludeId.empty()) {
    std::lock_guard<std::mutex> lk(connMu_);
    auto it = conns_.find(excludeId);
    if (it != conns_.end()) exclude = it->second;
  }
  deliverLocal(room, frame, exclude);
}

void RealtimeController::startSubscriber() {
  if (subRunning_.exchange(true)) return;
  subThread_ = std::thread([this]() {
    while (subRunning_) {
      try {
        sub_ = pulse::cache().createClient();
        auto subscriber = sub_->subscriber();
        // Pattern hits arrive through on_pmessage (channel + payload). One
        // pattern matches every room channel; rooms are created dynamically so a
        // pattern subscription avoids re-subscribing per room.
        subscriber.on_pmessage(
            [this](std::string /*pattern*/, std::string channel,
                   std::string msg) { onRedisRoomMessage(channel, msg); });
        subscriber.psubscribe("socketio:room:*");
        while (subRunning_) {
          subscriber.consume();
        }
      } catch (const sw::redis::TimeoutError&) {
        // Subscriber reads are intentionally bounded so shutdown can join.
        // An idle channel timing out is normal; reconnect without log spam.
        if (!subRunning_) break;
      } catch (const std::exception& e) {
        if (!subRunning_) break;
        pulse::log::error("Realtime: subscriber loop error: {} — retrying", e.what());
        for (int i = 0; i < 10 && subRunning_; ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
  });
}

void RealtimeController::stopSubscriber() {
  if (!subRunning_.exchange(false)) return;
  try {
    if (pub_) {
      Json::Value wake(Json::objectValue);
      wake["origin"] = instanceId_;
      wake["frame"] = "";
      pub_->publish("socketio:room:__shutdown", toCompactJson(wake));
    }
  } catch (...) {
  }
  if (subThread_.joinable()) {
    subThread_.join();
  }
  sub_.reset();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Authorization guard + sender profile (realtime.js helpers)
// ─────────────────────────────────────────────────────────────────────────────

// getAuthorizedConversation — conversation IFF the user is a participant. Returns
// the full conversation doc (Json) or null. (Mirrors Conversation.findOne({ _id,
// participants: userId }).)
Json::Value RealtimeController::getAuthorizedConversation(
    const std::string& userId, const std::string& conversationId) {
  if (conversationId.empty() || !isValidOid(conversationId))
    return Json::Value(Json::nullValue);
  try {
    auto col = pulse::db::collection(pulse::models::conversation::kCollection);
    auto doc = col.find_one(make_document(
        kvp("_id", pulse::bsonjson::oid(conversationId)),
        kvp("participants", pulse::bsonjson::oid(userId))));
    if (!doc) return Json::Value(Json::nullValue);
    return pulse::bsonjson::toJson(doc->view());
  } catch (const std::exception& e) {
    pulse::log::error("getAuthorizedConversation error: {}", e.what());
    return Json::Value(Json::nullValue);
  }
}

// resolveSenderProfile(socket) — cached profile (socket-local + Redis 10m).
Json::Value RealtimeController::resolveSenderProfile(
    const WebSocketConnectionPtr& conn) {
  auto st = state(conn);
  if (!st) return Json::Value(Json::objectValue);
  {
    std::lock_guard<std::mutex> lk(st->mu);
    if (st->hasSenderProfile) return st->senderProfile;
  }

  const std::string userId = st->userId;
  const std::string cacheKey = "chat_sender:" + userId;

  Json::Value profile(Json::nullValue);
  if (auto cached = pulse::cache().get(cacheKey); cached && !cached->empty()) {
    Json::Value parsed;
    if (parseJson(*cached, parsed)) profile = parsed;
  }

  if (profile.isNull()) {
    Json::Value u(Json::nullValue);
    try {
      auto col = pulse::db::collection(pulse::models::user::kCollection);
      mongocxx::options::find opts;
      opts.projection(make_document(kvp("username", 1), kvp("name", 1),
                                    kvp("avatar", 1), kvp("profile.avatar", 1),
                                    kvp("isVerified", 1)));
      auto doc = col.find_one(
          make_document(kvp("_id", pulse::bsonjson::oid(userId))), opts);
      if (doc) u = pulse::bsonjson::toJson(doc->view());
    } catch (...) {
      u = Json::Value(Json::nullValue);
    }

    if (!u.isNull() && u.isObject()) {
      // avatar: u.avatar || u.profile?.avatar || null
      Json::Value avatar(Json::nullValue);
      if (u.isMember("avatar") && u["avatar"].isString() &&
          !u["avatar"].asString().empty())
        avatar = u["avatar"];
      else if (u.isMember("profile") && u["profile"].isObject() &&
               u["profile"].isMember("avatar"))
        avatar = u["profile"]["avatar"];

      // profile.avatar: u.profile?.avatar || u.avatar || null
      Json::Value profAvatar(Json::nullValue);
      if (u.isMember("profile") && u["profile"].isObject() &&
          u["profile"].isMember("avatar") && u["profile"]["avatar"].isString() &&
          !u["profile"]["avatar"].asString().empty())
        profAvatar = u["profile"]["avatar"];
      else if (u.isMember("avatar"))
        profAvatar = u["avatar"];

      profile = Json::Value(Json::objectValue);
      profile["_id"] = userId;
      profile["username"] = u.get("username", Json::Value());
      profile["name"] = u.get("name", Json::Value());
      profile["avatar"] = avatar;
      profile["profile"] = Json::Value(Json::objectValue);
      profile["profile"]["avatar"] = profAvatar;
      profile["isVerified"] = u.get("isVerified", Json::Value());
    } else {
      profile = Json::Value(Json::objectValue);
      profile["_id"] = userId;  // { _id: userId }
    }
    // cacheService.set(cacheKey, profile, 600) — fire and forget.
    pulse::cache().set(cacheKey, toCompactJson(profile), 600);
  }

  {
    std::lock_guard<std::mutex> lk(st->mu);
    st->senderProfile = profile;
    st->hasSenderProfile = true;
  }
  return profile;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Event handlers (1:1 with realtime.js)
// ─────────────────────────────────────────────────────────────────────────────

// 1. Join Chat Room (SECURED) — onAny(['join_conversation','join-conversation'])
void RealtimeController::handleJoin(const WebSocketConnectionPtr& conn,
                                    const Json::Value& data) {
  auto st = state(conn);
  if (!st) return;
  try {
    const std::string conversationId = conversationIdFrom(data);
    if (conversationId.empty()) return;
    // realtime.js handleJoin does NOT pre-validate the id: an invalid ObjectId
    // makes Conversation.findOne throw a CastError that the catch logs (no emit).
    if (!isValidOid(conversationId))
      throw std::runtime_error("Cast to ObjectId failed for join_conversation");

    auto col = pulse::db::collection(pulse::models::conversation::kCollection);
    mongocxx::options::find opts;
    opts.projection(make_document(kvp("_id", 1)));  // .select('_id')
    auto doc = col.find_one(
        make_document(kvp("_id", pulse::bsonjson::oid(conversationId)),
                      kvp("participants", pulse::bsonjson::oid(st->userId))),
        opts);

    if (doc) {
      joinRoom(conn, conversationId);
    } else {
      Json::Value e(Json::objectValue);
      e["message"] = "Unauthorized access to conversation";
      emitTo(conn, "error", e);
    }
  } catch (const std::exception& e) {
    pulse::log::error("\xE2\x9D\x8C Join room error: {}", e.what());
  }
}

// 2. Leave Chat Room — onAny(['leave_conversation','leave-conversation'])
void RealtimeController::handleLeave(const WebSocketConnectionPtr& conn,
                                     const Json::Value& data) {
  try {
    const std::string conversationId = conversationIdFrom(data);
    if (conversationId.empty()) return;
    leaveRoom(conn, conversationId);
  } catch (const std::exception& e) {
    pulse::log::error("\xE2\x9D\x8C Leave room error: {}", e.what());
  }
}

// 3. Send Message (WITH REPLY SUPPORT) — onAny(['send_message','send-message'])
void RealtimeController::handleSendMessage(const WebSocketConnectionPtr& conn,
                                           const Json::Value& data,
                                           std::int64_t ackId) {
  auto st = state(conn);
  if (!st) return;

  if (!st->limiter.allow("message", nowMs())) {
    if (st->limiter.consumeWarn()) {
      Json::Value rl; rl["category"] = "message"; emitTo(conn, "rate_limited", rl);
    }
    Json::Value cb; cb["status"] = "error"; cb["message"] = "Rate limited";
    ack(conn, ackId, cb);
    return;
  }

  try {
    const Json::Value d = data.isObject() ? data : Json::Value(Json::objectValue);
    const std::string conversationId = str(d, "conversationId");
    const std::string content = str(d, "content");
    const std::string type = (d.isMember("type") && d["type"].isString() &&
                              !d["type"].asString().empty())
                                 ? d["type"].asString()
                                 : "text";  // type = 'text'
    const Json::Value mediaIn = d.isMember("media") ? d["media"] : Json::Value();
    const std::string replyTo = str(d, "replyTo");

    // A0a. Reject oversized content BEFORE the DB write + room broadcast.
    // Drogon caps the WS frame at 1MB, but that is far larger than any real chat
    // message; without this a client could persist ~1MB docs and amplify them to
    // every room member. Media travels through a separate upload path.
    if (content.size() > kMaxMessageContentLen) {
      Json::Value cb; cb["status"] = "error";
      cb["message"] = "Message too long";
      ack(conn, ackId, cb);
      return;
    }

    // A0. Authorization — sender must be a participant.
    Json::Value conversation = getAuthorizedConversation(st->userId, conversationId);
    if (conversation.isNull()) {
      Json::Value cb; cb["status"] = "error";
      cb["message"] = "Unauthorized access to conversation";
      ack(conn, ackId, cb);
      return;
    }

    // A reply target must belong to the same conversation. Without this check a
    // participant could attach (and broadcast a preview of) a message from a
    // different conversation whose ObjectId they learned elsewhere.
    if (!replyTo.empty()) {
      if (!isValidOid(replyTo)) {
        Json::Value cb; cb["status"] = "error";
        cb["message"] = "Invalid reply target";
        ack(conn, ackId, cb);
        return;
      }
      try {
        auto messages = pulse::db::collection(pulse::models::message::kCollection);
        auto reply = messages.find_one(make_document(
            kvp("_id", pulse::bsonjson::oid(replyTo)),
            kvp("conversation", pulse::bsonjson::oid(conversationId))));
        if (!reply) {
          Json::Value cb; cb["status"] = "error";
          cb["message"] = "Reply target is not in this conversation";
          ack(conn, ackId, cb);
          return;
        }
      } catch (...) {
        Json::Value cb; cb["status"] = "error";
        cb["message"] = "Unable to validate reply target";
        ack(conn, ackId, cb);
        return;
      }
    }

    // A. Create the message (ONE write). Mirror Message.create(...) defaults.
    Json::Value mdoc(Json::objectValue);
    mdoc["conversation"] = conversationId;
    mdoc["sender"] = st->userId;
    mdoc["content"] = content;
    mdoc["type"] = type;
    // media: media || undefined — only set when present (truthy object). We cast
    // it to ONLY the schema fields {url,thumbnail,width,height,mimeType} so the
    // broadcast reflects newMessage.media (the stored subdoc), not raw client keys.
    bool hasMedia = mediaIn.isObject() && !mediaIn.empty();
    Json::Value mediaClean(Json::objectValue);
    if (hasMedia) {
      if (mediaIn.isMember("url"))       mediaClean["url"] = mediaIn["url"];
      if (mediaIn.isMember("thumbnail")) mediaClean["thumbnail"] = mediaIn["thumbnail"];
      if (mediaIn.isMember("width") && mediaIn["width"].isNumeric())   mediaClean["width"] = mediaIn["width"];
      if (mediaIn.isMember("height") && mediaIn["height"].isNumeric()) mediaClean["height"] = mediaIn["height"];
      if (mediaIn.isMember("mimeType")) mediaClean["mimeType"] = mediaIn["mimeType"];
    }
    // replyTo: replyTo || undefined — only set when present.
    bool hasReplyTo = !replyTo.empty();
    Json::Value mdocDefaults = pulse::models::message::applyDefaults(mdoc);

    bsoncxx::oid newId;
    const std::string createdAt = mdocDefaults["createdAt"].asString();
    const std::string updatedAt = mdocDefaults["updatedAt"].asString();
    {
      bld::document insert;
      insert.append(kvp("conversation", pulse::bsonjson::oid(conversationId)));
      insert.append(kvp("sender", pulse::bsonjson::oid(st->userId)));
      insert.append(kvp("content", content));
      insert.append(kvp("type", type));
      if (hasMedia) {
        // media subdocument: { url, thumbnail, width, height, mimeType }.
        bld::document m;
        if (mediaClean.isMember("url"))       m.append(kvp("url", mediaClean["url"].asString()));
        if (mediaClean.isMember("thumbnail")) m.append(kvp("thumbnail", mediaClean["thumbnail"].asString()));
        if (mediaClean.isMember("width"))     m.append(kvp("width", mediaClean["width"].asDouble()));
        if (mediaClean.isMember("height"))    m.append(kvp("height", mediaClean["height"].asDouble()));
        if (mediaClean.isMember("mimeType"))  m.append(kvp("mimeType", mediaClean["mimeType"].asString()));
        insert.append(kvp("media", m.extract()));
      }
      if (hasReplyTo && isValidOid(replyTo))
        insert.append(kvp("replyTo", pulse::bsonjson::oid(replyTo)));
      insert.append(kvp("reactions", make_document()));  // default {}
      insert.append(kvp("readBy", make_array()));        // [] (schema array)
      insert.append(kvp("isDeleted", false));            // default false
      insert.append(kvp("createdAt", nowDate()));
      insert.append(kvp("updatedAt", nowDate()));

      auto col = pulse::db::collection(pulse::models::message::kCollection);
      auto res = col.insert_one(insert.extract());
      if (res && res->inserted_id().type() == bsoncxx::type::k_oid)
        newId = res->inserted_id().get_oid().value;
    }
    const std::string newMessageId = pulse::bsonjson::oidToHex(newId);

    // B. Build the broadcast payload IN PROCESS.
    Json::Value sender = resolveSenderProfile(conn);
    Json::Value payload(Json::objectValue);
    payload["_id"] = newMessageId;
    payload["conversation"] = conversationId;
    payload["sender"] = sender;
    payload["content"] = content;          // newMessage.content
    payload["type"] = type;                // newMessage.type
    if (hasMedia) payload["media"] = mediaClean;  // newMessage.media (undefined when absent)
    // replyTo: undefined initially (omitted), set below only on a real reply.
    payload["reactions"] = Json::Value(Json::objectValue);  // {}
    payload["readBy"] = Json::Value(Json::arrayValue);      // []
    payload["isDeleted"] = false;
    payload["createdAt"] = createdAt;
    payload["updatedAt"] = updatedAt;

    if (hasReplyTo && isValidOid(replyTo)) {
      // Message.findById(replyTo).select('content sender type media')
      //   .populate('sender', 'username name avatar profile.avatar').lean()
      // findById returns null when the message is gone -> payload.replyTo = null.
      payload["replyTo"] = Json::Value(Json::nullValue);
      try {
        auto mcol = pulse::db::collection(pulse::models::message::kCollection);
        mongocxx::options::find ropts;
        ropts.projection(make_document(kvp("content", 1), kvp("sender", 1),
                                       kvp("type", 1), kvp("media", 1)));
        auto rdoc = mcol.find_one(
            make_document(kvp("_id", pulse::bsonjson::oid(replyTo)),
                          kvp("conversation",
                              pulse::bsonjson::oid(conversationId))), ropts);
        if (rdoc) {
          Json::Value reply = pulse::bsonjson::toJson(rdoc->view());
          // populate('sender', 'username name avatar profile.avatar')
          if (reply.isMember("sender") && reply["sender"].isString()) {
            const std::string sid = reply["sender"].asString();
            try {
              auto ucol = pulse::db::collection(pulse::models::user::kCollection);
              mongocxx::options::find uopts;
              uopts.projection(make_document(kvp("username", 1), kvp("name", 1),
                                             kvp("avatar", 1),
                                             kvp("profile.avatar", 1)));
              auto udoc = ucol.find_one(
                  make_document(kvp("_id", pulse::bsonjson::oid(sid))), uopts);
              if (udoc) reply["sender"] = pulse::bsonjson::toJson(udoc->view());
            } catch (...) { /* leave sender as the raw id */ }
          }
          payload["replyTo"] = reply;
        }
      } catch (...) { /* reply lookup best-effort */ }
    }

    // C. Determine Preview Text (exact emoji literals from realtime.js).
    std::string previewText = content;
    if (type == "image")        previewText = "\xF0\x9F\x93\xB7 Photo";   // 📷 Photo
    else if (type == "gif")     previewText = "\xF0\x9F\x8E\xAC GIF";     // 🎬 GIF
    else if (type == "sticker") previewText = "\xF0\x9F\x98\x8A Sticker"; // 😊 Sticker
    else if (type == "video")   previewText = "\xF0\x9F\x8E\xA5 Video";   // 🎥 Video

    // D. Unread counts for everyone except the sender.
    bld::document incUpdate;
    bool hasInc = false;
    if (conversation.isMember("participants") &&
        conversation["participants"].isArray()) {
      for (const auto& p : conversation["participants"]) {
        if (!p.isString()) continue;
        const std::string pid = p.asString();
        if (pid == st->userId) continue;  // String(id) !== String(socket.userId)
        incUpdate.append(kvp("unreadCounts." + pid, 1));
        hasInc = true;
      }
    }

    // E. Update the conversation summary (fire-and-forget).
    try {
      bld::document setDoc;
      setDoc.append(kvp("lastMessage", newId));
      setDoc.append(kvp("lastMessageContent", previewText));
      setDoc.append(kvp("lastMessageAt", nowDate()));
      setDoc.append(kvp("lastMessageSender", pulse::bsonjson::oid(st->userId)));
      bld::document update;
      update.append(kvp("$set", setDoc.extract()));
      if (hasInc) update.append(kvp("$inc", incUpdate.extract()));
      auto ccol = pulse::db::collection(pulse::models::conversation::kCollection);
      ccol.update_one(
          make_document(kvp("_id", pulse::bsonjson::oid(conversationId))),
          update.extract());
    } catch (const std::exception& e) {
      pulse::log::error("\xE2\x9D\x8C Conversation summary update failed: {}", e.what());
    }

    // F. Broadcast to the room (including sender), under both aliases.
    emitRoom(conversationId, {"new_message", "new-message"}, payload);

    // G. Acknowledge.
    Json::Value cb; cb["status"] = "ok"; cb["message"] = payload;
    ack(conn, ackId, cb);
  } catch (const std::exception& e) {
    pulse::log::error("\xE2\x9D\x8C Send message error: {}", e.what());
    Json::Value cb; cb["status"] = "error"; cb["message"] = "Failed to send message";
    ack(conn, ackId, cb);
  }
}

// 4. Mark Messages as Seen
//    onAny(['mark_seen','mark-seen','message_seen','message-seen'])
void RealtimeController::handleMarkSeen(const WebSocketConnectionPtr& conn,
                                        const Json::Value& data) {
  auto st = state(conn);
  if (!st) return;
  if (!st->limiter.allow("reaction", nowMs())) {
    if (st->limiter.consumeWarn()) { Json::Value rl; rl["category"] = "reaction"; emitTo(conn, "rate_limited", rl); }
    return;
  }
  try {
    const std::string conversationId = str(data, "conversationId");
    const std::string messageId = str(data, "messageId");
    if (conversationId.empty()) return;

    Json::Value conversation = getAuthorizedConversation(st->userId, conversationId);
    if (conversation.isNull()) return;

    auto ccol = pulse::db::collection(pulse::models::conversation::kCollection);
    ccol.update_one(
        make_document(kvp("_id", pulse::bsonjson::oid(conversationId))),
        make_document(kvp("$set",
            make_document(kvp("unreadCounts." + st->userId, 0)))));

    if (!messageId.empty() && isValidOid(messageId)) {
      auto mcol = pulse::db::collection(pulse::models::message::kCollection);
      mcol.update_one(
          make_document(kvp("_id", pulse::bsonjson::oid(messageId)),
                        kvp("conversation", pulse::bsonjson::oid(conversationId))),
          make_document(kvp("$addToSet",
              make_document(kvp("readBy", pulse::bsonjson::oid(st->userId))))));
    }

    Json::Value payload(Json::objectValue);
    payload["conversationId"] = conversationId;
    payload["userId"] = st->userId;
    payload["messageIds"] = Json::Value(Json::arrayValue);
    if (!messageId.empty()) payload["messageIds"].append(messageId);

    emitOthers(conn, conversationId,
               {"messages_seen", "messages-seen", "message-seen"}, payload);
  } catch (const std::exception& e) {
    pulse::log::error("\xE2\x9D\x8C Mark seen error: {}", e.what());
  }
}

// 5. Add Reaction to Message — onAny(['add_reaction','add-reaction'])
void RealtimeController::handleAddReaction(const WebSocketConnectionPtr& conn,
                                           const Json::Value& data) {
  auto st = state(conn);
  if (!st) return;
  if (!st->limiter.allow("reaction", nowMs())) {
    if (st->limiter.consumeWarn()) { Json::Value rl; rl["category"] = "reaction"; emitTo(conn, "rate_limited", rl); }
    return;
  }
  try {
    const std::string conversationId = str(data, "conversationId");
    const std::string messageId = str(data, "messageId");
    const std::string reaction = str(data, "reaction");
    if (conversationId.empty() || messageId.empty()) return;
    if (!isValidOid(messageId)) return;

    Json::Value conversation = getAuthorizedConversation(st->userId, conversationId);
    if (conversation.isNull()) return;

    auto mcol = pulse::db::collection(pulse::models::message::kCollection);
    auto updated = mcol.find_one_and_update(
        make_document(kvp("_id", pulse::bsonjson::oid(messageId)),
                      kvp("conversation", pulse::bsonjson::oid(conversationId))),
        make_document(kvp("$set",
            make_document(kvp("reactions." + st->userId, reaction)))));
    if (!updated) return;

    Json::Value payload(Json::objectValue);
    payload["messageId"] = messageId;
    payload["userId"] = st->userId;
    payload["reaction"] = reaction;
    emitRoom(conversationId, {"message_reaction", "message-reaction"}, payload);
  } catch (const std::exception& e) {
    pulse::log::error("\xE2\x9D\x8C Add reaction error: {}", e.what());
  }
}

// 6. Remove Reaction — onAny(['remove_reaction','remove-reaction'])
void RealtimeController::handleRemoveReaction(const WebSocketConnectionPtr& conn,
                                              const Json::Value& data) {
  auto st = state(conn);
  if (!st) return;
  if (!st->limiter.allow("reaction", nowMs())) {
    if (st->limiter.consumeWarn()) { Json::Value rl; rl["category"] = "reaction"; emitTo(conn, "rate_limited", rl); }
    return;
  }
  try {
    const std::string conversationId = str(data, "conversationId");
    const std::string messageId = str(data, "messageId");
    if (conversationId.empty() || messageId.empty()) return;
    if (!isValidOid(messageId)) return;

    Json::Value conversation = getAuthorizedConversation(st->userId, conversationId);
    if (conversation.isNull()) return;

    auto mcol = pulse::db::collection(pulse::models::message::kCollection);
    auto updated = mcol.find_one_and_update(
        make_document(kvp("_id", pulse::bsonjson::oid(messageId)),
                      kvp("conversation", pulse::bsonjson::oid(conversationId))),
        make_document(kvp("$unset",
            make_document(kvp("reactions." + st->userId, "")))));
    if (!updated) return;

    Json::Value payload(Json::objectValue);
    payload["messageId"] = messageId;
    payload["userId"] = st->userId;
    payload["reaction"] = Json::Value(Json::nullValue);  // reaction: null
    emitRoom(conversationId, {"message_reaction", "message-reaction"}, payload);
  } catch (const std::exception& e) {
    pulse::log::error("\xE2\x9D\x8C Remove reaction error: {}", e.what());
  }
}

// 7. Typing Indicators — onAny(['typing_start','typing-start','typing']) /
//    onAny(['typing_stop','typing-stop','stop_typing','stop-typing'])
void RealtimeController::handleTypingStart(const WebSocketConnectionPtr& conn,
                                           const Json::Value& data) {
  auto st = state(conn);
  if (!st) return;
  if (!st->limiter.allow("typing", nowMs())) {
    if (st->limiter.consumeWarn()) { Json::Value rl; rl["category"] = "typing"; emitTo(conn, "rate_limited", rl); }
    return;
  }
  const std::string conversationId = conversationIdFrom(data);
  // socket.rooms.has(conversationId) — room membership implies authorization.
  bool inRoom = false;
  {
    std::lock_guard<std::mutex> lk(st->mu);
    inRoom = st->rooms.count(conversationId) > 0;
  }
  if (conversationId.empty() || !inRoom) return;
  Json::Value payload(Json::objectValue);
  payload["userId"] = st->userId;
  payload["isTyping"] = true;
  emitOthers(conn, conversationId, {"user_typing", "user-typing"}, payload);
}

void RealtimeController::handleTypingStop(const WebSocketConnectionPtr& conn,
                                          const Json::Value& data) {
  auto st = state(conn);
  if (!st) return;
  if (!st->limiter.allow("typing", nowMs())) {
    if (st->limiter.consumeWarn()) { Json::Value rl; rl["category"] = "typing"; emitTo(conn, "rate_limited", rl); }
    return;
  }
  const std::string conversationId = conversationIdFrom(data);
  bool inRoom = false;
  {
    std::lock_guard<std::mutex> lk(st->mu);
    inRoom = st->rooms.count(conversationId) > 0;
  }
  if (conversationId.empty() || !inRoom) return;
  Json::Value payload(Json::objectValue);
  payload["userId"] = st->userId;
  payload["isTyping"] = false;
  emitOthers(conn, conversationId, {"user_typing", "user-typing"}, payload);
}

// 8. Delete Message — onAny(['delete_message','delete-message'])
void RealtimeController::handleDeleteMessage(const WebSocketConnectionPtr& conn,
                                             const Json::Value& data,
                                             std::int64_t ackId) {
  auto st = state(conn);
  if (!st) return;
  if (!st->limiter.allow("message", nowMs())) {
    if (st->limiter.consumeWarn()) { Json::Value rl; rl["category"] = "message"; emitTo(conn, "rate_limited", rl); }
    Json::Value cb; cb["status"] = "error"; cb["message"] = "Rate limited";
    ack(conn, ackId, cb);
    return;
  }
  try {
    const std::string messageId = str(data, "messageId");
    if (messageId.empty() || !isValidOid(messageId)) {
      Json::Value cb; cb["status"] = "error"; cb["message"] = "Invalid message";
      ack(conn, ackId, cb);
      return;
    }

    // Only the sender can delete. findOneAndUpdate({_id, sender}, {...}, {new:false})
    // .select('conversation') — read back conversation for the broadcast.
    auto mcol = pulse::db::collection(pulse::models::message::kCollection);
    mongocxx::options::find_one_and_update fopts;
    fopts.projection(make_document(kvp("conversation", 1)));
    fopts.return_document(mongocxx::options::return_document::k_before); // new:false
    auto message = mcol.find_one_and_update(
        make_document(kvp("_id", pulse::bsonjson::oid(messageId)),
                      kvp("sender", pulse::bsonjson::oid(st->userId))),
        make_document(kvp("$set",
            make_document(kvp("isDeleted", true),
                          kvp("content", "This message was deleted")))),
        fopts);

    if (!message) {
      Json::Value cb; cb["status"] = "error";
      cb["message"] = "Not authorized or message not found";
      ack(conn, ackId, cb);
      return;
    }

    Json::Value mj = pulse::bsonjson::toJson(message->view());
    const std::string conversationId = str(mj, "conversation");
    Json::Value payload(Json::objectValue);
    payload["messageId"] = messageId;
    emitRoom(conversationId, {"message_deleted", "message-deleted"}, payload);

    Json::Value cb; cb["status"] = "ok";
    ack(conn, ackId, cb);
  } catch (const std::exception& e) {
    pulse::log::error("\xE2\x9D\x8C Delete message error: {}", e.what());
    Json::Value cb; cb["status"] = "error"; cb["message"] = "Failed to delete message";
    ack(conn, ackId, cb);
  }
}

// 9. User Goes Online — onAny(['user_online','user-online'])
void RealtimeController::handleUserOnline(const WebSocketConnectionPtr& conn) {
  auto st = state(conn);
  if (!st) return;
  if (!st->limiter.allow("presence", nowMs())) {
    if (st->limiter.consumeWarn()) { Json::Value rl; rl["category"] = "presence"; emitTo(conn, "rate_limited", rl); }
    return;
  }
  try {
    // The INCR now happens once per socket in handleNewConnection. An explicit
    // user_online event must NOT INCR again (that would double-count and the
    // user would never appear to "go offline"); it just refreshes the TTL.
    pulse::presence().touch(st->userId);
  } catch (const std::exception& e) {
    pulse::log::error("\xE2\x9D\x8C User online error: {}", e.what());
  }
}

// Helper: notify the user's conversation peers (scoped, capped fan-out).
void RealtimeController::notifyConversationPeers(
    const WebSocketConnectionPtr& conn, bool isOnline) {
  auto st = state(conn);
  if (!st) return;
  try {
    auto col = pulse::db::collection(pulse::models::conversation::kCollection);
    mongocxx::options::find opts;
    opts.projection(make_document(kvp("_id", 1)));  // .select('_id')
    opts.limit(500);                                 // .limit(500)
    auto cursor = col.find(
        make_document(kvp("participants", pulse::bsonjson::oid(st->userId))),
        opts);
    Json::Value payload(Json::objectValue);
    payload["userId"] = st->userId;
    payload["isOnline"] = isOnline;
    for (auto&& doc : cursor) {
      Json::Value conv = pulse::bsonjson::toJson(doc);
      const std::string convId = str(conv, "_id");
      if (convId.empty()) continue;
      emitOthers(conn, convId, {"user_status_change", "user-status-change"},
                 payload);
    }
  } catch (const std::exception& e) {
    pulse::log::error("notifyConversationPeers error: {}", e.what());
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Call signaling (voice/video) — control plane only; media is on LiveKit.
//
//  Frames are routed to the target user's `user_<id>` room, which every
//  authenticated socket joins at connect (handleNewConnection). That reuses the
//  existing local + Redis cross-instance fan-out, so a peer signed in on another
//  device/instance still gets the ring. The HTTP /calls/initiate path also sends
//  an FCM/Expo push for a peer whose app is backgrounded with no live socket.
//
//  Anti-spoof: `from` is always overwritten with the authenticated sender's id;
//  the client-supplied value is ignored. We do not authorize against the
//  conversation here (a call is a transient peer action, and the callee can
//  reject) — but we DO require a `to` user id and a `callId` to relay.
// ─────────────────────────────────────────────────────────────────────────────
bool RealtimeController::relayCallEvent(const WebSocketConnectionPtr& conn,
                                        const std::vector<std::string>& events,
                                        const Json::Value& data) {
  auto st = state(conn);
  if (!st) return false;

  if (!st->limiter.allow("presence", nowMs())) {
    if (st->limiter.consumeWarn()) {
      Json::Value limited; limited["category"] = "call";
      emitTo(conn, "rate_limited", limited);
    }
    return false;
  }

  const std::string to = str(data, "to");
  const std::string callId = str(data, "callId");
  const std::string conversationId = str(data, "conversationId");
  if (to.empty() || to == st->userId || !isValidOid(to) ||
      !isValidCallId(callId) || !isValidOid(conversationId)) return false;

  // The call must have been allocated by /calls/initiate and this exact sender
  // and recipient must be its two parties. This prevents arbitrary user-room
  // signaling and forged accept/end events.
  try {
    auto raw = pulse::cache().get("call:" + callId);
    Json::Value call;
    if (!raw || raw->empty() || !parseJson(*raw, call) ||
        str(call, "callId") != callId ||
        str(call, "conversationId") != conversationId) return false;
    const std::string caller = str(call, "caller");
    const std::string callee = str(call, "callee");
    const bool validDirection =
        (caller == st->userId && callee == to) ||
        (callee == st->userId && caller == to);
    if (!validDirection) return false;
  } catch (...) {
    return false;
  }

  Json::Value conversation =
      getAuthorizedConversation(st->userId, conversationId);
  if (conversation.isNull() || !hasParticipant(conversation, to)) return false;

  // Build the relayed payload: copy the client's fields but force `from` to the
  // authenticated sender (so the callee always sees who is really calling).
  Json::Value payload = data.isObject() ? data : Json::Value(Json::objectValue);
  payload["from"] = st->userId;

  // Deliver to every socket the recipient holds (their `user_<to>` room).
  emitRoom("user_" + to, events, payload);
  return true;
}

// call_invite — A rings B. Rate-limited on the "presence" bucket (calls are
// low-frequency; this stops a client spamming invites). Acked so the caller's
// UI can confirm the ring was relayed.
void RealtimeController::handleCallInvite(const WebSocketConnectionPtr& conn,
                                          const Json::Value& data,
                                          std::int64_t ackId) {
  auto st = state(conn);
  if (!st) return;
  const std::string to = str(data, "to");
  const std::string callId = str(data, "callId");
  if (to.empty() || !isValidOid(to) || callId.empty()) {
    Json::Value cb; cb["status"] = "error"; cb["message"] = "Invalid call invite";
    ack(conn, ackId, cb);
    return;
  }
  // incoming_call is what the callee listens for (matches the FCM data type).
  const bool relayed =
      relayCallEvent(conn, {"incoming_call", "incoming-call"}, data);
  Json::Value cb;
  cb["status"] = relayed ? "ok" : "error";
  if (!relayed) cb["message"] = "Call is not authorized or was rate limited";
  ack(conn, ackId, cb);
}

// call_accept — B accepted; tell A so it can join the LiveKit room.
void RealtimeController::handleCallAccept(const WebSocketConnectionPtr& conn,
                                          const Json::Value& data) {
  relayCallEvent(conn, {"call_accepted", "call-accepted"}, data);
}

// call_reject / call_decline — B declined.
void RealtimeController::handleCallReject(const WebSocketConnectionPtr& conn,
                                          const Json::Value& data) {
  relayCallEvent(conn, {"call_rejected", "call-rejected"}, data);
}

// call_cancel — A hung up before B answered (or the ring timed out on A's side).
void RealtimeController::handleCallCancel(const WebSocketConnectionPtr& conn,
                                          const Json::Value& data) {
  relayCallEvent(conn, {"call_cancelled", "call-cancelled"}, data);
}

// call_busy — B is already on another call.
void RealtimeController::handleCallBusy(const WebSocketConnectionPtr& conn,
                                        const Json::Value& data) {
  relayCallEvent(conn, {"call_busy", "call-busy"}, data);
}

// call_end — either side ended the connected call.
void RealtimeController::handleCallEnd(const WebSocketConnectionPtr& conn,
                                       const Json::Value& data) {
  relayCallEvent(conn, {"call_ended", "call-ended"}, data);
}

// 10. Disconnect — Redis DECR; only emit "offline" when the LAST socket goes.
void RealtimeController::handleDisconnect(const WebSocketConnectionPtr& conn) {
  auto st = state(conn);
  if (!st) return;
  if (!st->presenceRegistered.exchange(false)) return;
  try {
    bool wentOffline = pulse::presence().removeConnection(st->userId);
    if (wentOffline) notifyConversationPeers(conn, false);
  } catch (const std::exception& e) {
    pulse::log::error("\xE2\x9D\x8C Disconnect handler error: {}", e.what());
  }
}

} // namespace pulse::sockets
