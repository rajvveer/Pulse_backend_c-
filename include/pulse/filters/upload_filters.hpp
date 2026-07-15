// upload_filters.hpp — upload guards, port of src/middlewares/upload.js.
//
// multer's per-file limit + the process-wide in-flight-bytes semaphore. Drogon
// buffers the multipart body, so the guard checks both Content-Length and the
// actual buffered size (protecting against a missing/understated header):
//   - 413 PAYLOAD_TOO_LARGE when either size exceeds the per-request ceiling
//   - 503 UPLOAD_CAPACITY when accepting it would exceed the process-wide
//     in-flight byte budget (load-shed rather than risk OOM)
// The reservation is released when the request completes.
//
// UploadGuard   — image ceiling (UPLOAD_MAX_MB * UPLOAD_MAX_FILES + 2)
// ReelGuard     — video ceiling (REEL_MAX_MB + 2)
// Both share the same MAX_INFLIGHT budget (UPLOAD_MAX_INFLIGHT_MB).
#pragma once
#include <drogon/HttpFilter.h>

namespace pulse::filters {

using namespace drogon;

class UploadGuard : public HttpFilter<UploadGuard> {
public:
  void doFilter(const HttpRequestPtr&, FilterCallback&&, FilterChainCallback&&) override;
};

class ReelGuard : public HttpFilter<ReelGuard> {
public:
  void doFilter(const HttpRequestPtr&, FilterCallback&&, FilterChainCallback&&) override;
};

// Release a request's in-flight upload byte reservation. Called from the
// post-handling advice once the response is sent.
void releaseUploadReservation(const HttpRequestPtr& req);

} // namespace pulse::filters
