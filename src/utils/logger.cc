// logger.cc — configure spdlog from env, mirroring src/utils/logger.js levels.
#include "pulse/logger.hpp"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <cstdlib>
#include <string>

namespace pulse::log {

void init() {
  // Level: LOG_LEVEL env, else warn in production / info otherwise (matches JS).
  std::string lvl;
  if (const char* v = std::getenv("LOG_LEVEL")) lvl = v;
  if (lvl.empty()) {
    const char* ne = std::getenv("NODE_ENV");
    lvl = (ne && std::string(ne) == "production") ? "warn" : "info";
  }

  spdlog::level::level_enum level = spdlog::level::info;
  if (lvl == "debug") level = spdlog::level::debug;
  else if (lvl == "info") level = spdlog::level::info;
  else if (lvl == "warn" || lvl == "warning") level = spdlog::level::warn;
  else if (lvl == "error") level = spdlog::level::err;

  spdlog::set_level(level);
  // "HH:MM:SS [level]: message" — close to the winston console format.
  spdlog::set_pattern("%H:%M:%S [%^%l%$]: %v");
}

} // namespace pulse::log
