// scheduler.cc — background job scheduler. Ports src/jobs/scheduler.js.
//
// Runs inside the API process when ENABLE_BACKGROUND_JOBS=true. A Redis SET NX
// EX lock guarantees each job runs on exactly one process per interval, so it's
// safe under multiple instances. The lock TTL is the schedule: a held lock
// means the job already ran this interval somewhere.
#include "pulse/scheduler.hpp"
#include "pulse/cache.hpp"
#include "pulse/config.hpp"
#include "pulse/logger.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/models/userengagement.hpp"
#include "pulse/models/session.hpp"
#include "pulse/models/socialdna.hpp"

#include <drogon/drogon.h>
#include <sw/redis++/redis++.h>
#include <functional>
#include <vector>
#include <string>
#include <random>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>

// Defined in src/controllers/post_controller.cc — the trending-hashtags
// aggregation, returning the compact JSON string to cache.
namespace pulse::controllers { std::string computeTrendingHashtagsJson(); }

namespace pulse::jobs {

namespace {
constexpr long long MINUTE = 60'000;
constexpr long long HOUR   = 60 * MINUTE;
constexpr long long DAY    = 24 * HOUR;

struct Job {
  std::string name;
  long long intervalMs;
  long long lockTtlSec;
  std::function<void()> run;
};

// Acquire the distributed per-interval lock: SET job_lock:<name> <nowMs> EX ttl NX.
// Returns true only if WE set the key (i.e. no other instance holds it this
// interval). Mirrors scheduler.js acquireLock.
bool acquireLock(const std::string& name, long long intervalMs, long long lockTtlSec) {
  try {
    long long ttl = std::max(intervalMs / 1000 - 60, lockTtlSec);
    auto redis = pulse::cache().raw();
    if (!redis) {
      pulse::log::warn("[jobs] Redis unavailable; skipping {} tick", name);
      return false;
    }
    using namespace sw::redis;
    std::string value = std::to_string(pulse::bsonjson::nowMillis()); // String(Date.now())
    auto ok = redis->set("job_lock:" + name, value,
                         std::chrono::seconds(ttl), UpdateType::NOT_EXIST);
    return ok; // sw::redis::Redis::set returns bool (false when NX precondition fails)
  } catch (const std::exception& e) {
    pulse::log::warn("[jobs] Lock check failed for {}, skipping tick: {}", name, e.what());
    return false;
  }
}

void runJob(const Job& job) {
  if (!acquireLock(job.name, job.intervalMs, job.lockTtlSec)) return;
  auto start = std::chrono::steady_clock::now();
  pulse::log::info("[jobs] Starting {}...", job.name);
  try {
    job.run();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start).count();
    pulse::log::info("[jobs] {} completed in {}ms", job.name, ms);
  } catch (const std::exception& e) {
    pulse::log::error("[jobs] {} failed: {}", job.name, e.what());
  }
}

std::vector<Job> buildJobs() {
  return {
    { "engagement-decay", DAY, 6 * 60 * 60,
      []{ pulse::models::userengagement::applyGlobalDecay(); } },
    { "session-cleanup", DAY, 60 * 60,
      []{ pulse::models::session::cleanupExpired(); } },
    { "weekly-dna-computation", 7 * DAY, 12 * 60 * 60,
      []{ pulse::models::socialdna::runWeeklyComputation(); } },
    { "trending-hashtags", 5 * MINUTE, 4 * 60,
      []{
        // Precompute trending hashtags into Redis (postController exposes the
        // computation as a public, external-linkage function).
        std::string data = pulse::controllers::computeTrendingHashtagsJson();
        long long ttl = pulse::config().envInt("TRENDING_HASHTAG_TTL_SEC", 600);
        pulse::cache().set("trending:hashtags", data, ttl);
      } },
  };
}

std::atomic<bool> g_started{false};
std::mutex g_queueMu;
std::condition_variable g_queueCv;
std::deque<Job> g_queue;
std::unordered_set<std::string> g_pending;
std::vector<std::thread> g_workers;

void workerLoop() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(g_queueMu);
      g_queueCv.wait(lock, [] { return !g_started.load() || !g_queue.empty(); });
      if (!g_started.load() && g_queue.empty()) return;
      job = std::move(g_queue.front());
      g_queue.pop_front();
    }

    runJob(job);

    std::lock_guard<std::mutex> lock(g_queueMu);
    g_pending.erase(job.name);
  }
}

void enqueueJob(const Job& job) {
  if (!g_started.load()) return;
  {
    std::lock_guard<std::mutex> lock(g_queueMu);
    if (!g_started.load() || !g_pending.insert(job.name).second) return;
    g_queue.push_back(job);
  }
  g_queueCv.notify_one();
}
} // namespace

void start() {
  bool expected = false;
  if (!g_started.compare_exchange_strong(expected, true)) return;

  const int workerCount = static_cast<int>(std::max<int64_t>(
      1, std::min<int64_t>(8,
          pulse::config().envInt("BACKGROUND_JOB_WORKERS", 2))));
  g_workers.reserve(static_cast<size_t>(workerCount));
  for (int i = 0; i < workerCount; ++i) g_workers.emplace_back(workerLoop);

  auto jobs = std::make_shared<std::vector<Job>>(buildJobs());
  auto loop = drogon::app().getLoop();
  static thread_local std::mt19937_64 rng{std::random_device{}()};

  for (size_t i = 0; i < jobs->size(); ++i) {
    const Job& job = (*jobs)[i];
    // Stagger initial runs (60s + up to 4min jitter), then run every interval.
    std::uniform_int_distribution<long long> d(0, 4 * 60 * 1000);
    double initialDelaySec = (60'000 + d(rng)) / 1000.0;
    double intervalSec = job.intervalMs / 1000.0;
    loop->runAfter(initialDelaySec, [jobs, i]{ enqueueJob((*jobs)[i]); });
    loop->runEvery(intervalSec, [jobs, i]{ enqueueJob((*jobs)[i]); });
  }
  pulse::log::info("[jobs] Scheduler started with {} jobs and {} workers",
                   jobs->size(), workerCount);
}

void requestStop() {
  if (!g_started.exchange(false)) return;
  {
    std::lock_guard<std::mutex> lock(g_queueMu);
    g_queue.clear();
    g_pending.clear();
  }
  g_queueCv.notify_all();
}

void stop() {
  requestStop();
  for (auto& worker : g_workers) {
    if (worker.joinable()) worker.join();
  }
  g_workers.clear();
}

} // namespace pulse::jobs
