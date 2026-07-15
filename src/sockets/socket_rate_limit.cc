// socket_rate_limit.cc — per-socket event rate limiting + WS handshake scaffold.
// Ports src/sockets/socketRateLimit.js (token buckets / LIMITS / guard) and the
// Socket.IO handshake auth + per-user room join wired in src/server.js.
//
// Every Redis key, TTL, event name and payload shape is preserved 1:1 from the
// JS source. The only event this unit emits is `rate_limited` { category }.
#include "pulse/sockets/socket_rate_limit.hpp"

#include <json/json.h>

#include <algorithm>
#include <vector>

using namespace drogon;

namespace pulse::sockets {

// ── LIMITS (verbatim from socketRateLimit.js) ────────────────────────────────
const RateLimitConfig& limitFor(const std::string& category) {
  // message:  send_message / delete
  // reaction: add/remove reaction, mark seen
  // typing:   typing indicators
  // presence: user_online etc.
  static const RateLimitConfig kMessage  {5,  10};
  static const RateLimitConfig kReaction {8,  16};
  static const RateLimitConfig kTyping   {4,  8};
  static const RateLimitConfig kPresence {1,  3};
  static const RateLimitConfig kDefault  {10, 20};

  if (category == "message")  return kMessage;
  if (category == "reaction") return kReaction;
  if (category == "typing")   return kTyping;
  if (category == "presence") return kPresence;
  // LIMITS[category] || LIMITS.default — unknown categories fall back to default.
  return kDefault;
}

namespace {
// Date.now() — epoch milliseconds, matching the JS clock the JS bucket used.
int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
} // namespace

// ── TokenBucket ──────────────────────────────────────────────────────────────
TokenBucket::TokenBucket(double ratePerSec, double burst)
    : ratePerSec_(ratePerSec), burst_(burst), tokens_(burst), lastMs_(nowMs()) {}

bool TokenBucket::tryRemove(int64_t now) {
  const double elapsed = (now - lastMs_) / 1000.0;
  if (elapsed > 0) {
    tokens_ = std::min(burst_, tokens_ + elapsed * ratePerSec_);
    lastMs_ = now;
  }
  if (tokens_ >= 1) {
    tokens_ -= 1;
    return true;
  }
  return false;
}

// ── SocketLimiter (ports createSocketLimiter's returned guard) ───────────────
bool SocketLimiter::guard(const std::string& category, bool& emitRateLimited) {
  emitRateLimited = false;
  const RateLimitConfig& cfg = limitFor(category);

  auto it = buckets_.find(category);
  if (it == buckets_.end()) {
    it = buckets_.emplace(category, TokenBucket(cfg.ratePerSec, cfg.burst)).first;
  }

  const bool allowed = it->second.tryRemove(nowMs());
  if (!allowed) {
    // Only emit a throttle notice occasionally to avoid amplifying the flood.
    if (warned_ % 20 == 0) {
      emitRateLimited = true;  // caller emits `rate_limited` { category }.
    }
    warned_++;
  }
  return allowed;
}

// ── emitEvent — one Socket.IO-style frame ["<event>",<payload>] ──────────────
void emitEvent(const WebSocketConnectionPtr& conn, const std::string& event,
               const std::string& payloadJson) {
  if (!conn || !conn->connected()) return;
  Json::Value frame(Json::arrayValue);
  frame.append(event);
  if (payloadJson.empty()) {
    frame.append(Json::Value(Json::objectValue));
  } else {
    Json::Value parsed;
    Json::CharReaderBuilder b;
    std::string errs;
    const std::unique_ptr<Json::CharReader> reader(b.newCharReader());
    if (reader->parse(payloadJson.data(), payloadJson.data() + payloadJson.size(),
                      &parsed, &errs)) {
      frame.append(parsed);
    } else {
      frame.append(payloadJson);  // fall back to raw string payload
    }
  }
  Json::StreamWriterBuilder w;
  w["indentation"] = "";
  conn->send(Json::writeString(w, frame));
}

// ── RoomRegistry ─────────────────────────────────────────────────────────────
RoomRegistry& RoomRegistry::instance() {
  static RoomRegistry r;
  return r;
}

void RoomRegistry::join(const std::string& room, const WebSocketConnectionPtr& conn) {
  if (room.empty() || !conn) return;
  std::lock_guard<std::mutex> lk(mu_);
  rooms_[room].insert(conn);
}

void RoomRegistry::leave(const std::string& room, const WebSocketConnectionPtr& conn) {
  if (room.empty() || !conn) return;
  std::lock_guard<std::mutex> lk(mu_);
  auto it = rooms_.find(room);
  if (it == rooms_.end()) return;
  it->second.erase(conn);
  if (it->second.empty()) rooms_.erase(it);
}

void RoomRegistry::leaveAll(const WebSocketConnectionPtr& conn) {
  if (!conn) return;
  std::lock_guard<std::mutex> lk(mu_);
  for (auto it = rooms_.begin(); it != rooms_.end();) {
    it->second.erase(conn);
    if (it->second.empty()) it = rooms_.erase(it);
    else ++it;
  }
}

bool RoomRegistry::inRoom(const std::string& room, const WebSocketConnectionPtr& conn) {
  if (room.empty() || !conn) return false;
  std::lock_guard<std::mutex> lk(mu_);
  auto it = rooms_.find(room);
  return it != rooms_.end() && it->second.count(conn) > 0;
}

void RoomRegistry::emitToRoom(const std::string& room, const std::string& event,
                              const std::string& payloadJson) {
  std::vector<WebSocketConnectionPtr> targets;
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = rooms_.find(room);
    if (it == rooms_.end()) return;
    for (auto cit = it->second.begin(); cit != it->second.end();) {
      if (*cit && (*cit)->connected()) { targets.push_back(*cit); ++cit; }
      else cit = it->second.erase(cit);  // prune dead connections
    }
    if (it->second.empty()) rooms_.erase(it);
  }
  for (const auto& c : targets) emitEvent(c, event, payloadJson);
}

void RoomRegistry::emitToRoomExcept(const std::string& room, const std::string& event,
                                    const std::string& payloadJson,
                                    const WebSocketConnectionPtr& except) {
  std::vector<WebSocketConnectionPtr> targets;
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = rooms_.find(room);
    if (it == rooms_.end()) return;
    for (auto cit = it->second.begin(); cit != it->second.end();) {
      if (*cit && (*cit)->connected()) {
        if (*cit != except) targets.push_back(*cit);
        ++cit;
      } else {
        cit = it->second.erase(cit);  // prune dead connections
      }
    }
    if (it->second.empty()) rooms_.erase(it);
  }
  for (const auto& c : targets) emitEvent(c, event, payloadJson);
}

} // namespace pulse::sockets
