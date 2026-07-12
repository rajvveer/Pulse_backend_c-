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

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <mongocxx/options/find.hpp>

#include <sw/redis++/redis++.h>

#include <atomic>
#include <thread>

using namespace drogon;

namespace pulse::sockets {

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
    std::string token = req->getParameter("token");
    if (!token.empty()) return token;

    std::string auth = req->getHeader("authorization");
    if (auth.empty()) auth = req->getHeader("Authorization");
    const std::string bearer = "Bearer ";
    if (auth.rfind(bearer, 0) == 0) return auth.substr(bearer.size());
    return auth;  // tolerate a bare token in the header, as the JS path did not
}

}  // namespace

// ───────────────────────────── PresenceHub ──────────────────────────────────

PresenceHub& PresenceHub::instance() {
    static PresenceHub hub;
    return hub;
}

PresenceHub::PresenceHub() {
    try {
        pub_ = pulse::cache().createClient();
    } catch (const std::exception& e) {
        pulse::log::error("\xE2\x9D\x8C Presence pub client init failed: {}", e.what());
    }
    startSubscriber();
}

void PresenceHub::startSubscriber() {
    try {
        sub_ = pulse::cache().createClient();
    } catch (const std::exception& e) {
        pulse::log::error("\xE2\x9D\x8C Presence sub client init failed: {}", e.what());
        return;
    }

    // redis-plus-plus Subscriber owns a dedicated connection; consume() blocks,
    // so it runs on its own detached thread for the life of the process. The
    // captured shared_ptr keeps the subscriber's underlying Redis alive.
    auto sub = sub_;
    std::thread([this, sub]() {
        for (;;) {
            try {
                auto subscriber = sub->subscriber();
                subscriber.on_message(
                    [this](std::string /*channel*/, std::string msg) {
                        handlePublished(msg);
                    });
                subscriber.subscribe(kChannel);
                for (;;) subscriber.consume();
            } catch (const std::exception& e) {
                pulse::log::warn("\xE2\x9A\xA0\xEF\xB8\x8F  Presence subscriber dropped: {} — reconnecting",
                                 e.what());
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }).detach();
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
    try {
        // Same verification path as REST: pinned algorithm, issuer, audience,
        // and token type — a refresh or temp token cannot open a socket.
        AccessClaims claims = pulse::jwt().verifyAccessToken(token);
        userId = claims.userId;
    } catch (const std::exception& err) {
        pulse::log::warn("\xE2\x9B\x94 Socket connection rejected \xE2\x80\x94 {}", err.what());
        conn->shutdown(CloseCode::kViolation,
                       std::string("Authentication failed: ") + err.what());
        return;
    }

    auto ctx = std::make_shared<PresenceContext>();
    ctx->userId = userId;
    conn->setContext(ctx);

    // server.js io.on('connection'): always join the user's own room so server
    // code can target a specific user regardless of which worker they are on.
    const std::string userRoom = "user_" + userId;
    PresenceHub::instance().addToRoom(userRoom, conn);
    ctx->rooms.insert(userRoom);
}

void PresenceSocket::handleNewMessage(const WebSocketConnectionPtr& conn,
                                      std::string&& message,
                                      const WebSocketMessageType& type) {
    if (type == WebSocketMessageType::Ping ||
        type == WebSocketMessageType::Pong ||
        type == WebSocketMessageType::Close) {
        return;
    }
    auto ctx = conn->getContext<PresenceContext>();
    if (!ctx) return;  // unauthenticated frame; handshake should have closed it

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
    try {
        bool justCameOnline = pulse::presence().addConnection(ctx->userId);
        if (!justCameOnline) return;  // another tab/device was already online
        notifyConversationPeers(ctx->userId, true);
    } catch (const std::exception& error) {
        pulse::log::error("\xE2\x9D\x8C User online error: {}", error.what());
    }
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
