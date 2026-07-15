// presence_socket.cc — implementation of the presence WebSocketController.
//
// Ports the presence-related Socket.IO logic from src/sockets/realtime.js
// (handler #9 user_online, handler #10 disconnect, notifyConversationPeers) and
// the handshake-auth wiring from src/server.js (io.use(...) JWT gate +
// io.on('connection') join `user_<id>`). See presence_socket.hpp for the parity
// contract. Event names + payloads are preserved EXACTLY.
#include "pulse/sockets/presence_socket.hpp"

#include "pulse/cache.hpp"
#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/jwt_service.hpp"
#include "pulse/logger.hpp"
#include "pulse/models/conversation.hpp"
#include "pulse/services/presence_service.hpp"
#include "pulse/filters/auth_filters.hpp"
#include "pulse/config.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <mongocxx/options/find.hpp>

#include <sw/redis++/redis++.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <thread>

using namespace drogon;

namespace pulse::sockets {

std::atomic<PresenceHub*> PresenceHub::activeHub_{nullptr};

namespace {

// Event-name aliases. Socket.IO clients exist in two vintages (underscored and
// hyphenated); the server speaks both so old and new apps interoperate. These
// MUST stay byte-for-byte identical to realtime.js.
const std::vector<std::string> kStatusChangeEvents = {"user_status_change",
                                                       "user-status-change"};

// Serialize one outgoing Socket.IO-style frame: { "event": name, "data": data }.
std::string frame(const std::string& event, const Json::Value& data) {
    Json::Value f(Json::objectValue);
    f["event"] = event;
    f["data"] = data;
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    return Json::writeString(w, f);
}

// Read the JWT from the `token` query param first, then `Authorization: Bearer`.
// Mirrors server.js handshake auth (which used handshake.auth.token) adapted to
// the raw WS handshake per the porting task.
std::string extractToken(const HttpRequestPtr& req) {
    std::string auth = req->getHeader("authorization");
    if (auth.empty()) auth = req->getHeader("Authorization");
    const std::string bearer = "Bearer ";
    if (auth.rfind(bearer, 0) == 0) return auth.substr(bearer.size());
    return req->getParameter("token");
}

std::size_t maxConnectionsPerUser() {
    static const auto value = static_cast<std::size_t>(std::clamp<int64_t>(
        pulse::config().envInt("WS_MAX_CONNECTIONS_PER_USER", 10), 1, 100));
    return value;
}

std::size_t maxInboundFrameBytes() {
    static const auto value = static_cast<std::size_t>(std::clamp<int64_t>(
        pulse::config().envInt("WS_MAX_FRAME_BYTES", 64 * 1024),
        1024, 1024 * 1024));
    return value;
}

}  // namespace

// ───────────────────────────── PresenceHub ──────────────────────────────────

PresenceHub& PresenceHub::instance() {
    static PresenceHub hub;
    return hub;
}

PresenceHub::PresenceHub() {
    activeHub_.store(this);
    try {
        pub_ = pulse::cache().createClient();
    } catch (const std::exception& e) {
        pulse::log::error("\xE2\x9D\x8C Presence pub client init failed: {}", e.what());
    }
    startSubscriber();
    startAuthMonitor();
}

PresenceHub::~PresenceHub() {
    shutdownInfrastructure();
    PresenceHub* expected = this;
    activeHub_.compare_exchange_strong(expected, nullptr);
}

void PresenceHub::shutdownExistingInfrastructure() {
    if (auto* hub = activeHub_.load()) hub->shutdownInfrastructure();
}

void PresenceHub::shutdownInfrastructure() {
    std::unordered_set<WebSocketConnectionPtr> sockets;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (const auto& entry : rooms_)
            sockets.insert(entry.second.begin(), entry.second.end());
    }
    for (const auto& socket : sockets) {
        if (socket && socket->connected())
            socket->shutdown(CloseCode::kEndpointGone, "Server shutting down");
    }
    stopAuthMonitor();
    stopSubscriber();
}

