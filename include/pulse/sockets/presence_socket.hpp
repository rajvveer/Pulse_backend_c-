// presence_socket.hpp — Drogon WebSocketController port of the Socket.IO
// presence logic that lived in src/sockets/realtime.js (handlers #9 user_online,
// #10 disconnect, and the notifyConversationPeers helper). The original
// src/sockets/presence.js was an empty placeholder; the online / last-seen
// broadcast behaviour it referenced is implemented in realtime.js and is what is
// ported here, scoped to presence only.
//
// Parity contract (preserved 1:1 from the JS source — DO NOT change):
//   * Auth: a valid JWT *access* token is required to open the socket. In the JS
//     source the token arrived via Socket.IO's `handshake.auth.token`; for the
//     raw WebSocket handshake here it is read from the `token` query parameter or
//     the `Authorization: Bearer <token>` header, then verified with
//     pulse::jwt().verifyAccessToken (same pinned alg/issuer/audience/type path
//     as REST). A connection without a valid access token is rejected.
//   * Each connection joins the per-user room `user_<userId>` (server.js:212).
//   * Incoming events (Socket.IO cross-version aliases, both kept):
//       "user_online" | "user-online"   -> mark online, notify peers if first tab
//   * Outgoing events (both aliases emitted, exact payload):
//       "user_status_change" | "user-status-change"  { userId, isOnline }
//   * Presence is Redis-backed via pulse::presence() (presenceService.js). A user
//     flips online only on their FIRST socket and offline only when their LAST
//     socket disconnects; peers are notified ONLY on an actual transition.
//   * Peer fan-out is scoped to the user's conversation rooms (participants
//     contains userId, limit 500) — never a global broadcast.
//
// Cross-worker delivery: Socket.IO used a Redis adapter so `io.to(room).emit`
// reached sockets on every worker/process. Drogon has no room adapter, so this
// controller maintains an in-process room registry AND fans out across processes
// over Redis pub/sub using a client from pulse::cache().createClient(). Each
// process subscribes to one channel and re-delivers published frames to its own
// local members of the target room.
//
// Wire format: a WebSocket text frame is a JSON object { "event": <name>,
// "data": <payload> }, mirroring Socket.IO's (event, payload) model so the event
// names and payload shapes above are preserved verbatim for clients.
#pragma once

#include <drogon/WebSocketController.h>
#include "pulse/sockets/socket_rate_limit.hpp"

#include <memory>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <json/json.h>

namespace sw::redis { class Redis; }

namespace pulse::sockets {

// Per-connection state stashed on the Drogon WebSocketConnection so the message
// and close handlers know who is connected and which rooms they are in.
struct PresenceContext {
    std::string userId;          // authenticated user (from the access token)
    std::string accessToken;     // revalidated for every frame
    std::unordered_set<std::string> rooms;  // rooms this connection has joined
    SocketLimiter frameLimiter;  // cheap pre-database general-frame limiter
    std::atomic<bool> presenceRegistered{false};
    std::atomic<bool> closed{false};
};

// Process-wide room registry + Redis pub/sub bridge. One instance for the whole
// process (the WebSocketController is also a Drogon singleton). Thread-safe: the
// registry is touched from Drogon's IO worker threads and the subscriber thread.
class PresenceHub {
public:
    static PresenceHub& instance();
    ~PresenceHub();
    void shutdownInfrastructure();
    static void shutdownExistingInfrastructure();

    // Connection lifecycle (local membership bookkeeping).
    bool registerConnection(const std::string& userId,
                            const drogon::WebSocketConnectionPtr& conn,
                            std::size_t maxPerUser);
    void addToRoom(const std::string& room,
                   const drogon::WebSocketConnectionPtr& conn);
    void removeFromRoom(const std::string& room,
                        const drogon::WebSocketConnectionPtr& conn);
    void dropConnection(const drogon::WebSocketConnectionPtr& conn);

    // Emit `data` under each event alias in `events` to every LOCAL connection in
    // `room`. When `except` is non-null that connection is skipped (Socket.IO's
    // `socket.to(room).emit` = "everyone in the room except me").
    void emitLocal(const std::string& room,
                   const std::vector<std::string>& events,
                   const Json::Value& data,
                   const drogon::WebSocketConnectionPtr& except = nullptr);

    // Publish a room emit to ALL processes (including this one) over Redis. The
    // subscriber callback calls emitLocal on each process. `exceptUserId` lets the
    // origin process skip the sender's own connections to mirror `socket.to(...)`.
    void emitRoom(const std::string& room,
                  const std::vector<std::string>& events,
                  const Json::Value& data,
                  const std::string& exceptUserId = "");

private:
    PresenceHub();
    void startSubscriber();
    void stopSubscriber();
    void startAuthMonitor();
    void stopAuthMonitor();
    void handlePublished(const std::string& payload);

    static constexpr const char* kChannel = "presence:bcast";

    std::mutex mtx_;
    // room -> set of local connections
    std::unordered_map<std::string,
                       std::unordered_set<drogon::WebSocketConnectionPtr>> rooms_;
    // The reverse map makes close accounting idempotent.
    std::unordered_map<std::string, std::size_t> userConnCount_;
    std::unordered_map<drogon::WebSocketConnectionPtr, std::string> connUsers_;

    std::shared_ptr<sw::redis::Redis> pub_;  // publisher client (createClient())
    std::shared_ptr<sw::redis::Redis> sub_;  // subscriber client (createClient())
    std::thread subThread_;
    std::atomic<bool> subRunning_{false};
    std::thread authThread_;
    std::atomic<bool> authRunning_{false};
    std::mutex authWaitMu_;
    std::condition_variable authWaitCv_;
    static std::atomic<PresenceHub*> activeHub_;
};

// The Drogon WebSocketController. Mounted at /ws/presence only.
//
// NOTE: the primary realtime endpoint (RealtimeController) owns "/socket.io/"
// and "/ws" and already handles presence (user_online / disconnect). This
// dedicated presence channel is offered at /ws/presence for clients that want
// presence-only; it must NOT also claim /socket.io or Drogon would reject the
// duplicate route registration at startup.
class PresenceSocket : public drogon::WebSocketController<PresenceSocket> {
public:
    void handleNewMessage(const drogon::WebSocketConnectionPtr& conn,
                          std::string&& message,
                          const drogon::WebSocketMessageType& type) override;

    void handleNewConnection(const drogon::HttpRequestPtr& req,
                             const drogon::WebSocketConnectionPtr& conn) override;

    void handleConnectionClosed(
        const drogon::WebSocketConnectionPtr& conn) override;

    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/ws/presence");
    WS_PATH_LIST_END

private:
    // handler #9 (user_online / user-online).
    void handleUserOnline(const drogon::WebSocketConnectionPtr& conn);

    // notifyConversationPeers(isOnline): scoped, capped fan-out to the user's
    // conversation rooms.
    void notifyConversationPeers(const std::string& userId, bool isOnline);
};

}  // namespace pulse::sockets
