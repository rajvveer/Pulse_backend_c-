// config.cc — implementation of the Config singleton. Ports src/config/index.js.
#include "pulse/config.hpp"

#include <cstdlib>
#include <charconv>
#include <cctype>
#include <fstream>
#include <sstream>
#include <iostream>
#include <unordered_map>
#include <algorithm>
#include <system_error>

namespace pulse {

namespace {
// Process-wide env overlay populated from .env (real getenv still wins if set).
std::unordered_map<std::string, std::string>& dotenvStore() {
  static std::unordered_map<std::string, std::string> s;
  return s;
}

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

std::vector<std::string> split(const std::string& s, char delim) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, delim)) out.push_back(trim(item));
  return out;
}

bool looksLikePlaceholderSecret(const std::string& value) {
  std::string normalized = trim(value);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  static constexpr const char* kPrefixes[] = {
      "change_me", "changeme", "replace_me", "replace-me", "your_",
      "your-", "example", "placeholder", "dev-only", "default-", "test-"};
  for (const char* prefix : kPrefixes) {
    if (normalized.rfind(prefix, 0) == 0) return true;
  }
  return normalized == "secret" || normalized == "password" ||
         normalized == "jwt_secret" || normalized == "session_secret";
}
} // namespace

std::string Config::env(const std::string& key, const std::string& def) const {
  if (const char* v = std::getenv(key.c_str())) return std::string(v);
  auto it = dotenvStore().find(key);
  if (it != dotenvStore().end()) return it->second;
  return def;
}

int64_t Config::envInt(const std::string& key, int64_t def) const {
  const std::string v = env(key);
  if (v.empty()) return def;
  int64_t value = 0;
  const char* first = v.data();
  const char* last = first + v.size();
  const auto parsed = std::from_chars(first, last, value, 10);
  if (parsed.ec != std::errc{} || parsed.ptr != last) return def;
  return value;
}

bool Config::envBool(const std::string& key, bool def) const {
  std::string v = env(key);
  if (v.empty()) return def;
  return v == "true" || v == "1" || v == "yes";
}

void Config::loadDotEnv() {
  // Honor a DOTENV_PATH override, else read ./.env if present.
  std::string path = ".env";
  if (const char* p = std::getenv("DOTENV_PATH")) path = p;
  std::ifstream f(path);
  if (!f.is_open()) return;
  std::string line;
  while (std::getline(f, line)) {
    std::string t = trim(line);
    if (t.empty() || t[0] == '#') continue;
    auto eq = t.find('=');
    if (eq == std::string::npos) continue;
    std::string key = trim(t.substr(0, eq));
    std::string val = trim(t.substr(eq + 1));
    // Strip surrounding quotes.
    if (val.size() >= 2 && ((val.front() == '"' && val.back() == '"') ||
                            (val.front() == '\'' && val.back() == '\''))) {
      val = val.substr(1, val.size() - 2);
    }
    // Don't clobber a real environment variable.
    if (!std::getenv(key.c_str())) dotenvStore()[key] = val;
  }
}

