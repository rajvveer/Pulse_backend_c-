// realtime_controller.hpp — C++ port of src/sockets/realtime.js + the Socket.IO
// wiring in src/server.js (handshake auth, user_<id> room, presence, chat).
//
// The Node service ran on Socket.IO with the Redis adapter for cross-instance
// fan-out. This port reproduces the SAME behavior on Drogon:
//
//   * A drogon::WebSocketController is the connection endpoint. The handshake is
//     authenticated with pulse::jwt().verifyAccessToken (token taken from the
//     `token` query param, falling back to the Authorization header) — the exact
//     same verification path server.js used for handshake.auth.token.
//   * "Rooms" (conversation rooms + the per-user `user_<id>` room) are tracked in
//     an in-process registry. io.to(room).emit(...) / socket.to(room).emit(...)
//     are reproduced by sending to every LOCAL connection in the room AND
//     publishing the frame on a Redis channel so OTHER instances deliver it to
//     their local room members too. This is the Socket.IO Redis-adapter analogue.
//   * Presence is Redis-backed (pulse::presence()), exactly as in realtime.js.
//
// ── Wire protocol ──
// Socket.IO multiplexes named events over one connection. On raw WebSocket we
// model each event as one JSON text frame:
//     { "event": "<name>", "data": <payload>, "ack": <number?> }
// The server answers a client-provided "ack" id (the Socket.IO callback) with:
//     { "event": "ack", "ack": <number>, "data": <ackPayload> }
// Server-initiated emits (new_message, user_typing, ...) carry no "ack". Event
// NAMES and PAYLOAD SHAPES are byte-for-byte identical to realtime.js — every
// hyphenated/underscored alias is registered on input and emitted on output, so
// an old APK and a new APK still interoperate.
//
// Redis pub/sub channels (cross-instance fan-out):
//   socketio:room:<room>   — a frame to deliver to every member of <room>,
//                            optionally excluding the originating connection.
#pragma once

#include <drogon/WebSocketController.h>
#include <json/json.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sw::redis { class Redis; }

namespace pulse::sockets {

// ── Per-connection token-bucket rate limiter (ports socketRateLimit.js) ──
// In-memory, per connection. Tokens refill continuously at ratePerSec, capped at
// burst. createSocketLimiter()'s guard(category) -> RealtimeLimiter::allow(cat).
class RealtimeLimiter {
public:
  // Returns false when the connection has exceeded its budget for `category`.
  // Mirrors guard(): emits a throttled "rate_limited" notice every 20th drop.
  bool allow(const std::string& category, std::int64_t nowMs);

