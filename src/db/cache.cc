// cache.cc — Redis cache implementation. Ports src/services/cacheService.js.
#include "pulse/cache.hpp"
#include "pulse/config.hpp"
#include "pulse/logger.hpp"

#include <sw/redis++/redis++.h>
#include <mutex>
#include <unordered_map>
#include <random>
#include <future>
#include <memory>
#include <unordered_set>
#include <stdexcept>
#include <algorithm>
#include <chrono>

namespace pulse {

using sw::redis::Redis;
using sw::redis::ConnectionOptions;
using sw::redis::ConnectionPoolOptions;
using sw::redis::Uri;

namespace {
std::chrono::milliseconds redisTimeout(const char* envName, int64_t defaultMs) {
  constexpr int64_t kMinMs = 100;
  constexpr int64_t kMaxMs = 120000;
  const int64_t configured = config().envInt(envName, defaultMs);
  return std::chrono::milliseconds{
      std::clamp(configured, kMinMs, kMaxMs)};
}

// Build a redis-plus-plus client from env (REDIS_URL wins, else host/port/pw).
std::shared_ptr<Redis> buildClient() {
  auto& cfg = config();
  const auto connectTimeout =
      redisTimeout("REDIS_CONNECT_TIMEOUT_MS", 5000);
  const auto socketTimeout =
      redisTimeout("REDIS_SOCKET_TIMEOUT_MS", 5000);

  if (!cfg.redis.url.empty()) {
    // Parse the URI first so TLS, auth, DB, and URI pool settings are retained,
    // then override the two formerly-unbounded network deadlines.
    Uri uri{cfg.redis.url};
    ConnectionOptions opts = uri.connection_options();
    ConnectionPoolOptions pool = uri.connection_pool_options();
    opts.connect_timeout = connectTimeout;
    opts.socket_timeout = socketTimeout;
    return std::make_shared<Redis>(opts, pool);
  }
  ConnectionOptions opts;
  opts.host = cfg.redis.host;
  opts.port = cfg.redis.port;
  if (!cfg.redis.password.empty()) opts.password = cfg.redis.password;
  opts.connect_timeout = connectTimeout;
  opts.socket_timeout = socketTimeout;
  ConnectionPoolOptions pool;
  pool.size = 8;
  return std::make_shared<Redis>(opts, pool);
}

// Single-flight registry for getOrSet (per process). Mirrors the JS _inflight.
struct Flight {
  Flight() : future(promise.get_future().share()) {}

  std::promise<std::string> promise;
  std::shared_future<std::string> future;
};

std::mutex g_flightMu;
std::unordered_map<std::string, std::shared_ptr<Flight>> g_flight;

// A same-key recursive fetch would wait on its own future forever. Detect it
// explicitly while still allowing nested getOrSet calls for unrelated keys.
thread_local std::unordered_set<std::string> g_activeFlights;

class FlightOwnerGuard {
public:
  FlightOwnerGuard(std::string key, std::shared_ptr<Flight> flight)
      : key_(std::move(key)), flight_(std::move(flight)) {}

