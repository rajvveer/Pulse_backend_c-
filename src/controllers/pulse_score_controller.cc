// pulse_score_controller.cc — implementation of
// pulse::controllers::PulseScoreController.
//
// 1:1 port of src/controllers/pulseScoreController.js. The score query/compute
// logic lives in pulse::models::pulsescore (getOrCreate / getDisplayData /
// getLeaderboard / getUserRank) and is CALLED — not reimplemented — here. The
// only inline DB access is getUserScore, which the JS controller performs
// directly via PulseScore.findOne({ user: userId }).
//
// Each handler mirrors its Express counterpart exactly: read req.user.userId,
// parse the same query/path params, and on any thrown error reply with
// res.status(500).json({ success:false, error:'<handler message>' }). These
// endpoints use Express's bespoke { success, error } error shape (NO `code`
// field) and { success, data } success shape, so responses are built with
// pulse::http::json / pulse::http::ok directly.
#include "pulse/controllers/pulse_score_controller.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <optional>
#include <string>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>

#include "pulse/bson_json.hpp"
#include "pulse/db.hpp"
#include "pulse/http_response.hpp"
#include "pulse/logger.hpp"
#include "pulse/models/pulsescore.hpp"

using namespace pulse::controllers;
namespace model = pulse::models::pulsescore;
namespace bld = bsoncxx::builder::basic;
using bld::kvp;

namespace {

// ── JS parseInt() — parse a leading optional-sign integer; NaN on no digits ────
// Mirrors `parseInt(str, 10)`: trims leading whitespace, reads an optional sign
// then digits, stops at the first non-digit, returns nullopt (NaN) when no digit
// was consumed.
std::optional<long long> jsParseInt(const std::string& s) {
  size_t i = 0;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  const size_t numberStart = i;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
    ++i;
  }
  const size_t digitsStart = i;
  while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
  if (i == digitsStart) return std::nullopt;
  try {
    return std::stoll(s.substr(numberStart, i - numberStart));
  } catch (...) {
    return std::nullopt;
  }
}

// `parseInt(req.query.x) || fallback` — NaN OR 0 (both falsy) -> fallback.
long long clampedParam(const std::string& raw, long long fallback,
                       long long lo, long long hi) {
  auto parsed = jsParseInt(raw);
  long long value = (!parsed || *parsed == 0) ? fallback : *parsed;
  return std::max(lo, std::min(hi, value));
}

// req.user.userId from the AuthFilter-populated attribute.
std::string authedUserId(const drogon::HttpRequestPtr& req) {
  const auto& user = req->getAttributes()->get<Json::Value>("user");
  return user["userId"].asString();
}

// res.status(500).json({ success:false, error:<message> }) — the Express catch
// shape for this controller (note: `error`, NOT the standard { error, code }).
pulse::http::HttpResponsePtr failure(const std::string& message) {
  Json::Value body(Json::objectValue);
  body["success"] = false;
  body["error"] = message;
  return pulse::http::json(drogon::k500InternalServerError, std::move(body));
}

// arr.slice(-n) — last `n` elements of a JSON array (n<=0 -> empty, as JS
// slice(-0)===slice(0) returns the whole array only for n===0; here all call
// sites pass n>=1 via parseIntOr defaults so the n>0 branch is what matters).
Json::Value sliceTail(const Json::Value& arr, long n) {
  Json::Value out(Json::arrayValue);
  if (!arr.isArray()) return out;
  const long size = static_cast<long>(arr.size());
  long startIdx = size - n;
  if (startIdx < 0) startIdx = 0;
  for (long i = startIdx; i < size; ++i) out.append(arr[static_cast<Json::ArrayIndex>(i)]);
  return out;
}

}  // namespace

// ── GET /api/v1/pulse-score/me ────────────────────────────────────────────────
// const ps = await PulseScore.getOrCreate(req.user.userId);
// res.json({ success: true, data: ps.getDisplayData() });
void PulseScoreController::getMyScore(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    const std::string userId = authedUserId(req);
    Json::Value ps = model::getOrCreate(userId);
    callback(pulse::http::ok(model::getDisplayData(ps)));  // { success:true, data }
  } catch (const std::exception& e) {
    pulse::log::error("[PulseScore] getMyScore error: {}", e.what());
    callback(failure("Failed to get Pulse Score"));
  }
}

// ── GET /api/v1/pulse-score/breakdown ─────────────────────────────────────────
// const ps = await PulseScore.getOrCreate(req.user.userId);
// res.json({ success: true, data: {
//   ...ps.getDisplayData(), metrics: ps.metrics,
//   history: ps.history.slice(-30), achievements: ps.achievements } });
void PulseScoreController::getBreakdown(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    const std::string userId = authedUserId(req);
    Json::Value ps = model::getOrCreate(userId);

    // Spread ...ps.getDisplayData() first, then add the breakdown fields.
    Json::Value data = model::getDisplayData(ps);
    data["metrics"] = ps["metrics"];
    data["history"] = sliceTail(ps["history"], 30);
    data["achievements"] = ps["achievements"];

    callback(pulse::http::ok(std::move(data)));  // { success:true, data }
  } catch (const std::exception& e) {
    pulse::log::error("[PulseScore] getBreakdown error: {}", e.what());
    callback(failure("Failed to get breakdown"));
  }
}

