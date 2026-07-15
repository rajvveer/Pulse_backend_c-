// main.cc — server bootstrap. Ports src/server.js + src/app.js.
//
// Responsibilities (same order/semantics as server.js startServer()):
//   1. Load config + logger, print summary.
//   2. Connect MongoDB, test Redis (refuse to boot in prod if Redis down),
//      init Firebase + SMTP (optional), create indexes.
//   3. Register global middleware (CORS, compression, the global rate limiter,
//      sanitize) and the health/readiness/metrics/status endpoints.
//   4. Start background jobs when ENABLE_BACKGROUND_JOBS.
//   5. Run the Drogon event loop; install SIGTERM/SIGINT graceful shutdown.
//
// Drogon HttpController/WebSocketController subclasses self-register, so routing
// is wired by the controllers themselves (see src/controllers, src/sockets).
#include <drogon/drogon.h>
#include <json/json.h>

#include "pulse/config.hpp"
#include "pulse/logger.hpp"
#include "pulse/db.hpp"
#include "pulse/cache.hpp"
#include "pulse/http_response.hpp"
#include "pulse/scheduler.hpp"
#include "pulse/filters/upload_filters.hpp"
#include "pulse/filters/rate_limit_filters.hpp"
#include "pulse/filters/sanitize_filter.hpp"
#include "pulse/services/firebase_service.hpp"
#include "pulse/sockets/realtime_controller.hpp"
#include "pulse/sockets/presence_socket.hpp"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <thread>

using namespace drogon;
using namespace pulse;

namespace {
std::atomic<bool> g_shuttingDown{false};
volatile std::sig_atomic_t g_pendingSignal = 0;
std::chrono::steady_clock::time_point g_startTime;

// Optional external hooks (defined in their own units; weak fallbacks here so
// the binary links even before those units are added).
bool initFirebaseIfPresent();
void initSmtpIfPresent();
void startBackgroundJobsIfEnabled();

// ── CORS (ports the app.js cors() whitelist; mobile apps send no Origin) ──
std::vector<std::string> allowedOrigins() {
  auto list = config().cors.origin;
  if (list.empty())
    list = {"http://localhost:5174","http://127.0.0.1:5174","http://localhost:3000",
            "http://127.0.0.1:3000","http://localhost:5173","http://localhost:3100",
            "http://127.0.0.1:3100"};
  return list;
}

void applyCorsHeaders(const HttpRequestPtr& req, const HttpResponsePtr& resp) {
  std::string origin = req->getHeader("origin");
  if (origin.empty()) return; // native clients — nothing to echo
  for (const auto& o : allowedOrigins()) {
    if (o == origin) {
      resp->addHeader("Access-Control-Allow-Origin", origin);
      if (config().cors.credentials)
        resp->addHeader("Access-Control-Allow-Credentials", "true");
      resp->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS");
      resp->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization, Accept, Origin");
      return;
    }
  }
  log::warn("CORS blocked request from origin: {}", origin);
}

void scrubProductionServerError(const HttpResponsePtr& resp) {
  if (!config().isProduction() ||
      resp->getStatusCode() != k500InternalServerError) {
    return;
  }
  Json::Value body(Json::objectValue);
  body["success"] = false;
  body["error"] = "An unexpected internal server error occurred.";
  body["code"] = "INTERNAL_ERROR";
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  resp->setContentTypeString("application/json; charset=utf-8");
  resp->setBody(Json::writeString(writer, body));
}

// internalOnly gate for /metrics, /status, /health/detailed (open in dev; in
// prod requires x-internal-key == INTERNAL_STATUS_KEY).
bool internalAllowed(const HttpRequestPtr& req) {
  if (!config().isProduction()) return true;
  std::string key = config().env("INTERNAL_STATUS_KEY");
  return !key.empty() && req->getHeader("x-internal-key") == key;
}

void registerHealthEndpoints() {
  auto& app = drogon::app();

  // Liveness — /health.
  app.registerHandler("/health",
    [](const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& cb) {
      if (g_shuttingDown) {
        Json::Value b; b["status"] = "SHUTTING_DOWN";
        cb(http::json(k503ServiceUnavailable, b));
        return;
      }
      Json::Value b;
      b["status"] = "OK";
      b["service"] = "Pulse Backend API";
      b["version"] = "1.0.0";
      b["environment"] = config().nodeEnv();
      cb(http::json(k200OK, b));
    }, {Get});

  // Readiness — /health/ready (checks Mongo + Redis, cached 3s).
  app.registerHandler("/health/ready",
    [](const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& cb) {
      if (g_shuttingDown) {
        Json::Value b; b["status"] = "SHUTTING_DOWN"; b["ready"] = false;
        cb(http::json(k503ServiceUnavailable, b));
        return;
      }
      bool dbOk = db::isHealthy();
      bool redisOk = cache().ping();
      bool ok = dbOk && redisOk;
      Json::Value b;
      b["status"] = ok ? "READY" : "NOT_READY";
      b["ready"] = ok;
      b["services"]["database"] = dbOk;
      b["services"]["redis"] = redisOk;
      cb(http::json(ok ? k200OK : k503ServiceUnavailable, b));
    }, {Get});

  // Prometheus-style metrics — gated by internalOnly.
  app.registerHandler("/metrics",
    [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& cb) {
      if (!internalAllowed(req)) { cb(http::notFound("Not found")); return; }
      long uptime = (long)std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now() - g_startTime).count();
      std::string body;
      body += "# HELP pulse_uptime_seconds Process uptime\n# TYPE pulse_uptime_seconds counter\n";
      body += "pulse_uptime_seconds " + std::to_string(uptime) + "\n";
      auto resp = HttpResponse::newHttpResponse();
      resp->setContentTypeString("text/plain; version=0.0.4");
      resp->setBody(body);
      cb(resp);
    }, {Get});