void PresenceHub::startSubscriber() {
    if (subRunning_.exchange(true)) return;
    subThread_ = std::thread([this]() {
        while (subRunning_) {
            try {
                auto sub = pulse::cache().createClient();
                sub_ = sub;
                auto subscriber = sub->subscriber();
                subscriber.on_message(
                    [this](std::string /*channel*/, std::string msg) {
                        handlePublished(msg);
                    });
                subscriber.subscribe(kChannel);
                while (subRunning_) subscriber.consume();
            } catch (const sw::redis::TimeoutError&) {
                if (!subRunning_) break;
            } catch (const std::exception& e) {
                if (!subRunning_) break;
                pulse::log::warn("Presence subscriber dropped: {} — reconnecting",
                                 e.what());
                for (int i = 0; i < 10 && subRunning_; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    });
}

void PresenceHub::stopSubscriber() {
    if (!subRunning_.exchange(false)) return;
    try {
        if (pub_) {
            Json::Value wake(Json::objectValue);
            wake["room"] = "__shutdown";
            wake["except"] = "";
            wake["events"] = Json::Value(Json::arrayValue);
            wake["data"] = Json::Value(Json::objectValue);
            Json::StreamWriterBuilder writer;
            writer["indentation"] = "";
            pub_->publish(kChannel, Json::writeString(writer, wake));
        }
    } catch (...) {
    }
    if (subThread_.joinable()) subThread_.join();
    sub_.reset();
}

void PresenceHub::startAuthMonitor() {
    if (authRunning_.exchange(true)) return;
    authThread_ = std::thread([this] {
        const auto interval = std::chrono::seconds(std::max<int64_t>(
            5, std::min<int64_t>(300,
                pulse::config().envInt("WS_AUTH_RECHECK_SEC", 30))));
        std::unique_lock<std::mutex> waitLock(authWaitMu_);
        while (authRunning_) {
            if (authWaitCv_.wait_for(
                    waitLock, interval,
                    [this] { return !authRunning_.load(); })) break;
            waitLock.unlock();

            std::unordered_set<WebSocketConnectionPtr> unique;
            {
                std::lock_guard<std::mutex> lock(mtx_);
                for (const auto& entry : rooms_)
                    unique.insert(entry.second.begin(), entry.second.end());
            }
            for (const auto& socket : unique) {
                auto ctx = socket ? socket->getContext<PresenceContext>() : nullptr;
                if (!ctx) continue;
                AccessClaims claims;
                if (pulse::filters::validateAccessToken(ctx->accessToken, claims) !=
                        pulse::filters::AccessTokenStatus::Valid ||
                    claims.userId != ctx->userId) {
                    socket->shutdown(CloseCode::kViolation,
                                     "Authentication expired or revoked");
                } else if (ctx->presenceRegistered.load()) {
                    pulse::presence().touch(ctx->userId);
                }
            }
            waitLock.lock();
        }
    });
}

void PresenceHub::stopAuthMonitor() {
    if (!authRunning_.exchange(false)) return;
    authWaitCv_.notify_all();
    if (authThread_.joinable()) authThread_.join();
}

bool PresenceHub::registerConnection(
    const std::string& userId, const WebSocketConnectionPtr& conn,
    std::size_t maxPerUser) {
    if (userId.empty() || !conn || maxPerUser == 0) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    if (connUsers_.find(conn) != connUsers_.end()) return true;
    std::size_t& count = userConnCount_[userId];
    if (count >= maxPerUser) {
        if (count == 0) userConnCount_.erase(userId);
        return false;
    }
    ++count;
    connUsers_.emplace(conn, userId);
    return true;
}

void PresenceHub::addToRoom(const std::string& room,
                            const WebSocketConnectionPtr& conn) {
    std::lock_guard<std::mutex> lk(mtx_);
    rooms_[room].insert(conn);
}

void PresenceHub::removeFromRoom(const std::string& room,
                                 const WebSocketConnectionPtr& conn) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = rooms_.find(room);
    if (it == rooms_.end()) return;
    it->second.erase(conn);
    if (it->second.empty()) rooms_.erase(it);
}

void PresenceHub::dropConnection(const WebSocketConnectionPtr& conn) {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto it = rooms_.begin(); it != rooms_.end();) {
        it->second.erase(conn);
        if (it->second.empty())
            it = rooms_.erase(it);
        else
            ++it;
    }
    auto userIt = connUsers_.find(conn);
    if (userIt == connUsers_.end()) return;
    auto countIt = userConnCount_.find(userIt->second);
    if (countIt != userConnCount_.end()) {
        if (countIt->second <= 1)
            userConnCount_.erase(countIt);
        else
            --countIt->second;
    }
    connUsers_.erase(userIt);
}

void PresenceHub::emitLocal(const std::string& room,
                            const std::vector<std::string>& events,
                            const Json::Value& data,
                            const WebSocketConnectionPtr& except) {
    // Snapshot the room members under the lock, then send outside it (sending
    // can block on the socket and must not hold the registry mutex).
    std::vector<WebSocketConnectionPtr> targets;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = rooms_.find(room);
        if (it == rooms_.end()) return;
        targets.reserve(it->second.size());
        for (const auto& c : it->second) {
            if (c == except) continue;
            if (c && c->connected()) targets.push_back(c);
        }
    }
    if (targets.empty()) return;

    // One frame per alias, exactly as realtime.js emits under every alias.
    std::vector<std::string> frames;
    frames.reserve(events.size());
    for (const auto& ev : events) frames.push_back(frame(ev, data));

    for (const auto& c : targets)
        for (const auto& f : frames) c->send(f);
}