void Config::validateRequired() {
  std::vector<std::string> required = {"JWT_SECRET", "JWT_REFRESH_SECRET", "TEMP_JWT_SECRET"};
  if (isProduction()) { required.push_back("MONGO_URI"); required.push_back("SESSION_SECRET"); }

  std::vector<std::string> missing;
  for (const auto& k : required) if (env(k).empty()) missing.push_back(k);

  if (!missing.empty()) {
    std::cerr << "\xE2\x9D\x8C Missing required environment variables:\n";
    for (const auto& k : missing) std::cerr << "   - " << k << "\n";
    if (isProduction()) std::exit(1);
    else std::cerr << "\xE2\x9A\xA0\xEF\xB8\x8F  Continuing in development mode with missing env vars\n";
  }

  if (isProduction() && env("REDIS_URL").empty() && env("REDIS_HOST").empty()) {
    std::cerr << "\xE2\x9D\x8C REDIS_URL or REDIS_HOST must be set in production\n";
    std::exit(1);
  }

  if (isProduction()) {
    // These values authenticate sessions or sign bearer credentials. A short
    // value (or a deployment-template placeholder) is equivalent to shipping a
    // known key, so refuse to boot instead of silently weakening production.
    const std::vector<std::string> strongSecrets = {
        "JWT_SECRET", "JWT_REFRESH_SECRET", "TEMP_JWT_SECRET", "SESSION_SECRET"};
    std::vector<std::string> weak;
    for (const auto& key : strongSecrets) {
      const std::string value = env(key);
      if (!value.empty() && (value.size() < 32 || looksLikePlaceholderSecret(value))) {
        weak.push_back(key);
      }
    }
    for (const char* key : {"INTERNAL_STATUS_KEY", "ADMIN_SETUP_TOKEN"}) {
      const std::string value = env(key);
      if (!value.empty() && (value.size() < 32 || looksLikePlaceholderSecret(value))) {
        weak.emplace_back(key);
      }
    }
    if (!weak.empty()) {
      std::cerr << "\xE2\x9D\x8C Production secrets must be at least 32 characters and not placeholders:\n";
      for (const auto& key : weak) std::cerr << "   - " << key << "\n";
      std::exit(1);
    }
  }
}

void Config::build() {
  server.port        = static_cast<int>(std::clamp<int64_t>(envInt("PORT", 3000), 1, 65535));
  server.nodeEnv     = nodeEnv_;
  server.apiVersion  = env("API_VERSION", "v1");
  server.serverUrl   = env("SERVER_URL", "http://localhost:3000");
  server.frontendUrl = env("FRONTEND_URL", "http://localhost:3000");

  database.mongoUri     = env("MONGO_URI", "mongodb://localhost:27017/pulse");
  database.mongoTestUri = env("MONGO_TEST_URI", "mongodb://localhost:27017/pulse_test");
  database.maxPoolSize  = static_cast<int>(std::clamp<int64_t>(
      envInt("MONGO_OPTIONS_MAX_POOL_SIZE", 50), 1, 1000));
  database.minPoolSize  = static_cast<int>(std::clamp<int64_t>(
      envInt("MONGO_OPTIONS_MIN_POOL_SIZE", 5), 0, database.maxPoolSize));
  database.serverSelectionTimeoutMs = static_cast<int>(std::clamp<int64_t>(
      envInt("MONGO_OPTIONS_SERVER_SELECTION_TIMEOUT_MS", 5000), 100, 300000));

  jwt.secret           = env("JWT_SECRET");
  jwt.refreshSecret    = env("JWT_REFRESH_SECRET");
  jwt.tempSecret       = env("TEMP_JWT_SECRET");
  jwt.expiresIn        = env("JWT_EXPIRES_IN", "15m");
  jwt.refreshExpiresIn = env("JWT_REFRESH_EXPIRES_IN", "7d");

  security.bcryptSaltRounds = static_cast<int>(std::clamp<int64_t>(
      envInt("BCRYPT_SALT_ROUNDS", 12), 4, 16));
  security.sessionSecret    = env("SESSION_SECRET", "dev-only-session-secret");

  redis.url        = env("REDIS_URL");
  redis.host       = env("REDIS_HOST", "localhost");
  redis.port       = static_cast<int>(std::clamp<int64_t>(
      envInt("REDIS_PORT", 6379), 1, 65535));
  redis.password   = env("REDIS_PASSWORD");
  redis.maxRetries = static_cast<int>(std::clamp<int64_t>(
      envInt("REDIS_MAX_RETRIES", 3), 0, 100));

  rateLimit.windowMs = std::clamp<int64_t>(
      envInt("RATE_LIMIT_WINDOW_MS", 900000), 1000, 24LL * 60 * 60 * 1000);
  rateLimit.maxRequests = static_cast<int>(std::clamp<int64_t>(
      envInt("RATE_LIMIT_MAX_REQUESTS", 100), 1, 1000000));
  rateLimit.maxRequestsPerIp = static_cast<int>(std::clamp<int64_t>(
      envInt("RATE_LIMIT_MAX_REQUESTS_PER_IP", 1000), 1, 1000000));

  std::string co = env("CORS_ORIGIN");
  cors.origin = co.empty() ? std::vector<std::string>{"http://localhost:3000"} : split(co, ',');
  cors.credentials = envBool("CORS_CREDENTIALS", false);
}