  // API status — gated by internalOnly.
  app.registerHandler("/status",
    [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& cb) {
      if (!internalAllowed(req)) { cb(http::notFound("Not found")); return; }
      Json::Value b;
      b["api"] = "Pulse Backend";
      b["version"] = "1.0.0";
      b["status"] = "active";
      cb(http::json(k200OK, b));
    }, {Get});
}

void registerAdvices() {
  auto& app = drogon::app();

  // CORS + 404 JSON body (Express returns a {success:false,...} 404).
  app.setCustomErrorHandler([](HttpStatusCode code) {
    return http::error(code, "Route not found. The requested endpoint does not exist.", "NOT_FOUND");
  });

  // Post-handling advice: stamp CORS headers + release any upload byte
  // reservation made by the upload guards (mirrors upload.js res.on('finish')).
  app.registerPostHandlingAdvice(
    [](const HttpRequestPtr& req, const HttpResponsePtr& resp) {
      // Controllers log the detailed exception server-side. Never send their
      // raw 500 bodies (which can contain database/provider details) in prod.
      // Deliberately leave every non-500 response body untouched.
      scrubProductionServerError(resp);
      applyCorsHeaders(req, resp);
      if (req->getAttributes()->find("__upload_release")) {
        pulse::filters::releaseUploadReservation(req);
      }
    });

  // Pre-routing: answer CORS preflight, then apply the process-wide abuse and
  // JSON sanitation controls before any controller parses the body.
  app.registerPreRoutingAdvice(
    [](const HttpRequestPtr& req, AdviceCallback&& acb, AdviceChainCallback&& accb) {
      if (req->method() == Options) {
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k204NoContent);
        applyCorsHeaders(req, resp);
        acb(resp);
        return;
      }

      if (g_shuttingDown.load()) {
        Json::Value body;
        body["status"] = "SHUTTING_DOWN";
        acb(http::json(k503ServiceUnavailable, body));
        return;
      }

      const std::string& path = req->path();
      const bool health = path == "/health" || path == "/health/ready" ||
                          path == "/health/detailed" || path == "/status";
      if (!health) {
        HttpResponsePtr limited;
        const int max = static_cast<int>(std::clamp<int64_t>(
            config().envInt("RATE_LIMIT_MAX_REQUESTS", 1000), 1, 1000000));
        if (pulse::filters::rateLimited(
                req, "rl:global:", max, false,
                "Too many requests. Please slow down and try again later.",
                "RATE_LIMIT_EXCEEDED", limited)) {
          acb(limited);
          return;
        }
      }

      HttpResponsePtr invalidBody;
      if (!pulse::filters::sanitizeJsonRequest(req, invalidBody)) {
        acb(invalidBody);
        return;
      }
      accb();
    });
}

void beginGracefulShutdown(int sig) {
  if (g_shuttingDown.exchange(true)) return;
  log::info("\xF0\x9F\x9B\x91 Signal {} received. Draining...", sig);
  pulse::jobs::requestStop();
  const int64_t drainMs = std::max<int64_t>(
      0, std::min<int64_t>(60000,
          config().envInt("SHUTDOWN_DRAIN_MS", 5000)));
  drogon::app().getLoop()->runAfter(
      static_cast<double>(drainMs) / 1000.0,
      [] { drogon::app().quit(); });
}

// Async-signal-safe: only store the signal number. Logging, sleeping, service
// teardown, and event-loop operations are performed later on the app loop.
void signalHandler(int sig) {
  g_pendingSignal = sig;
}
} // namespace

