// logger.hpp — thin logging facade, ported from src/utils/logger.js (winston).
// Backed by spdlog. Level comes from LOG_LEVEL (info default). The JS logger
// exposed .info/.warn/.error/.debug; we keep the same call sites.
#pragma once
#include <string>
#include <spdlog/spdlog.h>

namespace pulse::log {

void init();  // configure level/format from env; call once at startup.

template <typename... Args> inline void info(Args&&... a)  { spdlog::info(std::forward<Args>(a)...); }
template <typename... Args> inline void warn(Args&&... a)  { spdlog::warn(std::forward<Args>(a)...); }
template <typename... Args> inline void error(Args&&... a) { spdlog::error(std::forward<Args>(a)...); }
template <typename... Args> inline void debug(Args&&... a) { spdlog::debug(std::forward<Args>(a)...); }

} // namespace pulse::log
