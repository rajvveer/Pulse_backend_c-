// config.hpp — central configuration, ported 1:1 from src/config/index.js.
//
// A process-wide singleton that reads environment variables (loaded from a .env
// file at startup via dotenv-style parsing), applies the same defaults as the
// Node config, validates required secrets, and exposes typed getters plus the
// dotted-path get() the JS code used (config.get('server.port')).
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace pulse {

class Config {
public:
  // Singleton accessor. First call constructs: loads .env, validates, builds.
  static Config& instance();

  // ── dotted-path getters (mirror JS config.get('a.b.c')) ──
  std::string getString(const std::string& path, const std::string& def = "") const;
  int64_t     getInt(const std::string& path, int64_t def = 0) const;
  bool        getBool(const std::string& path, bool def = false) const;
  std::vector<std::string> getList(const std::string& path) const;

  // ── environment helpers ──
  bool isDevelopment() const { return nodeEnv_ == "development"; }
  bool isProduction()  const { return nodeEnv_ == "production"; }
  bool isTest()        const { return nodeEnv_ == "test"; }
  const std::string& nodeEnv() const { return nodeEnv_; }

  std::string databaseUri() const;   // test uri when NODE_ENV=test
  void printSummary() const;

  // ── Strongly-typed sections (the hot-path values) ──
  struct Server { int port; std::string nodeEnv, apiVersion, serverUrl, frontendUrl; } server;
  struct Database { std::string mongoUri, mongoTestUri; int maxPoolSize, minPoolSize, serverSelectionTimeoutMs; } database;
  struct Jwt { std::string secret, refreshSecret, tempSecret, expiresIn, refreshExpiresIn; } jwt;
  struct Security { int bcryptSaltRounds; std::string sessionSecret; } security;
  struct Redis { std::string url, host, password; int port, maxRetries; } redis;
  struct RateLimit { int64_t windowMs; int maxRequests, maxRequestsPerIp; } rateLimit;
  struct Cors { std::vector<std::string> origin; bool credentials; } cors;

  // Raw env access (with default) for everything not modeled above.
  std::string env(const std::string& key, const std::string& def = "") const;
  int64_t envInt(const std::string& key, int64_t def) const;
  bool envBool(const std::string& key, bool def = false) const;

private:
  Config();
  void loadDotEnv();
  void validateRequired();
  void build();

  std::string nodeEnv_;
};

// Convenience free function matching the JS `config` singleton usage.
inline Config& config() { return Config::instance(); }

} // namespace pulse
