// presence_service.cc — Presence implementation. Ports src/services/presenceService.js.
//
// Online state is stored in Redis (never MongoDB). See presence_service.hpp for
// the key layout / TTL rationale. Every Redis key string and TTL value here is
// preserved verbatim from the JS source.
#include "pulse/services/presence_service.hpp"
#include "pulse/cache.hpp"

#include <sw/redis++/redis++.h>
#include <chrono>
#include <vector>
#include <optional>
#include <iterator>

namespace pulse {

using sw::redis::Redis;

PresenceService& PresenceService::instance() { static PresenceService p; return p; }

namespace {
// Shared TTLs (seconds). The counter lives 4x as long as the presence key so it
// never leaks if a process dies mid-session.
constexpr std::chrono::seconds kPresenceTtl{PresenceService::PRESENCE_TTL_SEC};       // 90s
constexpr std::chrono::seconds kCountTtl{PresenceService::PRESENCE_TTL_SEC * 4};       // 360s
} // namespace

// Register a new socket for a user. Returns true if the user JUST came online.
bool PresenceService::addConnection(const std::string& userId) {
  if (userId.empty()) return false;
  try {
    auto redis = cache().raw();
    if (!redis) return false;
    // Increment counter -> count.
    long long count = redis->incr(countKeyFor(userId));
    // Keep the counter from leaking if a process dies mid-session.
    redis->expire(countKeyFor(userId), kCountTtl);
    // Set presence key '1' with 90s TTL.
    redis->set(keyFor(userId), "1", kPresenceTtl);
    return count == 1;
  } catch (...) {
    return false;
  }
}

// De-register a socket for a user. Returns true if the user just went OFFLINE.
bool PresenceService::removeConnection(const std::string& userId) {
  if (userId.empty()) return false;
  try {
    auto redis = cache().raw();
    if (!redis) return false;
    long long count = redis->decr(countKeyFor(userId));
    if (count <= 0) {
      // Floor at 0: a crash/abnormal close or any INCR/DECR asymmetry could
      // otherwise drive the counter negative — which would leave it stuck below
      // 0 (so a later real connect never reads count==1) and DEL a
      // legitimately-online user's presence key. Reset to a clean offline state.
      redis->del(countKeyFor(userId));
      redis->del(keyFor(userId));
      return count == 0;  // only a true 1->0 transition reports "went offline"
    }
    // Counter still positive — keep its TTL fresh so it can't strand.
    redis->expire(countKeyFor(userId), kCountTtl);
    return false;
  } catch (...) {
    return false;
  }
}

// Refresh the TTL — call on socket heartbeat / activity. Best-effort.
void PresenceService::touch(const std::string& userId) {
  if (userId.empty()) return;
  try {
    auto redis = cache().raw();
    if (!redis) return;
    redis->set(keyFor(userId), "1", kPresenceTtl);
  } catch (...) {
    /* best-effort */
  }
}

// Is a single user online?
bool PresenceService::isOnline(const std::string& userId) {
  if (userId.empty()) return false;
  try {
    auto redis = cache().raw();
    if (!redis) return false;
    return redis->exists(keyFor(userId)) == 1;
  } catch (...) {
    return false;
  }
}

// Bulk presence check via a single MGET round-trip.
std::unordered_set<std::string> PresenceService::getOnlineSet(
    const std::vector<std::string>& userIds) {
  std::unordered_set<std::string> online;
  if (userIds.empty()) return online;
  try {
    auto redis = cache().raw();
    if (!redis) return online;

    // ids are already string userIds (JS did id.toString()); build the keys.
    std::vector<std::string> keys;
    keys.reserve(userIds.size());
    for (const auto& id : userIds) keys.push_back(keyFor(id));

    // MGET returns a parallel vector of OptionalString (null on a missing key).
    // A present value means that user is online. (redis-plus-plus writes its own
    // sw::redis::OptionalString, not std::optional.)
    std::vector<sw::redis::OptionalString> values;
    values.reserve(keys.size());
    redis->mget(keys.begin(), keys.end(), std::back_inserter(values));

    for (std::size_t i = 0; i < userIds.size() && i < values.size(); ++i) {
      if (values[i]) online.insert(userIds[i]);
    }
  } catch (...) {
    // Degrade to "everyone offline" rather than throw.
  }
  return online;
}

} // namespace pulse