void PresenceHub::emitRoom(const std::string& room,
                           const std::vector<std::string>& events,
                           const Json::Value& data,
                           const std::string& exceptUserId) {
    // Publish to every process (the Socket.IO Redis-adapter analogue). The
    // origin process applies the same exclusion when the message comes back.
    Json::Value msg(Json::objectValue);
    msg["room"] = room;
    msg["except"] = exceptUserId;  // userId to skip, "" = nobody
    Json::Value evs(Json::arrayValue);
    for (const auto& e : events) evs.append(e);
    msg["events"] = evs;
    msg["data"] = data;

    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    std::string payload = Json::writeString(w, msg);

    try {
        if (pub_) {
            pub_->publish(kChannel, payload);
            return;
        }
    } catch (const std::exception& e) {
        pulse::log::warn("\xE2\x9A\xA0\xEF\xB8\x8F  Presence publish failed: {}", e.what());
    }
    // Redis unavailable: still deliver to this process's own connections so a
    // single-process / no-Redis dev setup keeps working.
    handlePublished(payload);
}

void PresenceHub::handlePublished(const std::string& payload) {
    Json::Value msg;
    Json::CharReaderBuilder rb;
    std::string errs;
    std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
    if (!reader->parse(payload.data(), payload.data() + payload.size(), &msg,
                       &errs)) {
        return;
    }

    const std::string room = msg["room"].asString();
    const std::string exceptUserId = msg.get("except", "").asString();
    std::vector<std::string> events;
    for (const auto& e : msg["events"]) events.push_back(e.asString());
    const Json::Value& data = msg["data"];

    // `socket.to(room).emit` = everyone in the room EXCEPT the sender. Across
    // processes the sender is identified by userId, so skip every local
    // connection that belongs to exceptUserId.
    std::vector<WebSocketConnectionPtr> targets;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = rooms_.find(room);
        if (it == rooms_.end()) return;
        for (const auto& c : it->second) {
            if (!c || !c->connected()) continue;
            if (!exceptUserId.empty()) {
                auto ctx = c->getContext<PresenceContext>();
                if (ctx && ctx->userId == exceptUserId) continue;
            }
            targets.push_back(c);
        }
    }
    if (targets.empty()) return;

    std::vector<std::string> frames;
    frames.reserve(events.size());
    for (const auto& ev : events) frames.push_back(frame(ev, data));

    for (const auto& c : targets)
        for (const auto& f : frames) c->send(f);
}

// ─────────────────────────── PresenceSocket ─────────────────────────────────

void PresenceSocket::handleNewConnection(const HttpRequestPtr& req,
                                         const WebSocketConnectionPtr& conn) {
    // server.js io.use(...): reject connections without a valid access token.
    const std::string token = extractToken(req);
    if (token.empty()) {
        pulse::log::warn("\xE2\x9B\x94 Socket connection rejected \xE2\x80\x94 no auth token provided");
        conn->shutdown(CloseCode::kViolation, "Authentication required");
        return;
    }

    std::string userId;
    AccessClaims claims;
    if (pulse::filters::validateAccessToken(token, claims) !=
        pulse::filters::AccessTokenStatus::Valid) {
        pulse::log::warn(
            "Socket connection rejected - invalid/revoked token or inactive session");
        conn->shutdown(CloseCode::kViolation, "Authentication failed");
        return;
    }
    userId = claims.userId;

    auto ctx = std::make_shared<PresenceContext>();
    ctx->userId = userId;
    ctx->accessToken = token;
    conn->setContext(ctx);

    auto& hub = PresenceHub::instance();
    const std::size_t cap = maxConnectionsPerUser();
    if (!hub.registerConnection(userId, conn, cap)) {
        pulse::log::warn("Presence socket rejected - user at connection cap ({})",
                         cap);
        conn->setContext(nullptr);
        conn->shutdown(CloseCode::kViolation, "Connection limit reached");
        return;
    }

    // server.js io.on('connection'): always join the user's own room so server
    // code can target a specific user regardless of which worker they are on.
    const std::string userRoom = "user_" + userId;
    hub.addToRoom(userRoom, conn);
    ctx->rooms.insert(userRoom);

    // Every accepted socket owns exactly one Redis presence registration.
    bool registered = false;
    const bool justCameOnline =
        pulse::presence().addConnection(userId, registered);
    ctx->presenceRegistered.store(registered);
    if (justCameOnline) notifyConversationPeers(userId, true);
}

