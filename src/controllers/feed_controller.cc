// feed_controller.cc — implementation of pulse::controllers::FeedController.
//
// 1:1 port of src/controllers/feedController.js. The heavy lifting (follow graph,
// candidate generation, ranking, pagination, processPosts, caching) lives in
// pulse::services::feed and is called — NOT reimplemented — here. Each handler
// mirrors its Express counterpart: parse the same query params, read req.user,
// validate exactly as the JS did, and on any thrown error reply with
// res.status(500).json({ success:false, message:'Failed to load feed' }).
#include "pulse/controllers/feed_controller.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <string>

#include "pulse/http_response.hpp"
#include "pulse/logger.hpp"
#include "pulse/services/feed_service.hpp"

using namespace pulse::controllers;
namespace feed = pulse::services::feed;

namespace {

// ── JS parseInt() — parse the leading optional-sign integer; NaN on no digits ──
// Mirrors `parseInt(str, 10)`: skips nothing, reads an optional sign then digits,
// stops at the first non-digit, returns nullopt when no digit was consumed (NaN).
std::optional<long> jsParseInt(const std::string& s) {
  size_t i = 0;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;  // parseInt trims leading ws
  bool neg = false;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
    neg = (s[i] == '-');
    ++i;
  }
  size_t start = i;
  long value = 0;
  while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
    value = value * 10 + (s[i] - '0');
    ++i;
  }
  if (i == start) return std::nullopt;  // no digits -> NaN
  return neg ? -value : value;
}

// JS parseFloat — leading number, NaN (nullopt) on no parse.
std::optional<double> jsParseFloat(const std::string& s) {
  if (s.empty()) return std::nullopt;
  errno = 0;
  char* end = nullptr;
  double v = std::strtod(s.c_str(), &end);
  if (end == s.c_str()) return std::nullopt;  // no conversion -> NaN
  return v;
}

// clampPage(p) = Math.min(Math.max(parseInt(p) || 1, 1), MAX_PAGE=50)
int clampPage(const std::string& raw) {
  long v = jsParseInt(raw).value_or(0);
  if (v == 0) v = 1;  // (parseInt(p) || 1): NaN OR 0 -> 1
  v = std::max<long>(v, 1);
  v = std::min<long>(v, 50);
  return static_cast<int>(v);
}

// clampLimit(l) = Math.min(Math.max(parseInt(l) || 20, 1), 50)
int clampLimit(const std::string& raw) {
  long v = jsParseInt(raw).value_or(0);
  if (v == 0) v = 20;  // (parseInt(l) || 20): NaN OR 0 -> 20
  v = std::max<long>(v, 1);
  v = std::min<long>(v, 50);
  return static_cast<int>(v);
}

// The Express catch in every handler: console.error + 500 'Failed to load feed'.
pulse::http::HttpResponsePtr failedToLoad() {
  Json::Value body(Json::objectValue);
  body["success"] = false;
  body["message"] = "Failed to load feed";
  return pulse::http::json(drogon::k500InternalServerError, std::move(body));
}

// req.user.userId from the AuthFilter-populated attribute.
std::string authedUserId(const drogon::HttpRequestPtr& req) {
  const auto& user = req->getAttributes()->get<Json::Value>("user");
  return user["userId"].asString();
}

}  // namespace

// ── GET /api/v1/feed/foryou ───────────────────────────────────────────────────
void FeedController::getForYouFeed(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    feed::PageParams page;
    page.page = clampPage(req->getParameter("page"));
    page.limit = clampLimit(req->getParameter("limit"));
    const std::string userId = authedUserId(req);

    Json::Value body = feed::getForYouFeed(userId, page);
    callback(pulse::http::json(drogon::k200OK, std::move(body)));
  } catch (const std::exception& e) {
    pulse::log::error("Get for you feed error: {}", e.what());
    callback(failedToLoad());
  }
}

// ── GET /api/v1/feed/following ────────────────────────────────────────────────
void FeedController::getFollowingFeed(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    feed::PageParams page;
    page.page = clampPage(req->getParameter("page"));
    page.limit = clampLimit(req->getParameter("limit"));
    const std::string userId = authedUserId(req);
    const std::string before = req->getParameter("before");  // empty == not provided

    Json::Value body = feed::getFollowingFeed(userId, page, before);
    callback(pulse::http::json(drogon::k200OK, std::move(body)));
  } catch (const std::exception& e) {
    pulse::log::error("Get following feed error: {}", e.what());
    callback(failedToLoad());
  }
}