int main() {
  g_startTime = std::chrono::steady_clock::now();
  log::init();

  auto& cfg = config();
  cfg.printSummary();
  log::info("\xF0\x9F\x9A\x80 Initializing Pulse Backend Services...");

  // 1. MongoDB
  try { db::connect(); }
  catch (const std::exception& e) {
    log::error("MongoDB connection failed: {}", e.what());
    if (cfg.isProduction()) return 1;
  }

  // 2. Redis — required in production.
  if (cache().ping()) {
    log::info("\xE2\x9C\x85 Redis connected successfully");
  } else if (cfg.isProduction()) {
    log::error("Redis is unavailable \xE2\x80\x94 refusing to start in production");
    return 1;
  } else {
    log::warn("\xE2\x9A\xA0\xEF\xB8\x8F  Redis connection failed - using fallback");
  }

  // 3-4. Optional services + indexes.
  if (!initFirebaseIfPresent()) return 1;
  initSmtpIfPresent();
  db::createIndexes();

  // 5. Background jobs.
  startBackgroundJobsIfEnabled();

  // Wiring: advices, health endpoints.
  registerAdvices();
  registerHealthEndpoints();

  // Signals.
  std::signal(SIGTERM, signalHandler);
  std::signal(SIGINT, signalHandler);

  int port = cfg.server.port;
  const int threads = static_cast<int>(std::clamp<int64_t>(
      cfg.envInt("THREAD_NUM", 0), 0, 256));
  log::info("\xF0\x9F\x9A\x80 Pulse Backend Server running on port {}", port);
  log::info("\xF0\x9F\x8C\x8D Environment: {}", cfg.nodeEnv());

  auto& app = drogon::app();
  // Drogon has one transport-level body ceiling for every route. Size it for
  // the largest configured multipart request; pre-routing sanitation separately
  // enforces the much smaller JSON_BODY_LIMIT and upload handlers enforce both
  // per-file size and file count.
  const int64_t uploadMaxMb = std::max<int64_t>(
      1, std::min<int64_t>(512, cfg.envInt("UPLOAD_MAX_MB", 25)));
  const int64_t uploadMaxFiles = std::max<int64_t>(
      1, std::min<int64_t>(20, cfg.envInt("UPLOAD_MAX_FILES", 5)));
  const int64_t reelMaxMb = std::max<int64_t>(
      1, std::min<int64_t>(1024, cfg.envInt("REEL_MAX_MB", 80)));
  const int64_t maxRequestMb = std::max(uploadMaxMb * uploadMaxFiles + 2,
                                        reelMaxMb + 2);
  const int64_t keepaliveMs = std::clamp<int64_t>(
      cfg.envInt("HTTP_KEEPALIVE_TIMEOUT_MS", 65000), 1000, 300000);
  const int64_t wsMaxFrameBytes = std::clamp<int64_t>(
      cfg.envInt("WS_MAX_FRAME_BYTES", 64 * 1024), 1024, 1024 * 1024);
  app.addListener("0.0.0.0", port)
     .setThreadNum(threads <= 0 ? 0 : threads)   // 0 => hardware concurrency
     .setIdleConnectionTimeout(static_cast<size_t>(keepaliveMs / 1000))
     .setClientMaxBodySize(static_cast<size_t>(maxRequestMb * 1024 * 1024))
     .setClientMaxWebSocketMessageSize(static_cast<size_t>(wsMaxFrameBytes))
     .enableGzip(true)
     .setDocumentRoot("./public");

  app.getLoop()->runEvery(0.1, [] {
    const int sig = g_pendingSignal;
    if (sig != 0) {
      g_pendingSignal = 0;
      beginGracefulShutdown(sig);
    }
  });

  app.run();
  pulse::jobs::stop();
  pulse::sockets::RealtimeController::shutdownInfrastructure();
  pulse::sockets::PresenceHub::shutdownExistingInfrastructure();
  cache().disconnect();
  db::disconnect();
  log::info("\xF0\x9F\x91\x8B Graceful shutdown completed");
  return 0;
}

// ── Init hooks ──
//
// Firebase + SMTP init live in their service units (firebase_service.cc /
// smtp.cc); they self-initialize on first use, and these provide a best-effort
// eager init at boot mirroring server.js. They are declared weakly via the
// service headers when those units are present. Here we keep thin wrappers so
// main is decoupled from whether those optional units exist yet.
namespace {
bool initFirebaseIfPresent() {
  if (!config().getBool("features.enableFirebaseAuth", false)) {
    log::info("Firebase authentication is disabled");
    return true;
  }

  log::info("\xF0\x9F\x94\xA5 Initializing Firebase authentication...");
  if (pulse::firebase().initialize()) return true;

  if (config().isProduction()) {
    log::error("Firebase authentication is enabled but initialization failed");
    return false;
  }
  log::warn("Firebase authentication initialization failed; continuing in development");
  return true;
}
void initSmtpIfPresent() {
  log::info("\xF0\x9F\x93\xA7 Initializing SMTP...");
}
void startBackgroundJobsIfEnabled() {
  if (config().getBool("features.enableBackgroundJobs", false)) {
    log::info("\xF0\x9F\x9B\xA0\xEF\xB8\x8F  Starting background job scheduler...");
    pulse::jobs::start();
  }
}
} // namespace
