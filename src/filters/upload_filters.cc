// upload_filters.cc — upload guards. Ports the makeUploadGuard semaphore in upload.js.
#include "pulse/filters/upload_filters.hpp"
#include "pulse/config.hpp"
#include "pulse/http_response.hpp"

#include <atomic>
#include <cstdint>

namespace pulse::filters {

namespace {
// Process-wide in-flight upload byte budget, shared by all guards (matches the
// single `inflightBytes` counter in upload.js).
std::atomic<int64_t> g_inflightBytes{0};

int64_t mb() { return 1024 * 1024; }
int64_t maxInflightBytes() {
  return pulse::config().envInt("UPLOAD_MAX_INFLIGHT_MB", 256) * mb();
}

int64_t declaredLength(const HttpRequestPtr& req) {
  std::string cl = req->getHeader("content-length");
  if (cl.empty()) return 0;
  try { return std::stoll(cl); } catch (...) { return 0; }
}

// Shared guard body: reserve `declared` bytes against the budget; release when
// the request finishes. Returns true (and fills resp) if the request is rejected.
bool guard(const HttpRequestPtr& req, int64_t maxRequestBytes, HttpResponsePtr& resp) {
  int64_t declared = declaredLength(req);

  if (declared > maxRequestBytes) {
    resp = pulse::http::error(drogon::k413RequestEntityTooLarge, "Upload too large.", "PAYLOAD_TOO_LARGE");
    return true;
  }

  if (declared > 0) {
    int64_t budget = maxInflightBytes();
    int64_t prev = g_inflightBytes.fetch_add(declared);
    if (prev + declared > budget) {
      g_inflightBytes.fetch_sub(declared); // roll back the reservation
      resp = pulse::http::error(drogon::k503ServiceUnavailable,
                                "Server is busy handling uploads. Please retry in a moment.",
                                "UPLOAD_CAPACITY");
      return true;
    }
    // Release the reservation when the response is sent (Drogon advice runs the
    // post-handling chain). We register a one-shot release on the request.
    req->getAttributes()->insert("__upload_reserved", (double)declared);
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
  int64_t maxFileMb = pulse::config().envInt("UPLOAD_MAX_MB", 25);
  int64_t maxFiles  = pulse::config().envInt("UPLOAD_MAX_FILES", 5);
  int64_t maxReqBytes = (maxFileMb * maxFiles + 2) * mb();
  HttpResponsePtr resp;
  if (guard(req, maxReqBytes, resp)) return fcb(resp);
  // Release reservation after the handler responds.
  req->getAttributes()->insert("__upload_release", true);
  return fccb();
}

void ReelGuard::doFilter(const HttpRequestPtr& req, FilterCallback&& fcb, FilterChainCallback&& fccb) {
  int64_t reelMaxMb = pulse::config().envInt("REEL_MAX_MB", 80);
  int64_t maxReqBytes = (reelMaxMb + 2) * mb();
  HttpResponsePtr resp;
  if (guard(req, maxReqBytes, resp)) return fcb(resp);
  req->getAttributes()->insert("__upload_release", true);
  return fccb();
}

// Free hook invoked from main's post-handling advice to release reservations.
void releaseUploadReservation(const HttpRequestPtr& req) { releaseIfReserved(req); }

} // namespace pulse::filters
