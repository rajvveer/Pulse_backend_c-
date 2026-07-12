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

namespace pulse {

using sw::redis::Redis;
using sw::redis::ConnectionOptions;
using sw::redis::ConnectionPoolOptions;

namespace {
// Build a redis-plus-plus client from env (REDIS_URL wins, else host/port/pw).
std::shared_ptr<Redis> buildClient() {
  auto& cfg = config();
  if (!cfg.redis.url.empty()) {
    return std::make_shared<Redis>(cfg.redis.url);
  }
  ConnectionOptions opts;
  opts.host = cfg.redis.host;
  opts.port = cfg.redis.port;
  if (!cfg.redis.password.empty()) opts.password = cfg.redis.password;
  ConnectionPoolOptions pool;
  pool.size = 8;
  return std::make_shared<Redis>(opts, pool);
}

// Single-flight registry for getOrSet (per process). Mirrors the JS _inflight.
std::mutex g_flightMu;
std::unordered_map<std::string, std::shared_ptr<std::shared_future<std::string>>> g_flight;
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
  try {
    if (ttl) redis_->setex(key, ttl, value);
    else     redis_->set(key, value);
    return true;
  } catch (...) { return false; }
}

std::optional<std::string> CacheService::get(const std::string& key) {
  try {
    auto v = redis_->get(key);            // sw::redis::OptionalString
    if (v) return std::optional<std::string>(*v);
    return std::nullopt;
  }
  catch (...) { return std::nullopt; }
}

long long CacheService::del(const std::string& key) {
  try { return redis_->del(key); }
  catch (...) { return 0; }
}

bool CacheService::exists(const std::string& key) {
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

  std::shared_ptr<std::shared_future<std::string>> fut;
  bool owner = false;
  {
    std::lock_guard<std::mutex> lk(g_flightMu);
    auto it = g_flight.find(key);
    if (it != g_flight.end()) {
      fut = it->second;
    } else {
      auto task = std::make_shared<std::packaged_task<std::string()>>([&]() {
        std::string fresh = fetch();
        if (!fresh.empty()) set(key, fresh, jitter(ttl));
        return fresh;
      });
      fut = std::make_shared<std::shared_future<std::string>>(task->get_future().share());
      g_flight[key] = fut;
      owner = true;
      // Run synchronously: this coalesces concurrent callers onto one fetch.
      (*task)();
    }
  }
  std::string result = fut->get();
  if (owner) {
    std::lock_guard<std::mutex> lk(g_flightMu);
    g_flight.erase(key);
  }
  return result;
}

long long CacheService::delPattern(const std::string& pattern) {
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
  try {
    static const std::string lua =
        "local c = redis.call('INCR', KEYS[1])\n"
        "if c == 1 then redis.call('EXPIRE', KEYS[1], ARGV[1]) end\n"
        "return c";
    auto r = redis_->eval<long long>(lua, {key}, {std::to_string(ttl)});
    return r;
  } catch (...) { return 1; }
}

std::shared_ptr<Redis> CacheService::createClient() { return buildClient(); }

void CacheService::disconnect() { /* redis-plus-plus pools close on destruction */ }

std::string CacheService::getStats() {
  try { return std::string("{\"raw\":\"") + redis_->info("memory") + "\"}"; }
  catch (...) { return "{\"error\":\"Stats unavailable\"}"; }
}

} // namespace pulse