  ~FlightOwnerGuard() {
    g_activeFlights.erase(key_);
    std::lock_guard<std::mutex> lk(g_flightMu);
    auto it = g_flight.find(key_);
    if (it != g_flight.end() && it->second == flight_) g_flight.erase(it);
  }

private:
  std::string key_;
  std::shared_ptr<Flight> flight_;
};
} // namespace

CacheService::CacheService() {
  try {
    redis_ = buildClient();
    redis_->ping();
    connected_ = true;
    pulse::log::info("\xE2\x9C\x85 Redis is ready to use!");
  } catch (const std::exception& e) {
    connected_ = false;
    pulse::log::error("\xE2\x9D\x8C Redis connection error: {}", e.what());
  }
}

CacheService& CacheService::instance() { static CacheService c; return c; }

bool CacheService::ping() {
  try { return redis_ && redis_->ping() == "PONG"; }
  catch (...) { return false; }
}

bool CacheService::set(const std::string& key, const std::string& value, int64_t ttl) {
  if (!redis_) return false;
  try {
    if (ttl) redis_->setex(key, ttl, value);
    else     redis_->set(key, value);
    return true;
  } catch (...) { return false; }
}

std::optional<std::string> CacheService::get(const std::string& key) {
  if (!redis_) return std::nullopt;
  try {
    auto v = redis_->get(key);            // sw::redis::OptionalString
    if (v) return std::optional<std::string>(*v);
    return std::nullopt;
  }
  catch (...) { return std::nullopt; }
}

long long CacheService::del(const std::string& key) {
  if (!redis_) return 0;
  try { return redis_->del(key); }
  catch (...) { return 0; }
}

bool CacheService::exists(const std::string& key) {
  if (!redis_) return false;
  try { return redis_->exists(key) > 0; }
  catch (...) { return false; }
}

int64_t CacheService::jitter(int64_t ttl) const {
  if (!ttl) return ttl;
  int64_t spread = ttl / 10;
  if (spread == 0) return ttl;
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uniform_int_distribution<int64_t> d(-spread, spread);
  return ttl + d(rng);
}

std::string CacheService::getOrSet(const std::string& key,
                                   const std::function<std::string()>& fetch,
                                   int64_t ttl) {
  if (auto cached = get(key); cached && !cached->empty()) return *cached;

  std::shared_ptr<Flight> flight;
  bool owner = false;
  {
    std::lock_guard<std::mutex> lk(g_flightMu);
    auto it = g_flight.find(key);
    if (it != g_flight.end()) {
      flight = it->second;
    } else {
      flight = std::make_shared<Flight>();
      g_flight.emplace(key, flight);
      owner = true;
    }
  }

  if (!owner) {
    if (g_activeFlights.find(key) != g_activeFlights.end()) {
      throw std::logic_error("recursive cache getOrSet for key: " + key);
    }
    return flight->future.get();
  }

  // Install cleanup before any operation that can throw. It removes only this
  // generation of the key and runs during exceptional unwinding as well.
  FlightOwnerGuard ownerGuard{key, flight};
  g_activeFlights.insert(key);
  try {
    std::string fresh = fetch();
    if (!fresh.empty()) set(key, fresh, jitter(ttl));
    flight->promise.set_value(std::move(fresh));
  } catch (...) {
    flight->promise.set_exception(std::current_exception());
  }

  return flight->future.get();
}

long long CacheService::delPattern(const std::string& pattern) {
  if (!redis_) return 0;
  try {
    long long deleted = 0;
    auto cursor = 0LL;
    std::vector<std::string> keys;
    while (true) {
      cursor = redis_->scan(cursor, pattern, 200, std::back_inserter(keys));
      if (!keys.empty()) { deleted += redis_->del(keys.begin(), keys.end()); keys.clear(); }
      if (cursor == 0) break;
    }
    return deleted;
  } catch (...) { return 0; }
}

long long CacheService::incrementRateLimit(const std::string& key, int64_t ttl) {
  // Rate-limit callers choose their own availability policy. Propagate Redis
  // failures so security-sensitive callers (OTP/SMS budgets) can fail closed,
  // while best-effort request limiters can explicitly catch and fail open.
  if (!redis_) throw std::runtime_error("Redis is unavailable");
  if (ttl <= 0) throw std::invalid_argument("Rate-limit TTL must be positive");

  static const std::string lua =
      "local c = redis.call('INCR', KEYS[1])\n"
      "if c == 1 then redis.call('EXPIRE', KEYS[1], ARGV[1]) end\n"
      "return c";
  return redis_->eval<long long>(lua, {key}, {std::to_string(ttl)});
}

std::shared_ptr<Redis> CacheService::createClient() { return buildClient(); }

void CacheService::disconnect() { /* redis-plus-plus pools close on destruction */ }

std::string CacheService::getStats() {
  if (!redis_) return "{\"error\":\"Stats unavailable\"}";
  try { return std::string("{\"raw\":\"") + redis_->info("memory") + "\"}"; }
  catch (...) { return "{\"error\":\"Stats unavailable\"}"; }
}

} // namespace pulse
