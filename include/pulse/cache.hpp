// cache.hpp — Redis cache layer, ports src/services/cacheService.js.
//
// Backed by redis-plus-plus. Values are JSON-serialized strings (the JS service
// did JSON.stringify on set / JSON.parse on get), so get/set here operate on
// JSON strings. A process-wide singleton mirrors the JS module singleton.
//
// Key semantics preserved 1:1:
//   set(key, json, ttl=600)   -> SETEX (or SET when ttl=0)
//   get(key)                  -> raw stored string or "" (null) on miss
//   getOrSet                  -> cache-aside + single-flight + ±10% TTL jitter
//   delPattern                -> SCAN-based bulk delete (never KEYS)
//   incrementRateLimit        -> atomic INCR + EXPIRE-on-first via Lua
#pragma once
#include <string>
#include <functional>
#include <optional>
#include <cstdint>
#include <memory>

namespace sw::redis { class Redis; }

namespace pulse {

class CacheService {
public:
  static CacheService& instance();

  bool isConnected() const { return connected_; }

  bool ping();

  // value is an already-serialized JSON string. ttl in seconds (0 = no expiry).
  bool set(const std::string& key, const std::string& value, int64_t ttl = 600);

  // Returns the stored string, or std::nullopt on miss / Redis error.
  std::optional<std::string> get(const std::string& key);

  long long del(const std::string& key);
  bool exists(const std::string& key);

  // Cache-aside with single-flight coalescing + TTL jitter. fetch() returns the
  // fresh JSON string; only non-empty results are cached.
  std::string getOrSet(const std::string& key,
                       const std::function<std::string()>& fetch,
                       int64_t ttl = 600);

  // SCAN + DEL every key matching `pattern`. Returns count deleted.
  long long delPattern(const std::string& pattern);

  // Atomic INCR; sets EXPIRE only on the first increment. Returns new count.
  long long incrementRateLimit(const std::string& key, int64_t ttl = 60);

  // Raw client for pub/sub (Socket.IO adapter analogue). Caller owns lifecycle.
  std::shared_ptr<sw::redis::Redis> createClient();

  // Direct passthroughs used by presence / realtime (SADD/SREM/SMEMBERS/etc.)
  std::shared_ptr<sw::redis::Redis> raw() { return redis_; }

  void disconnect();
  std::string getStats();

private:
  CacheService();
  int64_t jitter(int64_t ttl) const;

  std::shared_ptr<sw::redis::Redis> redis_;
  bool connected_ = false;
};

inline CacheService& cache() { return CacheService::instance(); }

} // namespace pulse
