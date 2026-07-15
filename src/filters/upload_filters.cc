// upload_filters.cc — upload guards. Ports the makeUploadGuard semaphore in upload.js.
#include "pulse/filters/upload_filters.hpp"
#include "pulse/config.hpp"
#include "pulse/http_response.hpp"

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <limits>

namespace pulse::filters {

namespace {
// Process-wide in-flight upload byte budget, shared by all guards (matches the
// single `inflightBytes` counter in upload.js).
std::atomic<int64_t> g_inflightBytes{0};

int64_t mb() { return 1024 * 1024; }
int64_t maxInflightBytes() {
  const int64_t maxMb = std::clamp<int64_t>(
      pulse::config().envInt("UPLOAD_MAX_INFLIGHT_MB", 256), 1, 16384);
  return maxMb * mb();
}

int64_t declaredLength(const HttpRequestPtr& req, bool& valid) {
  std::string cl = req->getHeader("content-length");
  if (cl.empty()) return 0;
  try {
    size_t consumed = 0;
    const int64_t value = std::stoll(cl, &consumed);
    valid = consumed == cl.size() && value >= 0;
    return valid ? value : 0;
  } catch (...) {
    valid = false;
    return 0;
  }
}

// Shared guard body: reserve the larger of the declared and actually buffered
// body sizes against the budget; release when
// the request finishes. Returns true (and fills resp) if the request is rejected.
bool guard(const HttpRequestPtr& req, int64_t maxRequestBytes, HttpResponsePtr& resp) {
  bool validLength = true;
  const int64_t declared = declaredLength(req, validLength);

  if (!validLength) {
    resp = pulse::http::error(drogon::k400BadRequest,
                              "Invalid Content-Length header.",
                              "INVALID_CONTENT_LENGTH");
    return true;
  }

  const size_t actualSize = req->body().size();
  if (actualSize > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
    resp = pulse::http::error(drogon::k413RequestEntityTooLarge,
                              "Upload too large.", "PAYLOAD_TOO_LARGE");
    return true;
  }
  const int64_t actual = static_cast<int64_t>(actualSize);
  const int64_t reserved = std::max(declared, actual);

  if (reserved > maxRequestBytes) {
    resp = pulse::http::error(drogon::k413RequestEntityTooLarge, "Upload too large.", "PAYLOAD_TOO_LARGE");
    return true;
  }

  if (reserved > 0) {
    const int64_t budget = maxInflightBytes();
    int64_t current = g_inflightBytes.load(std::memory_order_relaxed);
    for (;;) {
      if (reserved > budget || current > budget - reserved) {
        resp = pulse::http::error(drogon::k503ServiceUnavailable,
                                  "Server is busy handling uploads. Please retry in a moment.",
                                  "UPLOAD_CAPACITY");
        return true;
      }
      if (g_inflightBytes.compare_exchange_weak(
              current, current + reserved, std::memory_order_acq_rel,
              std::memory_order_relaxed)) {
        break;
      }
    }
    // Release the reservation when the response is sent (Drogon advice runs the
    // post-handling chain). We register a one-shot release on the request.
    req->getAttributes()->insert("__upload_reserved", static_cast<double>(reserved));
  }
  return false;
}

void releaseIfReserved(const HttpRequestPtr& req) {
  auto attrs = req->getAttributes();
  if (attrs->find("__upload_reserved")) {
    int64_t bytes = (int64_t)attrs->get<double>("__upload_reserved");
    g_inflightBytes.fetch_sub(bytes);
    attrs->erase("__upload_reserved");
  }
}
} // namespace

void UploadGuard::doFilter(const HttpRequestPtr& req, FilterCallback&& fcb, FilterChainCallback&& fccb) {
  int64_t maxFileMb = std::max<int64_t>(1, std::min<int64_t>(512,
      pulse::config().envInt("UPLOAD_MAX_MB", 25)));
  int64_t maxFiles  = std::max<int64_t>(1, std::min<int64_t>(20,
      pulse::config().envInt("UPLOAD_MAX_FILES", 5)));
  int64_t maxReqBytes = (maxFileMb * maxFiles + 2) * mb();
  HttpResponsePtr resp;
  if (guard(req, maxReqBytes, resp)) return fcb(resp);
  // Release reservation after the handler responds.
  req->getAttributes()->insert("__upload_release", true);
  return fccb();
}

void ReelGuard::doFilter(const HttpRequestPtr& req, FilterCallback&& fcb, FilterChainCallback&& fccb) {
  int64_t reelMaxMb = std::max<int64_t>(1, std::min<int64_t>(1024,
      pulse::config().envInt("REEL_MAX_MB", 80)));
  int64_t maxReqBytes = (reelMaxMb + 2) * mb();
  HttpResponsePtr resp;
  if (guard(req, maxReqBytes, resp)) return fcb(resp);
  req->getAttributes()->insert("__upload_release", true);
  return fccb();
}

// Free hook invoked from main's post-handling advice to release reservations.
void releaseUploadReservation(const HttpRequestPtr& req) { releaseIfReserved(req); }

} // namespace pulse::filters