// ── GET /api/v1/feed/global ───────────────────────────────────────────────────
void FeedController::getGlobalFeed(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    feed::PageParams page;
    page.page = clampPage(req->getParameter("page"));
    page.limit = clampLimit(req->getParameter("limit"));
    const std::string userId = authedUserId(req);

    Json::Value body = feed::getGlobalFeed(userId, page);
    callback(pulse::http::json(drogon::k200OK, std::move(body)));
  } catch (const std::exception& e) {
    pulse::log::error("Get global feed error: {}", e.what());
    callback(failedToLoad());
  }
}

// ── GET /api/v1/feed/home ─────────────────────────────────────────────────────
void FeedController::getHomeFeed(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    feed::PageParams page;
    page.page = clampPage(req->getParameter("page"));
    page.limit = clampLimit(req->getParameter("limit"));
    // req.query.vibe || 'auto' — empty query param falls back to "auto".
    std::string vibe = req->getParameter("vibe");
    if (vibe.empty()) vibe = "auto";
    const std::string userId = authedUserId(req);

    Json::Value body = feed::getHomeFeed(userId, page, vibe);
    callback(pulse::http::json(drogon::k200OK, std::move(body)));
  } catch (const std::exception& e) {
    pulse::log::error("Get home feed error: {}", e.what());
    callback(failedToLoad());
  }
}

// ── GET /api/v1/feed/trending ─────────────────────────────────────────────────
void FeedController::getTrendingPosts(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    const int limit = clampLimit(req->getParameter("limit"));
    // timeRange = Math.min(Math.max(parseInt(req.query.timeRange) || 6, 1), 168).
    // The service clamps to 1..168; resolve the JS `parseInt || 6` default here.
    long tr = jsParseInt(req->getParameter("timeRange")).value_or(0);
    if (tr == 0) tr = 6;  // (parseInt || 6): NaN OR 0 -> 6
    const std::string userId = authedUserId(req);

    Json::Value body = feed::getTrendingPosts(userId, limit, static_cast<int>(tr));
    callback(pulse::http::json(drogon::k200OK, std::move(body)));
  } catch (const std::exception& e) {
    pulse::log::error("Get trending posts error: {}", e.what());
    callback(failedToLoad());
  }
}

// ── GET /api/v1/feed/nearby ───────────────────────────────────────────────────
void FeedController::getNearbyPosts(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    const std::string longitudeRaw = req->getParameter("longitude");
    const std::string latitudeRaw = req->getParameter("latitude");
    const int limit = clampLimit(req->getParameter("limit"));

    // if (!longitude || !latitude) -> 400 { success:false, message:'Location required' }
    // JS truthiness: missing/empty query params are falsy. (Note: "0" is a
    // non-empty string and therefore truthy in JS, matching this empty() check.)
    if (longitudeRaw.empty() || latitudeRaw.empty()) {
      Json::Value body(Json::objectValue);
      body["success"] = false;
      body["message"] = "Location required";
      callback(pulse::http::json(drogon::k400BadRequest, std::move(body)));
      return;
    }

    const std::string userId = authedUserId(req);

    // [parseFloat(longitude), parseFloat(latitude)]
    const double longitude = jsParseFloat(longitudeRaw).value_or(std::nan(""));
    const double latitude = jsParseFloat(latitudeRaw).value_or(std::nan(""));

    // Math.min(parseInt(maxDistance) || 1000, 50000) — service clamps to 50km;
    // resolve the JS `parseInt(maxDistance) || 1000` default (default 1000) here.
    long maxDistance = jsParseInt(req->getParameter("maxDistance")).value_or(0);
    if (maxDistance == 0) maxDistance = 1000;  // (parseInt || 1000): NaN OR 0 -> 1000

    Json::Value body = feed::getNearbyPosts(
        userId, longitude, latitude, static_cast<double>(maxDistance), limit);
    callback(pulse::http::json(drogon::k200OK, std::move(body)));
  } catch (const std::exception& e) {
    pulse::log::error("Get nearby posts error: {}", e.what());
    callback(failedToLoad());
  }
}
