// db.cc — MongoDB connection layer implementation. Ports src/config/database.js.
#include "pulse/db.hpp"
#include "pulse/config.hpp"
#include "pulse/logger.hpp"

#include <mongocxx/instance.hpp>
#include <mongocxx/uri.hpp>
#include <mongocxx/exception/exception.hpp>
#include <mongocxx/exception/operation_exception.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/string/to_string.hpp>

#include <memory>
#include <mutex>
#include <atomic>
#include <thread>

namespace pulse::db {

namespace {
std::unique_ptr<mongocxx::instance> g_instance;
std::unique_ptr<mongocxx::pool> g_pool;
std::string g_dbName;
std::atomic<bool> g_connected{false};
std::once_flag g_initFlag;

// Extract the default database name from the URI path (…/pulse?…).
std::string dbNameFromUri(const std::string& uri) {
  auto scheme = uri.find("//");
  size_t start = (scheme == std::string::npos) ? 0 : scheme + 2;
  auto slash = uri.find('/', start);
  if (slash == std::string::npos) return "pulse";
  auto q = uri.find('?', slash);
  std::string name = uri.substr(slash + 1, (q == std::string::npos ? uri.size() : q) - slash - 1);
  return name.empty() ? "pulse" : name;
}
} // namespace

void connect() {
  std::call_once(g_initFlag, [] {
    auto& cfg = config();
    std::string uriStr = cfg.databaseUri();

    // Pool sizing mirrors database.js: a per-container budget divided by worker
    // count, clamped to >= 5; explicit MONGO_OPTIONS_MAX_POOL_SIZE wins.
    unsigned workers = std::max(1u, std::thread::hardware_concurrency());
    long long explicitPool = cfg.envInt("MONGO_OPTIONS_MAX_POOL_SIZE", 0);
    long long budget = cfg.envInt("MONGO_CONTAINER_POOL_BUDGET", 50);
    long long derived = std::max<long long>(5, budget / workers);
    long long maxPool = explicitPool ? explicitPool : derived;
    long long minPool = cfg.envInt("MONGO_OPTIONS_MIN_POOL_SIZE", std::min<long long>(2, maxPool));

    try {
      g_instance = std::make_unique<mongocxx::instance>();

      // Build a URI carrying pool + timeout options.
      std::string fullUri = uriStr;
      std::string sep = (uriStr.find('?') == std::string::npos) ? "?" : "&";
      fullUri += sep + "maxPoolSize=" + std::to_string(maxPool) +
                 "&minPoolSize=" + std::to_string(minPool) +
                 "&serverSelectionTimeoutMS=" + std::to_string(cfg.database.serverSelectionTimeoutMs) +
                 "&socketTimeoutMS=45000&connectTimeoutMS=10000&retryWrites=true&retryReads=true";

      mongocxx::uri uri{fullUri};
      g_dbName = dbNameFromUri(uriStr);
      g_pool = std::make_unique<mongocxx::pool>(uri);

      // Verify connectivity with a ping.
      auto client = g_pool->acquire();
      (*client)[g_dbName].run_command(
          bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp("ping", 1)));

      g_connected = true;
      pulse::log::info("\xE2\x9C\x85 Connected to MongoDB successfully (db={}, maxPoolSize={})", g_dbName, maxPool);
    } catch (const std::exception& e) {
      pulse::log::error("\xE2\x9D\x8C MongoDB connection failed: {}", e.what());
      if (config().isProduction()) std::exit(1);
      throw;
    }
  });
}

void disconnect() {
  // The pool/instance tear down at process exit; flip the flag for health.
  g_connected = false;
  pulse::log::info("\xF0\x9F\x91\x8B MongoDB connection closed");
}

bool isConnected() { return g_connected.load(); }

bool isHealthy() {
  if (!g_connected || !g_pool) return false;
  try {
    auto client = g_pool->acquire();
    (*client)[g_dbName].run_command(
        bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp("ping", 1)));
    return true;
  } catch (...) {
    return false;
  }
}

std::string connectionStats() {
  if (!g_connected) return "{\"connected\":false}";
  return std::string("{\"connected\":true,\"name\":\"") + g_dbName + "\"}";
}

ClientHandle::ClientHandle() : entry_(g_pool->acquire()) {}
mongocxx::client& ClientHandle::client() { return *entry_; }
mongocxx::database ClientHandle::database() { return (*entry_)[g_dbName]; }
mongocxx::collection ClientHandle::collection(const std::string& name) { return (*entry_)[g_dbName][name]; }

mongocxx::collection collection(const std::string& name) {
  // Borrow a client for the lifetime of this call's collection use. Callers
  // that need multi-step ops should hold a ClientHandle.
  thread_local mongocxx::pool::entry tlEntry = g_pool->acquire();
  return (*tlEntry)[g_dbName][name];
}

void safeCreateIndex(mongocxx::collection& col,
                     bsoncxx::document::view_or_value keys,
                     bsoncxx::document::view_or_value opts) {
  try {
    col.create_index(std::move(keys), std::move(opts));
  } catch (const mongocxx::operation_exception& e) {
    // Tolerate the "an equivalent index already exists" family of errors, which
    // happen when the index was first created by the Node backend (often with a
    // different/auto-generated name or background:true). Codes:
    //   85 IndexOptionsConflict, 86 IndexKeySpecsConflict, 68 IndexAlreadyExists.
    int code = e.code().value();
    std::string what = e.what();
    bool benign = code == 85 || code == 86 || code == 68 ||
                  what.find("already exists") != std::string::npos ||
                  what.find("same name") != std::string::npos;
    if (benign) {
      pulse::log::debug("[indexes] skipping pre-existing index on '{}': {}", col.name(), what);
    } else {
      pulse::log::warn("[indexes] create_index failed on '{}': {}", col.name(), what);
    }
  } catch (const std::exception& e) {
    pulse::log::warn("[indexes] create_index error on '{}': {}", col.name(), e.what());
  }
}

void createIndexes() {
  pulse::log::info("\xF0\x9F\x93\x9D Creating database indexes...");
  // Index definitions are applied by scripts/create_indexes (and per-model
  // ensureIndexes()); see src/db/indexes.cc which centralizes them.
  extern void ensureAllIndexes();
  try { ensureAllIndexes(); }
  catch (const std::exception& e) { pulse::log::error("Index creation error: {}", e.what()); }
}

} // namespace pulse::db