void PresenceSocket::handleNewMessage(const WebSocketConnectionPtr& conn,
                                      std::string&& message,
                                      const WebSocketMessageType& type) {
    auto ctx = conn->getContext<PresenceContext>();
    if (!ctx) return;

    // Control frames are handled by the transport and require no DB lookup.
    if (type == WebSocketMessageType::Ping ||
        type == WebSocketMessageType::Pong ||
        type == WebSocketMessageType::Close) {
        return;
    }
    if (type != WebSocketMessageType::Text) {
        conn->shutdown(CloseCode::kInvalidMessage, "Text frames required");
        return;
    }
    if (message.size() > maxInboundFrameBytes()) {
        conn->shutdown(CloseCode::kMessageTooBig, "Frame too large");
        return;
    }

    // The in-memory limiter runs before active-session validation, preventing
    // a frame flood from becoming a MongoDB lookup flood.
    if (!ctx->frameLimiter.guard("default")) return;

    AccessClaims currentClaims;
    if (pulse::filters::validateAccessToken(ctx->accessToken, currentClaims) !=
            pulse::filters::AccessTokenStatus::Valid ||
        currentClaims.userId != ctx->userId) {
        conn->shutdown(CloseCode::kViolation,
                       "Authentication expired or revoked");
        return;
    }

    Json::Value root;
    Json::CharReaderBuilder rb;
    std::string errs;
    std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
    if (!reader->parse(message.data(), message.data() + message.size(), &root,
                       &errs)) {
        return;  // ignore malformed frames (no error event for presence)
    }

    const std::string event = root.get("event", "").asString();

    // handler #9: user_online / user-online (both aliases share one handler).
    if (event == "user_online" || event == "user-online") {
        handleUserOnline(conn);
    }
    // No other presence events exist in the JS source; everything else is a
    // no-op here (chat / typing / reactions are ported in the realtime socket).
}

void PresenceSocket::handleConnectionClosed(const WebSocketConnectionPtr& conn) {
    auto ctx = conn->getContext<PresenceContext>();
    PresenceHub::instance().dropConnection(conn);
    if (!ctx) return;  // never authenticated
    if (ctx->closed.exchange(true)) return;
    if (!ctx->presenceRegistered.exchange(false)) return;

    // handler #10 (disconnect): Redis DECR; only emit "offline" when the LAST
    // socket for this user goes away. No DB write for the presence bookkeeping.
    try {
        bool wentOffline = pulse::presence().removeConnection(ctx->userId);
        if (wentOffline) {
            notifyConversationPeers(ctx->userId, false);
        }
    } catch (const std::exception& error) {
        pulse::log::error("\xE2\x9D\x8C Disconnect handler error: {}", error.what());
    }
}

void PresenceSocket::handleUserOnline(const WebSocketConnectionPtr& conn) {
    auto ctx = conn->getContext<PresenceContext>();
    if (!ctx) return;
    // Registration happens once during the authenticated connect. A client
    // heartbeat only refreshes the TTL; incrementing here double-counts.
    pulse::presence().touch(ctx->userId);
}

void PresenceSocket::notifyConversationPeers(const std::string& userId,
                                             bool isOnline) {
    // Tell the user's conversation peers about an online/offline change. Scoped,
    // capped fan-out — bounded by the number of conversations the user is in
    // (limit 500), never a global broadcast. Mirrors:
    //   Conversation.find({ participants: userId }).select('_id').limit(500)
    auto oid = pulse::bsonjson::tryOid(userId);
    if (!oid) return;

    using bsoncxx::builder::basic::kvp;
    using bsoncxx::builder::basic::make_document;

    // Payload is identical for every peer: { userId, isOnline }.
    Json::Value payload(Json::objectValue);
    payload["userId"] = userId;
    payload["isOnline"] = isOnline;

    auto& hub = PresenceHub::instance();
    try {
        auto col = pulse::db::collection(pulse::models::conversation::kCollection);
        mongocxx::options::find opts;
        opts.projection(make_document(kvp("_id", 1)));
        opts.limit(500);

        auto cursor = col.find(make_document(kvp("participants", *oid)), opts);
        for (const auto& doc : cursor) {
            auto idEl = doc["_id"];
            if (!idEl || idEl.type() != bsoncxx::type::k_oid) continue;
            std::string convId = pulse::bsonjson::oidToHex(idEl.get_oid().value);
            // emitOthers(conv, ['user_status_change','user-status-change'], payload)
            // — everyone in the conversation room except the originating user.
            hub.emitRoom(convId, kStatusChangeEvents, payload, userId);
        }
    } catch (const std::exception& error) {
        pulse::log::error("\xE2\x9D\x8C notifyConversationPeers error: {}", error.what());
    }
}

}  // namespace pulse::sockets
