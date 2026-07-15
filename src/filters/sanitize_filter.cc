// sanitize_filter.cc — XSS deep-walk sanitization. Ports src/middlewares/sanitize.js.
#include "pulse/filters/sanitize_filter.hpp"
#include "pulse/config.hpp"
#include "pulse/http_response.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <stdexcept>

namespace pulse::sanitize {

namespace {
int maxDepth() {
  return static_cast<int>(std::clamp<int64_t>(
      pulse::config().envInt("SANITIZE_MAX_DEPTH", 12), 1, 64));
}
int maxNodes() {
  return static_cast<int>(std::clamp<int64_t>(
      pulse::config().envInt("SANITIZE_MAX_NODES", 5000), 1, 100000));
}
int maxString() {
  return static_cast<int>(std::clamp<int64_t>(
      pulse::config().envInt("SANITIZE_MAX_STRING", 50000), 1, 1024 * 1024));
}

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

bool hasSuffix(const std::string& value, const std::string& suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool isOpaqueSemanticField(const std::string& key) {
  if (key == "password" || key == "currentPassword" ||
         key == "newPassword" || key == "confirmPassword" ||
         key == "token" || key == "accessToken" ||
         key == "refreshToken" || key == "idToken" || key == "otp") {
    return true;
  }

  // IDs, verification/referral codes, addresses and signed hashes are opaque
  // protocol values. HTML entity encoding them changes their identity.
  if (key == "id" || key == "_id" || key == "uid" || key == "identifier" ||
      key == "email" || key == "phone" || key == "phoneNumber" ||
      key == "hash" || key == "signature" || key == "authorization" ||
      hasSuffix(key, "Id") || hasSuffix(key, "Ids") ||
      hasSuffix(key, "ID") || hasSuffix(key, "IDs") ||
      hasSuffix(key, "Code")) {
    return true;
  }

  std::string lower = key;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return lower.find("url") != std::string::npos || hasSuffix(lower, "uri") ||
         lower == "avatar" || lower == "thumbnail" || lower == "media" ||
         lower == "image" || lower == "video" || lower == "audio" ||
         lower == "file" || lower == "filename" || lower == "path" ||
         lower == "mimetype" || lower == "mediatype" ||
         lower == "contenttype";
}

Json::Value walk(const Json::Value& v, int depth, int& count,
                 bool preserveOpaqueString = false) {
  if (depth > maxDepth()) throw std::length_error("JSON nesting is too deep");
  ++count;
  if (count > maxNodes()) throw std::length_error("JSON contains too many values");
  switch (v.type()) {
    case Json::stringValue: {
      const std::string& s = v.asString();
      if ((int)s.size() > maxString())
        throw std::length_error("JSON string is too long");
      return Json::Value(preserveOpaqueString ? s : escapeHtml(s));
    }
    case Json::arrayValue: {
      Json::Value out(Json::arrayValue);
      for (const auto& e : v)
        out.append(walk(e, depth + 1, count, preserveOpaqueString));
      return out;
    }
    case Json::objectValue: {
      Json::Value out(Json::objectValue);
      for (const auto& k : v.getMemberNames()) {
        // Do not inherit an outer object's semantic classification: a media
        // object may also contain a human-authored caption. Arrays do inherit
        // it so fields such as recipientIds/urls preserve each string value.
        out[k] = walk(v[k], depth + 1, count, isOpaqueSemanticField(k));
      }
      return out;
    }
    default:
      return v;
  }
}
} // namespace

std::string clean(const std::string& s) {
  if ((int)s.size() > maxString())
    throw std::length_error("String is too long");
  return escapeHtml(s);
}

Json::Value deepSanitize(const Json::Value& v) {
  int count = 0;
  return walk(v, 0, count, false);
}

} // namespace pulse::sanitize

namespace pulse::filters {

bool sanitizeJsonRequest(const HttpRequestPtr& req, HttpResponsePtr& response) {
  std::string contentType = req->getHeader("content-type");
  std::transform(contentType.begin(), contentType.end(), contentType.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const auto semicolon = contentType.find(';');
  if (semicolon != std::string::npos) contentType.resize(semicolon);
  const auto first = contentType.find_first_not_of(" \t");
  const auto last = contentType.find_last_not_of(" \t");
  const std::string mediaType = first == std::string::npos
      ? std::string()
      : contentType.substr(first, last - first + 1);
  const bool vendorJson = mediaType.rfind("application/", 0) == 0 &&
      mediaType.size() >= 5 &&
      mediaType.compare(mediaType.size() - 5, 5, "+json") == 0;
  const bool isJson = mediaType == "application/json" || vendorJson;
  if (!isJson) return true;

  const auto raw = req->body();
  const int64_t maxBytes = std::clamp<int64_t>(
      pulse::config().envInt("JSON_BODY_LIMIT", 256 * 1024),
      1024, 16LL * 1024 * 1024);
  if (raw.size() > static_cast<size_t>(maxBytes)) {
    response = pulse::http::error(drogon::k413RequestEntityTooLarge,
                                  "JSON request body is too large",
                                  "PAYLOAD_TOO_LARGE");
    return false;
  }
  if (raw.empty()) return true;

  Json::Value parsed;
  Json::CharReaderBuilder builder;
  builder["collectComments"] = false;
  std::string errors;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  if (!reader->parse(raw.data(), raw.data() + raw.size(), &parsed, &errors)) {
    response = pulse::http::error(drogon::k400BadRequest,
                                  "Invalid JSON request body", "INVALID_JSON");
    return false;
  }

  try {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    req->setBody(Json::writeString(writer, pulse::sanitize::deepSanitize(parsed)));
    return true;
  } catch (const std::length_error& e) {
    response = pulse::http::error(drogon::k413RequestEntityTooLarge, e.what(),
                                  "PAYLOAD_TOO_LARGE");
    return false;
  }
}

void SanitizeFilter::doFilter(const HttpRequestPtr& req, FilterCallback&& fcb, FilterChainCallback&& fccb) {
  HttpResponsePtr response;
  if (!sanitizeJsonRequest(req, response)) return fcb(response);
  return fccb();
}

} // namespace pulse::filters