Config::Config() {
  loadDotEnv();
  const char* ne = std::getenv("NODE_ENV");
  nodeEnv_ = ne ? ne : env("NODE_ENV", "development");
  validateRequired();
  build();
}

Config& Config::instance() {
  static Config c;
  return c;
}

std::string Config::databaseUri() const {
  return isTest() ? database.mongoTestUri : database.mongoUri;
}

// dotted-path getters — cover the paths the JS code referenced.
std::string Config::getString(const std::string& path, const std::string& def) const {
  if (path == "server.port") return std::to_string(server.port);
  if (path == "server.nodeEnv") return server.nodeEnv;
  if (path == "server.serverUrl") return server.serverUrl;
  if (path == "server.frontendUrl") return server.frontendUrl;
  if (path == "server.apiVersion") return server.apiVersion;
  if (path == "jwt.secret") return jwt.secret;
  if (path == "jwt.expiresIn") return jwt.expiresIn;
  if (path == "redis.host") return redis.host;
  if (path == "redis.url") return redis.url;
  return def;
}

int64_t Config::getInt(const std::string& path, int64_t def) const {
  if (path == "server.port") return server.port;
  if (path == "redis.port") return redis.port;
  if (path == "rateLimit.windowMs") return rateLimit.windowMs;
  if (path == "rateLimit.maxRequests") return rateLimit.maxRequests;
  return def;
}

bool Config::getBool(const std::string& path, bool def) const {
  if (path == "features.enableBackgroundJobs") return envBool("ENABLE_BACKGROUND_JOBS", false);
  if (path == "features.enableFirebaseAuth") return envBool("ENABLE_FIREBASE_AUTH", false);
  if (path == "features.enableGoogleLogin") return envBool("ENABLE_GOOGLE_LOGIN", false);
  if (path == "features.enablePhoneVerification") return envBool("ENABLE_PHONE_VERIFICATION", false);
  if (path == "features.enableEmailVerification") return envBool("ENABLE_EMAIL_VERIFICATION", false);
  if (path == "features.enableTwoFactorAuth") return envBool("ENABLE_TWO_FACTOR_AUTH", false);
  if (path == "cors.credentials") return cors.credentials;
  return def;
}

std::vector<std::string> Config::getList(const std::string& path) const {
  if (path == "cors.origin") return cors.origin;
  return {};
}

void Config::printSummary() const {
  std::string dbMasked = databaseUri();
  auto at = dbMasked.find('@');
  if (at != std::string::npos) {
    auto proto = dbMasked.find("//");
    if (proto != std::string::npos) dbMasked = dbMasked.substr(0, proto + 2) + "***@" + dbMasked.substr(at + 1);
  }
  std::cout << "\xE2\x9A\x99\xEF\xB8\x8F  Configuration Summary:\n";
  std::cout << "   \xF0\x9F\x8C\x8D Environment: " << nodeEnv_ << "\n";
  std::cout << "   \xF0\x9F\x9A\x80 Server: " << server.serverUrl << "\n";
  std::cout << "   \xF0\x9F\x93\x9A Database: " << dbMasked << "\n";
  std::cout << "   \xF0\x9F\x94\xB4 Redis: " << redis.host << ":" << redis.port << "\n";
}

} // namespace pulse