  // True only when the caller should emit the throttle notice for this drop
  // (every 20th rejection). Populated by allow() via wasWarn().
  bool consumeWarn();

private:
  struct Bucket {
    double ratePerSec = 0;
    double burst = 0;
    double tokens = 0;
    std::int64_t last = 0;
  };
  std::unordered_map<std::string, Bucket> buckets_;
  std::int64_t warned_ = 0;
  bool warnNow_ = false;
};

// ── Per-connection state stored on the WebSocketConnection ──
struct ConnState {
  std::string id;                         // process-unique connection id (NOT the pointer)
  std::string userId;                     // socket.userId
  std::string accessToken;                 // revalidated while the socket lives
  Json::Value user;                       // socket.user (lightweight identity)
  std::set<std::string> rooms;            // socket.rooms (room names this conn is in)
  Json::Value senderProfile;              // socket._senderProfile (cached, lazy)
  bool hasSenderProfile = false;
  RealtimeLimiter limiter;                // createSocketLimiter(socket)
  // Pair Redis presence bookkeeping with a successful registration and make
  // teardown idempotent if a transport reports close more than once.
  std::atomic<bool> presenceRegistered{false};
  std::atomic<bool> closed{false};
  std::mutex mu;                          // guards rooms / senderProfile
};

// ── The WebSocket endpoint ──
class RealtimeController
    : public drogon::WebSocketController<RealtimeController> {
public:
  RealtimeController();
  ~RealtimeController() override;
  static void shutdownInfrastructure();

  void handleNewConnection(const drogon::HttpRequestPtr& req,
                           const drogon::WebSocketConnectionPtr& conn) override;

  void handleNewMessage(const drogon::WebSocketConnectionPtr& conn,
                        std::string&& message,
                        const drogon::WebSocketMessageType& type) override;

  void handleConnectionClosed(
      const drogon::WebSocketConnectionPtr& conn) override;

  // Socket.IO's default mount path is "/socket.io/"; we expose the realtime
  // endpoint at /ws (and /socket.io/ for legacy clients that hit the old path).
  WS_PATH_LIST_BEGIN
  WS_PATH_ADD("/ws");
  WS_PATH_ADD("/socket.io/");
  WS_PATH_LIST_END

private:
  // ===== Room registry (Socket.IO rooms, local to this instance) =====
  void joinRoom(const drogon::WebSocketConnectionPtr& conn,
                const std::string& room);
  void leaveRoom(const drogon::WebSocketConnectionPtr& conn,
                 const std::string& room);
  void leaveAllRooms(const drogon::WebSocketConnectionPtr& conn);

  // Deliver `frame` to every LOCAL connection in `room`. When `exclude` is set,
  // that connection is skipped (socket.to(room) excludes the sender).
  void deliverLocal(const std::string& room, const std::string& frame,
                    const drogon::WebSocketConnectionPtr& exclude);

  // io.to(room).emit  — deliver to the whole room (local + remote), sender too.
  void emitRoom(const std::string& room,
                const std::vector<std::string>& events,
                const Json::Value& payload);

  // socket.to(room).emit — deliver to the room EXCEPT the originating conn.
  void emitOthers(const drogon::WebSocketConnectionPtr& self,
                  const std::string& room,
                  const std::vector<std::string>& events,
                  const Json::Value& payload);

  // socket.emit — send a single event to one connection (no fan-out).
  void emitTo(const drogon::WebSocketConnectionPtr& conn,
              const std::string& event, const Json::Value& payload);

  // Acknowledge a Socket.IO callback (frame.ack) back to the originating conn.
  void ack(const drogon::WebSocketConnectionPtr& conn, std::int64_t ackId,
           const Json::Value& payload);

  // ===== Redis pub/sub (cross-instance fan-out) =====
  void startSubscriber();
  void stopSubscriber();
  void startAuthMonitor();
  void stopAuthMonitor();
  // Publish one room delivery to peer instances. `excludeId` is the originating
  // connection's stable id so the publishing instance does not double-deliver.
  void publishRoom(const std::string& room, const std::string& frame,
                   const std::string& excludeId);
  void onRedisRoomMessage(const std::string& channel, const std::string& msg);

  // ===== Event handlers (one per realtime.js handler) =====
  void handleJoin(const drogon::WebSocketConnectionPtr& conn,
                  const Json::Value& data);
  void handleLeave(const drogon::WebSocketConnectionPtr& conn,
                   const Json::Value& data);
  void handleSendMessage(const drogon::WebSocketConnectionPtr& conn,
                         const Json::Value& data, std::int64_t ackId);
  void handleMarkSeen(const drogon::WebSocketConnectionPtr& conn,
                      const Json::Value& data);
  void handleAddReaction(const drogon::WebSocketConnectionPtr& conn,
                         const Json::Value& data);
  void handleRemoveReaction(const drogon::WebSocketConnectionPtr& conn,
                            const Json::Value& data);
  void handleTypingStart(const drogon::WebSocketConnectionPtr& conn,
                         const Json::Value& data);
  void handleTypingStop(const drogon::WebSocketConnectionPtr& conn,
                        const Json::Value& data);
  void handleDeleteMessage(const drogon::WebSocketConnectionPtr& conn,
                           const Json::Value& data, std::int64_t ackId);
  void handleUserOnline(const drogon::WebSocketConnectionPtr& conn);
  void handleDisconnect(const drogon::WebSocketConnectionPtr& conn);

  // ===== Call signaling (voice/video) =====
  // Lightweight signaling for LiveKit-backed calls. Media never touches this
  // server — these events only carry ring/accept/reject/cancel/busy/end control
  // frames, routed to the peer's `user_<id>` room so any of their open sockets
  // (other devices/tabs) get them. The HTTP /calls/initiate path additionally
  // fires an FCM/Expo push for a backgrounded peer with no live socket.
  //
  // Each frame carries at least { callId, conversationId, from, to } plus
  // call-specific fields. We re-emit to "user_<to>" (server->peer) so the
  // existing room fan-out + Redis cross-instance delivery is reused verbatim.
  void handleCallInvite(const drogon::WebSocketConnectionPtr& conn,
                        const Json::Value& data, std::int64_t ackId);
  void handleCallAccept(const drogon::WebSocketConnectionPtr& conn,
                        const Json::Value& data);
  void handleCallReject(const drogon::WebSocketConnectionPtr& conn,
                        const Json::Value& data);
  void handleCallCancel(const drogon::WebSocketConnectionPtr& conn,
                        const Json::Value& data);
  void handleCallBusy(const drogon::WebSocketConnectionPtr& conn,
                      const Json::Value& data);
  void handleCallEnd(const drogon::WebSocketConnectionPtr& conn,
                     const Json::Value& data);
  // Common relay: forward a call control frame to the `to` user's room, stamping
  // `from` with the sender's id so the peer can't be spoofed. `extraEvents` lets
  // a single logical event ship under both underscore + hyphen aliases.
  bool relayCallEvent(const drogon::WebSocketConnectionPtr& conn,
                      const std::vector<std::string>& events,
                      const Json::Value& data);

  // Helpers
  // resolveSenderProfile(socket) — Redis (10m) + socket-local cached profile.
  Json::Value resolveSenderProfile(const drogon::WebSocketConnectionPtr& conn);
  // getAuthorizedConversation(conversationId) — conversation IFF user is a
  // participant, else a null Json::Value. Returns the loaded conversation doc.
  Json::Value getAuthorizedConversation(const std::string& userId,
                                        const std::string& conversationId);
  // notifyConversationPeers(isOnline) — scoped, capped peer fan-out.
  void notifyConversationPeers(const drogon::WebSocketConnectionPtr& conn,
                               bool isOnline);

  static std::shared_ptr<ConnState> state(
      const drogon::WebSocketConnectionPtr& conn);
  // A process-stable id for a connection (used to exclude the sender across the
  // pub/sub round-trip). We use the connection pointer value as a string.
  static std::string connId(const drogon::WebSocketConnectionPtr& conn);

  // ===== Registries =====
  std::mutex roomsMu_;
  // room -> set of local connections in that room.
  std::unordered_map<std::string,
                     std::set<drogon::WebSocketConnectionPtr>> rooms_;
  // connId -> connection, so a pub/sub frame can identify/exclude the sender.
  std::mutex connMu_;
  std::unordered_map<std::string, drogon::WebSocketConnectionPtr> conns_;
  // Live connection count per userId (under connMu_) for the per-user cap.
  std::unordered_map<std::string, int> userConnCount_;
  // Monotonic source for process-unique connection ids (never the pointer).
  std::atomic<std::uint64_t> connSeq_{0};

  // Redis pub/sub.
  std::shared_ptr<sw::redis::Redis> pub_;   // publisher client
  std::shared_ptr<sw::redis::Redis> sub_;   // subscriber client
  std::thread subThread_;
  std::atomic<bool> subRunning_{false};
  std::thread authThread_;
  std::atomic<bool> authRunning_{false};
  std::mutex authWaitMu_;
  std::condition_variable authWaitCv_;

  // This instance's id, embedded in published frames so we can drop our own.
  std::string instanceId_;
  static std::atomic<RealtimeController*> activeInstance_;
};

} // namespace pulse::sockets
