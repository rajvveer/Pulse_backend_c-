// socket_rate_limit.hpp — per-socket event rate limiting.
// Ports src/sockets/socketRateLimit.js.
//
// Socket.IO events bypass the HTTP rate limiter entirely, so a handful of
// abusive sockets could flood send_message / typing / reactions and drive
// event-loop + Mongo load for the whole worker. We use a simple in-memory token
// bucket PER SOCKET. In-memory is correct here (unlike HTTP limits) because a
// WebSocket connection lives entirely on one worker, so there is nothing to
// share across processes — and it costs one small object per connection, fine
// even at tens of thousands of sockets. The bucket map is discarded when the
// connection closes (it lives in the per-connection context).
//
// Drogon has no Socket.IO "rooms" primitive, so this controller also provides
// the realtime handshake scaffold the JS server.js + realtime.js relied on:
//   - authenticate the WS handshake with pulse::jwt().verifyAccessToken (token
//     from the `token` query param OR the Authorization: Bearer header),
//   - join a per-user room user_<id> (a process-local room registry),
//   - hold a Redis pub/sub client (pulse::cache().createClient()) per socket so
//     cross-worker broadcasts can be wired (Socket.IO redis-adapter analogue).
//
// Event names + payloads are preserved EXACTLY from the JS source — the only
// event this unit itself emits is `rate_limited` with payload { category }.
#pragma once

#include <drogon/WebSocketController.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace sw::redis { class Redis; }

namespace pulse::sockets {

// ── Limit table (verbatim from socketRateLimit.js LIMITS) ────────────────────
// Default ceilings per event class (generous for legit clients, low enough to
// stop a flood). Tunable without touching handler code.
struct RateLimitConfig {
  double ratePerSec;
  double burst;
};

// LIMITS keys: "message", "reaction", "typing", "presence", "default".
const RateLimitConfig& limitFor(const std::string& category);

// ── TokenBucket (ports the JS TokenBucket class) ─────────────────────────────
// Tokens refill continuously at `ratePerSec`, capped at `burst`.
class TokenBucket {
public:
  TokenBucket(double ratePerSec, double burst);

  // `nowMs` is epoch milliseconds (JS Date.now()). Returns true if a token was
  // available (and consumes one), false otherwise.
  bool tryRemove(int64_t nowMs);

private:
  double ratePerSec_;
  double burst_;
  double tokens_;
  int64_t lastMs_;
};

// ── Per-socket limiter (ports createSocketLimiter) ───────────────────────────
// One instance is bound to a single WebSocket connection. Call guard(category)
// at the top of each handler; it returns false (and the handler should bail)
// when the socket has exceeded its budget for that category. On the 1st and
// every 20th rejection it asks the caller to emit a `rate_limited` notice
// (returned via the out-param) so a flood is not amplified.
class SocketLimiter {
public:
  // Returns true if the event is allowed. When it returns false and
  // `emitRateLimited` is set true, the caller should emit
  //   `rate_limited` with payload { "category": <category> }
  // to the socket (matching the JS socket.emit('rate_limited', { category })).
  bool guard(const std::string& category, bool& emitRateLimited);

  // Convenience overload when the caller does not need the emit signal.
  bool guard(const std::string& category) {
    bool ignored = false;
    return guard(category, ignored);
  }

private:
  std::map<std::string, TokenBucket> buckets_;
  long long warned_ = 0;
};

// ── Per-connection context ───────────────────────────────────────────────────
// Stored on each WebSocketConnectionPtr (setContext). Holds the authenticated
// identity, the in-memory rate-limit buckets, and a dedicated Redis client for
// pub/sub (the Socket.IO redis-adapter analogue).
struct SocketContext {
  std::string userId;
  SocketLimiter limiter;
  std::shared_ptr<sw::redis::Redis> pubsub;  // pulse::cache().createClient()
};

// ── Room registry (Socket.IO rooms analogue) ─────────────────────────────────
// Socket.IO multiplexes broadcasts via named rooms; Drogon has no such concept,
// so we keep a process-local registry mapping room name -> the set of live
// connections in it. The realtime handlers (separate unit) target a room with
// emitToRoom / emitToRoomExcept, exactly mirroring io.to(room).emit and
// socket.to(room).emit. join/leave are membership operations on this registry.
class RoomRegistry {
public:
  static RoomRegistry& instance();

  void join(const std::string& room, const drogon::WebSocketConnectionPtr& conn);
  void leave(const std::string& room, const drogon::WebSocketConnectionPtr& conn);
  void leaveAll(const drogon::WebSocketConnectionPtr& conn);
  bool inRoom(const std::string& room, const drogon::WebSocketConnectionPtr& conn);

  // io.to(room).emit(event, payload) — sends to every member (incl. sender).
  void emitToRoom(const std::string& room, const std::string& event,
                  const std::string& payloadJson);

  // socket.to(room).emit(event, payload) — every member EXCEPT `except`.
  void emitToRoomExcept(const std::string& room, const std::string& event,
                        const std::string& payloadJson,
                        const drogon::WebSocketConnectionPtr& except);

private:
  RoomRegistry() = default;
  std::mutex mu_;
  // room -> connections.  We hold weak ownership via the shared_ptr the
  // framework owns; dead connections are pruned on access.
  std::unordered_map<std::string,
                     std::unordered_set<drogon::WebSocketConnectionPtr>> rooms_;
};

// Emit a single Socket.IO-style frame to one connection: ["<event>",<payload>].
// Used by both this controller (rate_limited) and the realtime handlers.
void emitEvent(const drogon::WebSocketConnectionPtr& conn, const std::string& event,
               const std::string& payloadJson);

// ── The controller ───────────────────────────────────────────────────────────
// Owns the WS endpoint, authenticates the handshake, wires the per-socket
// limiter + pub/sub, and joins the user's room user_<id>. Self-registers via
// WS_PATH_LIST. The chat event routing (send_message, typing, ...) lives in the
// realtime unit, which reads the SocketContext placed here.
class SocketRateLimitController
    : public drogon::WebSocketController<SocketRateLimitController> {
public:
  void handleNewConnection(const drogon::HttpRequestPtr& req,
                           const drogon::WebSocketConnectionPtr& conn) override;

  void handleNewMessage(const drogon::WebSocketConnectionPtr& conn,
                        std::string&& message,
                        const drogon::WebSocketMessageType& type) override;

  void handleConnectionClosed(
      const drogon::WebSocketConnectionPtr& conn) override;

  WS_PATH_LIST_BEGIN
  // NOTE: the primary realtime endpoint (chat + presence) lives at /ws in the
  // realtime controller, which embeds this limiter via SocketContext. This unit
  // is the standalone socketRateLimit.js port; it mounts its own endpoint on a
  // DISTINCT path so the two WebSocketControllers never contend for the same
  // route. A client connecting here is authenticated + rate-limited identically.
  WS_PATH_ADD("/ws/ratelimit");
  WS_PATH_LIST_END
};

} // namespace pulse::sockets
