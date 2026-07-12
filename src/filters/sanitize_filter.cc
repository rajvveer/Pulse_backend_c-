// sanitize_filter.cc — XSS deep-walk sanitization. Ports src/middlewares/sanitize.js.
#include "pulse/filters/sanitize_filter.hpp"
#include "pulse/config.hpp"

namespace pulse::sanitize {

namespace {
int maxDepth()  { return (int)pulse::config().envInt("SANITIZE_MAX_DEPTH", 12); }
int maxNodes()  { return (int)pulse::config().envInt("SANITIZE_MAX_NODES", 5000); }
int maxString() { return (int)pulse::config().envInt("SANITIZE_MAX_STRING", 50000); }

// The js `xss` library, by default, HTML-escapes tags and strips dangerous
// attributes. A faithful, dependency-free approximation: entity-encode the five
// HTML-significant characters so any markup is rendered inert when echoed back.
std::string escapeHtml(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (char c : in) {
    switch (c) {
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '&': out += "&amp;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c;
    }
  }
  return out;
}

Json::Value walk(const Json::Value& v, int depth, int& count) {
  if (depth > maxDepth() || count > maxNodes()) return v;
  ++count;
  switch (v.type()) {
    case Json::stringValue: {
      const std::string& s = v.asString();
      if ((int)s.size() > maxString()) return Json::Value(s.substr(0, maxString()));
      return Json::Value(escapeHtml(s));
    }
    case Json::arrayValue: {
      Json::Value out(Json::arrayValue);
      for (const auto& e : v) out.append(walk(e, depth + 1, count));
      return out;
    }
    case Json::objectValue: {
      Json::Value out(Json::objectValue);
      for (const auto& k : v.getMemberNames()) {
        if (count > maxNodes()) { out[k] = v[k]; continue; }
        out[k] = walk(v[k], depth + 1, count);
      }
      return out;
    }
    default:
      return v;
  }
}
} // namespace

std::string clean(const std::string& s) {
  if ((int)s.size() > maxString()) return escapeHtml(s.substr(0, maxString()));
  return escapeHtml(s);
}

Json::Value deepSanitize(const Json::Value& v) {
  int count = 0;
  return walk(v, 0, count);
}

} // namespace pulse::sanitize

namespace pulse::filters {

void SanitizeFilter::doFilter(const HttpRequestPtr& req, FilterCallback&& fcb, FilterChainCallback&& fccb) {
  // Only JSON bodies are sanitized (multipart/upload bodies pass through, like
  // the JS middleware which only touched req.body objects). The sanitized body
  // is stored on the request attributes under "sanitizedBody" for controllers
  // that opt to read it; the original parse remains available too.
  auto body = req->getJsonObject();
  if (body && (body->isObject() || body->isArray())) {
    Json::Value cleaned = pulse::sanitize::deepSanitize(*body);
    req->getAttributes()->insert("sanitizedBody", cleaned);
  }
  return fccb();
}

} // namespace pulse::filters
