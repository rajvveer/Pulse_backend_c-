#pragma once

#include "pulse/config.hpp"

#include <algorithm>
#include <cstdint>

namespace pulse::services {

// Drogon's timeout argument is seconds. Keep every outbound provider call
// bounded so a stalled upstream cannot pin an IO/worker thread indefinitely.
inline double outboundHttpTimeoutSeconds() {
  const int64_t ms = std::max<int64_t>(
      250, std::min<int64_t>(120000,
          pulse::config().envInt("OUTBOUND_HTTP_TIMEOUT_MS", 10000)));
  return static_cast<double>(ms) / 1000.0;
}

}  // namespace pulse::services
