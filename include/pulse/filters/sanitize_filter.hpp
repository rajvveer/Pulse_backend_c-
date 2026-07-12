// sanitize_filter.hpp — XSS sanitization filter, ports src/middlewares/sanitize.js.
//
// Recursively cleans every string value in the JSON request body, bounded by
// depth / node-count / string-length caps to keep the cost bounded against a
// pathological payload. The cleaned body is stored back on the request so
// controllers read sanitized input. Also provides the standalone deepSanitize
// used by services that need to clean strings.
#pragma once
#include <drogon/HttpFilter.h>
#include <json/json.h>
#include <string>

namespace pulse::sanitize {

// Escape HTML-significant chars in a single string (the xss() analogue: encodes
// <, >, ", ', & and strips obvious script vectors). Bounded by maxStringLen.
std::string clean(const std::string& s);

// Recursively sanitize a JSON value, bounded by MAX_DEPTH / MAX_NODES /
// MAX_STRING from env (defaults 12 / 5000 / 50000).
Json::Value deepSanitize(const Json::Value& v);

} // namespace pulse::sanitize

namespace pulse::filters {
using namespace drogon;

class SanitizeFilter : public HttpFilter<SanitizeFilter> {
public:
  void doFilter(const HttpRequestPtr&, FilterCallback&&, FilterChainCallback&&) override;
};

} // namespace pulse::filters
