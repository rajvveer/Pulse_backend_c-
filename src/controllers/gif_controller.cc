// gif_controller.cc — implementation of GifController (ports gifController.js).
//
// Proxies Tenor's public GIF API. The Node controller used axios; here we use
// Drogon's HttpClient (synchronous sendRequest on a worker thread, the same
// pattern the service ports use). Every validation check, query parameter,
// result mapping, response body and status code mirrors the JS source 1:1.
#include "pulse/controllers/gif_controller.hpp"

#include <drogon/HttpClient.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <future>
#include <string>
#include <system_error>
#include <utility>

#include "pulse/config.hpp"
#include "pulse/http_response.hpp"
#include "pulse/logger.hpp"
#include "pulse/services/http_client.hpp"

using namespace pulse::controllers;

namespace {

// const TENOR_BASE_URL = 'https://tenor.googleapis.com/v2'
constexpr const char* kTenorHost = "https://tenor.googleapis.com";
constexpr const char* kTenorBasePath = "/v2";

// const TENOR_API_KEY = process.env.TENOR_API_KEY (read at call time)
std::string tenorApiKey() { return pulse::config().env("TENOR_API_KEY", ""); }

// const TENOR_CLIENT_KEY = process.env.TENOR_CLIENT_KEY || 'pulse_app'
std::string tenorClientKey() {
  std::string k = pulse::config().env("TENOR_CLIENT_KEY", "");
  return k.empty() ? std::string("pulse_app") : k;
}

// JS res.json({ success: false, message }) with an explicit status.
pulse::http::HttpResponsePtr messageResponse(drogon::HttpStatusCode status,
                                             const std::string& message,
                                             bool success = false) {
  Json::Value body(Json::objectValue);
  body["success"] = success;
  body["message"] = message;
  return pulse::http::json(status, std::move(body));
}

// const ensureConfigured = (res) => { if (TENOR_API_KEY) return true;
//   res.status(503).json({ success:false, message:'GIF service is not configured' });
//   return false; }
// Returns true if configured; otherwise invokes the callback with the 503 body.
bool ensureConfigured(const std::function<void(const drogon::HttpResponsePtr&)>& callback) {
  if (!tenorApiKey().empty()) return true;
  callback(messageResponse(drogon::k503ServiceUnavailable,
                           "GIF service is not configured"));
  return false;
}

// Tenor accepts at most 50 results. Invalid values fall back to the API default
// rather than being forwarded as an unbounded or nonsensical parameter.
std::string boundedLimit(const std::string& s) {
  if (s.empty()) return "20";
  int value = 0;
  const auto parsed = std::from_chars(s.data(), s.data() + s.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != s.data() + s.size()) return "20";
  return std::to_string(std::clamp(value, 1, 50));
}

// trim helper (JS .trim()).
std::string trim(const std::string& s) {
  auto isws = [](char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
  };
  size_t b = 0, e = s.size();
  while (b < e && isws(s[b])) ++b;
  while (e > b && isws(s[e - 1])) --e;
  return s.substr(b, e - b);
}

// Result of a Tenor GET: ok=false on transport/error status; body parsed JSON.
struct TenorResult {
  bool ok = false;
  Json::Value body;
};

// GET kTenorBasePath + subPath with the given query params (URL-encoded), the
// axios-equivalent call. ok mirrors axios resolving (2xx) vs throwing.
TenorResult tenorGet(const std::string& subPath,
                     const std::vector<std::pair<std::string, std::string>>& params) {
  TenorResult result;
  try {
    auto client = drogon::HttpClient::newHttpClient(kTenorHost);
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    // setPath() takes the raw (unencoded) path; query params go through
    // setParameter(), which Drogon percent-encodes correctly on the wire.
    // (Hand-building an already-encoded query and passing it to setPath()
    // double-encodes the '%', which made Tenor drop media_formats.)
    req->setPath(std::string(kTenorBasePath) + subPath);
    for (const auto& p : params) {
      req->setParameter(p.first, p.second);
    }
    req->addHeader("Accept", "application/json");

    std::promise<std::pair<drogon::ReqResult, drogon::HttpResponsePtr>> prom;
    auto fut = prom.get_future();
    client->sendRequest(
        req, [&prom](drogon::ReqResult rr, const drogon::HttpResponsePtr& resp) {
          prom.set_value({rr, resp});
        }, pulse::services::outboundHttpTimeoutSeconds());
    auto [rr, resp] = fut.get();

    if (rr != drogon::ReqResult::Ok || !resp) return result;  // axios throws
    int status = static_cast<int>(resp->getStatusCode());
    if (status < 200 || status >= 300) return result;  // axios throws on non-2xx

    auto jsonPtr = resp->getJsonObject();
    if (jsonPtr) result.body = *jsonPtr;
    result.ok = true;
  } catch (...) {
    // transport error -> ok=false, handled by caller's catch branch
  }
  return result;
}

// Map Tenor results -> the trimmed gif shape the JS controller returned.
//   { id, url, preview, width, height, description }
// Mirrors the optional-chaining fallbacks exactly.
Json::Value mapGifs(const Json::Value& results) {
  Json::Value gifs(Json::arrayValue);
  if (!results.isArray()) return gifs;

  for (const auto& gif : results) {
    const Json::Value& formats = gif["media_formats"];
    const Json::Value& gifFmt =
        formats.isObject() ? formats["gif"] : Json::Value(Json::nullValue);
    const Json::Value& tinyFmt =
        formats.isObject() ? formats["tinygif"] : Json::Value(Json::nullValue);

    // url: gif.media_formats?.gif?.url || ''
    std::string url =
        gifFmt.isObject() ? gifFmt.get("url", "").asString() : std::string();

    // preview: gif.media_formats?.tinygif?.url || gif.media_formats?.gif?.url || ''
    std::string preview =
        tinyFmt.isObject() ? tinyFmt.get("url", "").asString() : std::string();
    if (preview.empty()) preview = url;

    // dims: gif.media_formats?.gif?.dims?.[0] || 200  (and [1] || 200)
    Json::Value width(200);
    Json::Value height(200);
    if (gifFmt.isObject() && gifFmt["dims"].isArray()) {
      const Json::Value& dims = gifFmt["dims"];
      if (dims.size() > 0 && !dims[0u].isNull() &&
          !(dims[0u].isNumeric() && dims[0u].asDouble() == 0)) {
        width = dims[0u];
      }
      if (dims.size() > 1 && !dims[1u].isNull() &&
          !(dims[1u].isNumeric() && dims[1u].asDouble() == 0)) {
        height = dims[1u];
      }
    }

    Json::Value item(Json::objectValue);
    item["id"] = gif.get("id", "").asString();
    item["url"] = url;
    item["preview"] = preview;
    item["width"] = width;
    item["height"] = height;
    item["description"] = gif.get("content_description", "").asString();
    gifs.append(std::move(item));
  }
  return gifs;
}

}  // namespace