// ── GET /api/v1/pulse-score/achievements ──────────────────────────────────────
// const ps = await PulseScore.getOrCreate(req.user.userId);
// res.json({ success: true, data: ps.achievements });
void PulseScoreController::getAchievements(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    const std::string userId = authedUserId(req);
    Json::Value ps = model::getOrCreate(userId);
    callback(pulse::http::ok(ps["achievements"]));  // { success:true, data }
  } catch (const std::exception& e) {
    pulse::log::error("[PulseScore] getAchievements error: {}", e.what());
    callback(failure("Failed to get achievements"));
  }
}

// ── GET /api/v1/pulse-score/history ───────────────────────────────────────────
// const days = parseInt(req.query.days) || 30;
// const ps = await PulseScore.getOrCreate(req.user.userId);
// res.json({ success: true, data: ps.history.slice(-days) });
void PulseScoreController::getHistory(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    const long long days = clampedParam(req->getParameter("days"), 30, 1, 365);
    const std::string userId = authedUserId(req);
    Json::Value ps = model::getOrCreate(userId);
    callback(pulse::http::ok(sliceTail(ps["history"], days)));  // { success:true, data }
  } catch (const std::exception& e) {
    pulse::log::error("[PulseScore] getHistory error: {}", e.what());
    callback(failure("Failed to get history"));
  }
}

// ── GET /api/v1/pulse-score/leaderboard ───────────────────────────────────────
// const limit = parseInt(req.query.limit) || 50;
// const leaderboard = await PulseScore.getLeaderboard(limit);
// const myRank = await PulseScore.getUserRank(req.user.userId);
// res.json({ success: true, data: {
//   leaderboard: leaderboard.map((entry, i) => ({
//     rank: i + 1, user: entry.user, score: entry.score, tier: entry.tier })),
//   myRank } });
void PulseScoreController::getLeaderboard(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    const long long limit = clampedParam(req->getParameter("limit"), 50, 1, 100);
    const std::string userId = authedUserId(req);

    Json::Value leaderboard = model::getLeaderboard(limit);
    std::optional<Json::Value> myRank = model::getUserRank(userId);

    Json::Value mapped(Json::arrayValue);
    if (leaderboard.isArray()) {
      for (Json::ArrayIndex i = 0; i < leaderboard.size(); ++i) {
        const Json::Value& entry = leaderboard[i];
        Json::Value row(Json::objectValue);
        row["rank"] = static_cast<int>(i) + 1;
        row["user"] = entry["user"];
        row["score"] = entry["score"];
        row["tier"] = entry["tier"];
        mapped.append(std::move(row));
      }
    }

    Json::Value data(Json::objectValue);
    data["leaderboard"] = std::move(mapped);
    // getUserRank returns null when the user has no PulseScore document.
    data["myRank"] = myRank ? *myRank : Json::Value(Json::nullValue);

    callback(pulse::http::ok(std::move(data)));  // { success:true, data }
  } catch (const std::exception& e) {
    pulse::log::error("[PulseScore] getLeaderboard error: {}", e.what());
    callback(failure("Failed to get leaderboard"));
  }
}

// ── GET /api/v1/pulse-score/user/:userId ──────────────────────────────────────
// const { userId } = req.params;
// const ps = await PulseScore.findOne({ user: userId });
// if (!ps) return res.json({ success: true,
//   data: { score: 0, tier: 'newcomer', tierEmoji: '🌱' } });
// res.json({ success: true, data: ps.getDisplayData() });
void PulseScoreController::getUserScore(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string userId) {
  try {
    // findOne({ user: userId }) — `user` is an ObjectId in the schema; coerce a
    // 24-hex string to ObjectId (string fallback simply matches nothing for a
    // non-oid, mirroring the model statics).
    bld::document filter;
    if (auto uid = pulse::bsonjson::tryOid(userId)) filter.append(kvp("user", *uid));
    else                                            filter.append(kvp("user", userId));

    auto doc = pulse::db::collection(model::kCollection).find_one(filter.view());

    if (!doc) {
      // Default newcomer payload for users without a PulseScore document.
      Json::Value data(Json::objectValue);
      data["score"] = 0;
      data["tier"] = "newcomer";
      data["tierEmoji"] = "\xF0\x9F\x8C\xB1";  // 🌱 (U+1F331)
      callback(pulse::http::ok(std::move(data)));  // { success:true, data }
      return;
    }

    Json::Value ps = pulse::bsonjson::toJson(doc->view());
    callback(pulse::http::ok(model::getDisplayData(ps)));  // { success:true, data }
  } catch (const std::exception& e) {
    pulse::log::error("[PulseScore] getUserScore error: {}", e.what());
    callback(failure("Failed to get user score"));
  }
}
