// presence_service.hpp — Presence layer, ports src/services/presenceService.js.
//
// Tracks which users are online WITHOUT touching MongoDB. Online state lives in
// Redis as short-TTL keys (`presence:<userId>`) so it is shared across all
// cluster workers / containers and self-heals if a process dies (the key simply
// expires). A per-user socket counter (`presence:count:<userId>`) handles the
// common multi-device / multi-tab case: a user only flips "offline" when their
// LAST socket disconnects.
//
// Redis layout (preserved 1:1 from the JS source):
//   presence:<userId>       = '1'   (key exists => online), TTL 90s
//   presence:count:<userId> = <int> connected-socket count, TTL 360s (4x) to
//                             prevent the counter leaking if a process dies.
//
// A process-wide singleton mirrors the JS module singleton.
#pragma once
#include <string>
#include <vector>
#include <unordered_set>
#include <cstdint>

namespace pulse {

class PresenceService {
public:
  // How long a presence key lives without a refresh. Sockets ping every ~25s,
  // so 90s comfortably survives a missed heartbeat without showing ghosts.
  static constexpr int PRESENCE_TTL_SEC = 90;

  static PresenceService& instance();

  // Register a new socket for a user. Returns true if the user JUST came online
  // (this was their first connected socket), so the caller can notify peers.
  bool addConnection(const std::string& userId);

  // De-register a socket for a user. Returns true if the user just went OFFLINE
  // (their last socket disconnected).
  bool removeConnection(const std::string& userId);

  // Refresh the TTL — call on socket heartbeat / activity. Best-effort.
  void touch(const std::string& userId);

  // Is a single user online?
  bool isOnline(const std::string& userId);

  // Bulk presence check for a list of user IDs (e.g. a conversation list).
  // Returns the set of online userId strings via a single MGET round-trip.
  std::unordered_set<std::string> getOnlineSet(const std::vector<std::string>& userIds);

private:
  PresenceService() = default;

  static std::string keyFor(const std::string& userId)      { return "presence:" + userId; }
  static std::string countKeyFor(const std::string& userId) { return "presence:count:" + userId; }
};

inline PresenceService& presence() { return PresenceService::instance(); }

} // namespace pulse