// ── Search GIFs ──────────────────────────────────────────────────────────────
// GET /api/v1/gifs/search
void GifController::searchGifs(const HttpRequestPtr& req,
                              std::function<void(const HttpResponsePtr&)>&& callback) {
  if (!ensureConfigured(callback)) return;
  try {
    // const { q, limit = 20 } = req.query;
    std::string q = req->getParameter("q");
    const std::string limit = boundedLimit(req->getParameter("limit"));

    // if (!q || !q.trim()) -> 400 { success:false, message:'Search query is required' }
    q = trim(q);
    if (q.empty()) {
      callback(messageResponse(drogon::k400BadRequest, "Search query is required"));
      return;
    }
    if (q.size() > 200) {
      callback(messageResponse(drogon::k400BadRequest, "Search query is too long"));
      return;
    }

    auto resp = tenorGet("/search", {
                                        {"q", q},
                                        {"key", tenorApiKey()},
                                        {"client_key", tenorClientKey()},
                                        {"limit", limit},
                                        {"media_filter", "gif,tinygif"},
                                        {"contentfilter", "medium"},
                                    });
    if (!resp.ok) {
      pulse::log::error("Tenor search error");
      callback(messageResponse(drogon::k500InternalServerError, "Failed to search GIFs"));
      return;
    }

    Json::Value gifs = mapGifs(resp.body["results"]);

    Json::Value body(Json::objectValue);
    body["data"] = gifs;
    body["count"] = static_cast<Json::UInt>(gifs.size());
    callback(pulse::http::success(std::move(body)));
  } catch (const std::exception& e) {
    pulse::log::error("Tenor search error: {}", e.what());
    callback(messageResponse(drogon::k500InternalServerError, "Failed to search GIFs"));
  }
}

// ── Trending/featured GIFs ───────────────────────────────────────────────────
// GET /api/v1/gifs/trending
void GifController::getTrendingGifs(const HttpRequestPtr& req,
                                   std::function<void(const HttpResponsePtr&)>&& callback) {
  if (!ensureConfigured(callback)) return;
  try {
    // const { limit = 20 } = req.query;
    const std::string limit = boundedLimit(req->getParameter("limit"));

    auto resp = tenorGet("/featured", {
                                          {"key", tenorApiKey()},
                                          {"client_key", tenorClientKey()},
                                          {"limit", limit},
                                          {"media_filter", "gif,tinygif"},
                                          {"contentfilter", "medium"},
                                      });
    if (!resp.ok) {
      pulse::log::error("Tenor trending error");
      callback(messageResponse(drogon::k500InternalServerError, "Failed to fetch trending GIFs"));
      return;
    }

    Json::Value gifs = mapGifs(resp.body["results"]);

    Json::Value body(Json::objectValue);
    body["data"] = gifs;
    body["count"] = static_cast<Json::UInt>(gifs.size());
    callback(pulse::http::success(std::move(body)));
  } catch (const std::exception& e) {
    pulse::log::error("Tenor trending error: {}", e.what());
    callback(messageResponse(drogon::k500InternalServerError, "Failed to fetch trending GIFs"));
  }
}

// ── GIF categories ───────────────────────────────────────────────────────────
// GET /api/v1/gifs/categories
void GifController::getCategories(const HttpRequestPtr& req,
                                 std::function<void(const HttpResponsePtr&)>&& callback) {
  (void)req;
  if (!ensureConfigured(callback)) return;
  try {
    auto resp = tenorGet("/categories", {
                                            {"key", tenorApiKey()},
                                            {"client_key", tenorClientKey()},
                                        });
    if (!resp.ok) {
      pulse::log::error("Tenor categories error");
      callback(messageResponse(drogon::k500InternalServerError, "Failed to fetch categories"));
      return;
    }

    // data: response.data.tags || []
    Json::Value tags = resp.body["tags"];
    if (!tags.isArray()) tags = Json::Value(Json::arrayValue);

    Json::Value body(Json::objectValue);
    body["data"] = tags;
    callback(pulse::http::success(std::move(body)));
  } catch (const std::exception& e) {
    pulse::log::error("Tenor categories error: {}", e.what());
    callback(messageResponse(drogon::k500InternalServerError, "Failed to fetch categories"));
  }
}
