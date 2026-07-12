// roulette_controller.cc — implementation of the roulette route group
// (src/controllers/rouletteController.js + src/routes/rouletteRoutes.js).
//
// Ground truth: rouletteController.js. Every handler mirrors its JS counterpart
// 1:1 — same Mongo queries (delegated to the Roulette model statics/instance
// helpers already ported in pulse/models/roulette.hpp), same response JSON
// shapes ({ success, data } / { success, message } / { success, error }), and
// the same status codes (200 / 400 / 404 / 500).
//
// The JS controller uses the Express-style error envelope { success:false,
// error:'...' } WITHOUT a `code` field, so we build those verbatim instead of
// going through pulse::http::error (which always stamps a `code`).
//
// Mongoose .populate('users.user', 'username profile.displayName
// profile.avatar') is reproduced by fetching the referenced User docs with the
// exact field selection the JS requested.
#include "pulse/controllers/roulette_controller.hpp"

#include "pulse/http_response.hpp"
#include "pulse/db.hpp"
#include "pulse/cache.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"
#include "pulse/models/roulette.hpp"
#include "pulse/models/user.hpp"
#include "pulse/models/follow.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/oid.hpp>

#include <mongocxx/collection.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/exception/operation_exception.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>
#include <optional>
#include <string>
#include <vector>

namespace pulse::controllers {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;

namespace {

// ---------------------------------------------------------------------------
// Response helpers — the roulette controller does NOT use the { error, code }
// envelope. It returns Express-style { success:false, error } /
// { success:true, data } / { success:true, message }. Build them verbatim so
// the JSON shape matches res.json() exactly.
// ---------------------------------------------------------------------------

// res.status(code).json({ success:false, error })
HttpResponsePtr failError(HttpStatusCode code, const std::string& error) {
  Json::Value body(Json::objectValue);
  body["success"] = false;
  body["error"] = error;
  return pulse::http::json(code, std::move(body));
}

// res.json({ success:true, data })
HttpResponsePtr okData(Json::Value data) {
  Json::Value body(Json::objectValue);
  body["success"] = true;
  body["data"] = std::move(data);
  return pulse::http::json(drogon::k200OK, std::move(body));
}

// res.json({ success:true, message })
HttpResponsePtr okMessage(const std::string& message) {
  Json::Value body(Json::objectValue);
  body["success"] = true;
  body["message"] = message;
  return pulse::http::json(drogon::k200OK, std::move(body));
}

// JS String.prototype.trim — removes leading/trailing whitespace
// (space, \t, \n, \r, \f, \v).
std::string trim(const std::string& s) {
  const char* ws = " \t\n\r\f\v";
  const auto b = s.find_first_not_of(ws);
  if (b == std::string::npos) return "";
  const auto e = s.find_last_not_of(ws);
  return s.substr(b, e - b + 1);
}

// JS parseInt-style integer parse (leading numeric prefix, NaN-on-failure).
// Returns std::nullopt when no leading integer is present (mirrors NaN).
std::optional<long long> jsParseInt(const std::string& s) {
  std::size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' ||
                          s[i] == '\r' || s[i] == '\f' || s[i] == '\v')) {
    ++i;
  }
  std::size_t start = i;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
  std::size_t digitsStart = i;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
  if (i == digitsStart) return std::nullopt;
  try {
    return std::stoll(s.substr(start, i - start));
  } catch (...) {
    return std::nullopt;
  }
}

// Populate one referenced User by hex id with the projection from
// .populate('users.user', 'username profile.displayName profile.avatar').
// _id is always included by Mongoose. Returns the user doc (sanitized), or a
// null Json when the user cannot be resolved (mirrors a populate that yields
// no document -> null).
Json::Value populateUser(const std::string& hexId) {
  auto oid = pulse::bsonjson::tryOid(hexId);
  if (!oid) return Json::Value(Json::nullValue);
  mongocxx::options::find opts;
  opts.projection(make_document(kvp("username", 1),
                                kvp("profile.displayName", 1),
                                kvp("profile.avatar", 1)));
  auto col = pulse::db::collection(pulse::models::user::kCollection);
  auto res = col.find_one(make_document(kvp("_id", *oid)), opts);
  if (!res) return Json::Value(Json::nullValue);
  return pulse::models::user::sanitizeForOutput(
      pulse::bsonjson::toJson(res->view()));
}

// Replace every users[].user hex-id ref with the populated user doc, mirroring
// .populate('users.user', 'username profile.displayName profile.avatar').
void populateSessionUsers(Json::Value& session) {
  if (!session.isObject() || !session.isMember("users") ||
      !session["users"].isArray()) {
    return;
  }
  for (auto& entry : session["users"]) {
    if (entry.isObject() && entry.isMember("user") && entry["user"].isString()) {
      entry["user"] = populateUser(entry["user"].asString());
    }
  }
}

// JS: populated.users.find(u => u.user._id.toString() !== userId)?.user
// (over a populated users array). Returns the partner user object, or null if
// there is no other participant. A null/unpopulated `user` is skipped.
Json::Value findPartner(const Json::Value& session, const std::string& userId) {
  if (!session.isObject() || !session.isMember("users") ||
      !session["users"].isArray()) {
    return Json::Value(Json::nullValue);
  }
  for (const auto& entry : session["users"]) {
    if (!entry.isObject() || !entry.isMember("user")) continue;
    const Json::Value& u = entry["user"];
    if (!u.isObject() || !u.isMember("_id")) continue;
    if (u["_id"].asString() != userId) {
      return u;
    }
  }
  return Json::Value(Json::nullValue);
}

// Returns true when userId is a participant of the (unpopulated) session, i.e.
// some users[].user hex string === userId. Mirrors
// session.users.find(u => u.user.toString() === userId).
bool isParticipantRaw(const Json::Value& session, const std::string& userId) {
  if (!session.isObject() || !session.isMember("users") ||
      !session["users"].isArray()) {
    return false;
  }
  for (const auto& entry : session["users"]) {
    if (entry.isObject() && entry.isMember("user") && entry["user"].isString() &&
        entry["user"].asString() == userId) {
      return true;
    }
  }
  return false;
}

} // namespace

// ===========================================================================
// POST /api/v1/roulette/join  — joinQueue
// ===========================================================================
void RouletteController::joinQueue(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    const auto user = req->getAttributes()->get<Json::Value>("user");
    const std::string userId = user["userId"].asString();

    // Try to find a match immediately.
    auto match = pulse::models::roulette::findMatch(userId);

    if (match) {
      // Found a match! Start the chat.
      const std::string sessionHex = (*match)["_id"].asString();
      auto sessionOid = pulse::bsonjson::oid(sessionHex);
      pulse::models::roulette::startChat(sessionOid);

      // const populated = await match.populate('users.user',
      //   'username profile.displayName profile.avatar');
      Json::Value populated = *match;
      populateSessionUsers(populated);

      Json::Value data(Json::objectValue);
      data["sessionId"] = (*match)["_id"];          // match._id
      data["status"] = "matched";                   // hardcoded literal
      data["partner"] = findPartner(populated, userId);
      data["icebreaker"] = (*match)["icebreaker"];  // match.icebreaker
      data["chatDuration"] = (*match)["chatDuration"]; // match.chatDuration

      callback(okData(std::move(data)));
      return;
    }

    // No match yet — join the queue.
    Json::Value session = pulse::models::roulette::joinQueue(userId);

    Json::Value data(Json::objectValue);
    data["sessionId"] = session["_id"];
    data["status"] = "waiting";
    data["message"] = "Looking for someone to match with...";

    callback(okData(std::move(data)));
  } catch (const std::exception& e) {
    pulse::log::error("[Roulette] joinQueue error: {}", e.what());
    callback(failError(drogon::k500InternalServerError, "Failed to join roulette"));
  }
}

// ===========================================================================
// GET /api/v1/roulette/status  — checkStatus (polling)
// ===========================================================================
void RouletteController::checkStatus(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    const auto user = req->getAttributes()->get<Json::Value>("user");
    const std::string userId = user["userId"].asString();

    auto col = pulse::db::collection(pulse::models::roulette::kCollection);

    // Roulette.findOne({ 'users.user': userId,
    //   status: { $in: ['waiting','matched','chatting','deciding'] } })
    //   .populate('users.user', 'username profile.displayName profile.avatar')
    auto found = col.find_one(make_document(
        kvp("users.user", pulse::bsonjson::oid(userId)),
        kvp("status", make_document(kvp("$in",
            bld::make_array("waiting", "matched", "chatting", "deciding"))))));

    if (!found) {
      Json::Value data(Json::objectValue);
      data["status"] = "none";
      callback(okData(std::move(data)));
      return;
    }

    Json::Value session = pulse::models::roulette::sanitizeForOutput(
        pulse::bsonjson::toJson(found->view()));

    Json::Value populated = session;
    populateSessionUsers(populated);
    Json::Value partner = findPartner(populated, userId);

    // Calculate remaining time.
    Json::Value timeRemaining(Json::nullValue);
    const std::string status = session.isMember("status")
                                   ? session["status"].asString() : "";
    if (status == "chatting" && session.isMember("chatStartedAt") &&
        session["chatStartedAt"].isString() &&
        !session["chatStartedAt"].asString().empty()) {
      // elapsed = (Date.now() - chatStartedAt) / 1000
      long long nowMs = pulse::bsonjson::nowMillis();
      long long startedMs = 0;
      {
        // chatStartedAt serialized as ISO-8601 by bsonjson::toJson; parse it.
        const std::string iso = session["chatStartedAt"].asString();
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0;
        if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d.%dZ",
                        &y, &mo, &d, &h, &mi, &s, &ms) >= 6) {
          std::tm tm{};
          tm.tm_year = y - 1900;
          tm.tm_mon = mo - 1;
          tm.tm_mday = d;
          tm.tm_hour = h;
          tm.tm_min = mi;
          tm.tm_sec = s;
#if defined(_WIN32)
          std::time_t t = _mkgmtime(&tm);
#else
          std::time_t t = timegm(&tm);
#endif
          startedMs = static_cast<long long>(t) * 1000 + ms;
        }
      }
      double elapsed = static_cast<double>(nowMs - startedMs) / 1000.0;
      int chatDuration = session.isMember("chatDuration")
                             ? session["chatDuration"].asInt()
                             : pulse::models::roulette::kDefaultChatDuration;
      // Math.max(0, chatDuration - Math.floor(elapsed))
      long long remaining = chatDuration -
          static_cast<long long>(std::floor(elapsed));
      if (remaining < 0) remaining = 0;
      timeRemaining = static_cast<Json::Int64>(remaining);

      // Auto-transition to deciding if time is up.
      if (remaining <= 0 && status == "chatting") {
        col.update_one(
            make_document(kvp("_id", found->view()["_id"].get_oid().value)),
            make_document(kvp("$set", make_document(kvp("status", "deciding")))));
        session["status"] = "deciding";
      }
    }

    Json::Value data(Json::objectValue);
    data["sessionId"] = session["_id"];
    data["status"] = session["status"];
    data["partner"] = partner.isNull() ? Json::Value(Json::nullValue) : partner;
    data["icebreaker"] = session.isMember("icebreaker")
                             ? session["icebreaker"] : Json::Value("");

    // messages: session.messages.slice(-50)
    Json::Value messages(Json::arrayValue);
    if (session.isMember("messages") && session["messages"].isArray()) {
      const Json::Value& msgs = session["messages"];
      Json::ArrayIndex total = msgs.size();
      Json::ArrayIndex startIdx = total > 50 ? total - 50 : 0;
      for (Json::ArrayIndex i = startIdx; i < total; ++i) {
        messages.append(msgs[i]);
      }
    }
    data["messages"] = std::move(messages);
    data["timeRemaining"] = timeRemaining;
    data["chatDuration"] = session.isMember("chatDuration")
                               ? session["chatDuration"]
                               : Json::Value(pulse::models::roulette::kDefaultChatDuration);
    data["outcome"] = session.isMember("outcome")
                          ? session["outcome"] : Json::Value(Json::nullValue);

    callback(okData(std::move(data)));
  } catch (const std::exception& e) {
    pulse::log::error("[Roulette] checkStatus error: {}", e.what());
    callback(failError(drogon::k500InternalServerError, "Failed to check status"));
  }
}

// ===========================================================================
// POST /api/v1/roulette/message  — sendMessage
// ===========================================================================
void RouletteController::sendMessage(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    const auto user = req->getAttributes()->get<Json::Value>("user");
    const std::string userId = user["userId"].asString();

    auto bodyPtr = req->getJsonObject();
    std::string sessionId;
    std::string text;
    if (bodyPtr) {
      if ((*bodyPtr).isMember("sessionId") && (*bodyPtr)["sessionId"].isString())
        sessionId = (*bodyPtr)["sessionId"].asString();
      if ((*bodyPtr).isMember("text") && (*bodyPtr)["text"].isString())
        text = (*bodyPtr)["text"].asString();
    }

    // if (!text?.trim()) -> 400 'Message text required'
    const std::string trimmedText = trim(text);
    if (trimmedText.empty()) {
      callback(failError(drogon::k400BadRequest, "Message text required"));
      return;
    }

    // const session = await Roulette.findById(sessionId);
    auto sessionOid = pulse::bsonjson::tryOid(sessionId);
    Json::Value session(Json::nullValue);
    if (sessionOid) {
      auto col = pulse::db::collection(pulse::models::roulette::kCollection);
      auto found = col.find_one(make_document(kvp("_id", *sessionOid)));
      if (found) {
        session = pulse::models::roulette::sanitizeForOutput(
            pulse::bsonjson::toJson(found->view()));
      }
    }

    // if (!session || !session.users.find(u => u.user.toString() === userId))
    //   -> 404 'Session not found'
    if (session.isNull() || !isParticipantRaw(session, userId)) {
      callback(failError(drogon::k404NotFound, "Session not found"));
      return;
    }

    // if (session.status !== 'chatting') -> 400 'Chat time is over'
    const std::string status = session.isMember("status")
                                   ? session["status"].asString() : "";
    if (status != "chatting") {
      callback(failError(drogon::k400BadRequest, "Chat time is over"));
      return;
    }

    // const message = await session.addMessage(userId, text.trim());
    auto message = pulse::models::roulette::addMessage(*sessionOid, userId,
                                                       trimmedText);

    callback(okData(message ? *message : Json::Value(Json::nullValue)));
  } catch (const std::exception& e) {
    pulse::log::error("[Roulette] sendMessage error: {}", e.what());
    callback(failError(drogon::k500InternalServerError, "Failed to send message"));
  }
}

// ===========================================================================
// POST /api/v1/roulette/decide  — decide (connect or pass)
// ===========================================================================
void RouletteController::decide(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    const auto user = req->getAttributes()->get<Json::Value>("user");
    const std::string userId = user["userId"].asString();

    auto bodyPtr = req->getJsonObject();
    std::string sessionId;
    std::string decision;
    if (bodyPtr) {
      if ((*bodyPtr).isMember("sessionId") && (*bodyPtr)["sessionId"].isString())
        sessionId = (*bodyPtr)["sessionId"].asString();
      if ((*bodyPtr).isMember("decision") && (*bodyPtr)["decision"].isString())
        decision = (*bodyPtr)["decision"].asString();
    }

    // if (!['connect','pass'].includes(decision))
    //   -> 400 'Decision must be "connect" or "pass"'
    if (decision != "connect" && decision != "pass") {
      callback(failError(drogon::k400BadRequest,
                         "Decision must be \"connect\" or \"pass\""));
      return;
    }

    // const session = await Roulette.findById(sessionId);
    auto sessionOid = pulse::bsonjson::tryOid(sessionId);
    Json::Value session(Json::nullValue);
    if (sessionOid) {
      auto col = pulse::db::collection(pulse::models::roulette::kCollection);
      auto found = col.find_one(make_document(kvp("_id", *sessionOid)));
      if (found) {
        session = pulse::models::roulette::sanitizeForOutput(
            pulse::bsonjson::toJson(found->view()));
      }
    }

    // if (!session || !session.users.find(u => u.user.toString() === userId))
    //   -> 404 'Session not found'
    if (session.isNull() || !isParticipantRaw(session, userId)) {
      callback(failError(drogon::k404NotFound, "Session not found"));
      return;
    }

    // const result = await session.recordDecision(userId, decision);
    auto result = pulse::models::roulette::recordDecision(*sessionOid, userId,
                                                          decision);
    if (!result) {
      // Session vanished between the read and recordDecision; mirror the
      // controller's surrounding try/catch (500 'Failed to record decision').
      callback(failError(drogon::k500InternalServerError,
                         "Failed to record decision"));
      return;
    }

    // If mutual connect, create an actual connection.
    if (result->isMember("outcome") &&
        (*result)["outcome"].asString() == "mutual_connect") {
      // const otherUserId = session.users.find(
      //   u => u.user.toString() !== userId)?.user
      std::string otherUserId;
      if (session.isMember("users") && session["users"].isArray()) {
        for (const auto& entry : session["users"]) {
          if (entry.isObject() && entry.isMember("user") &&
              entry["user"].isString() &&
              entry["user"].asString() != userId) {
            otherUserId = entry["user"].asString();
            break;
          }
        }
      }

      if (!otherUserId.empty()) {
        try {
          // Mutual follow — both follow each other via the Follow collection.
          // Follow.create is idempotent (unique index): a repeat connect races
          // to 11000, which is swallowed. We create directly; the unique index
          // makes a duplicate a no-op.
          auto followCol = pulse::db::collection(pulse::models::follow::kCollection);

          auto createFollow = [&](const std::string& follower,
                                  const std::string& following) {
            // Follow.create({ follower, following }) — the Mongoose
            // `timestamps:true` option stamps createdAt/updatedAt as real BSON
            // Dates so the { ..., createdAt:-1 } indexes sort correctly.
            const bsoncxx::types::b_date now{
                std::chrono::milliseconds{pulse::bsonjson::nowMillis()}};
            try {
              followCol.insert_one(make_document(
                  kvp("follower", pulse::bsonjson::oid(follower)),
                  kvp("following", pulse::bsonjson::oid(following)),
                  kvp("createdAt", now),
                  kvp("updatedAt", now)));
            } catch (const mongocxx::operation_exception& err) {
              // catch(e => { if (e.code !== 11000) throw e; }) — swallow the
              // duplicate-key race (unique { follower, following } index); any
              // other error bubbles to the surrounding try/catch.
              if (err.code().value() != 11000) throw;
            }
          };

          createFollow(userId, otherUserId);
          createFollow(otherUserId, userId);

          // Reconcile the cached counters from the source of truth.
          long long uF = pulse::models::follow::getFollowerCount(userId);
          long long uG = pulse::models::follow::getFollowingCount(userId);
          long long oF = pulse::models::follow::getFollowerCount(otherUserId);
          long long oG = pulse::models::follow::getFollowingCount(otherUserId);

          auto userCol = pulse::db::collection(pulse::models::user::kCollection);
          userCol.update_one(
              make_document(kvp("_id", pulse::bsonjson::oid(userId))),
              make_document(kvp("$set", make_document(
                  kvp("stats.followers", static_cast<std::int64_t>(uF)),
                  kvp("stats.following", static_cast<std::int64_t>(uG))))));
          userCol.update_one(
              make_document(kvp("_id", pulse::bsonjson::oid(otherUserId))),
              make_document(kvp("$set", make_document(
                  kvp("stats.followers", static_cast<std::int64_t>(oF)),
                  kvp("stats.following", static_cast<std::int64_t>(oG))))));

          // Best-effort cache invalidation (JS .catch(() => {}) — never throws).
          try { pulse::cache().del("followgraph:" + userId); } catch (...) {}
          try { pulse::cache().del("followgraph:" + otherUserId); } catch (...) {}
          try { pulse::cache().del("reel:following:" + userId); } catch (...) {}
          try { pulse::cache().del("reel:following:" + otherUserId); } catch (...) {}
        } catch (const std::exception& ex) {
          // try/catch around Follow creation logs and continues (JS behavior).
          pulse::log::error("[Roulette] Follow creation error: {}", ex.what());
        }
      }
    }

    callback(okData(*result));
  } catch (const std::exception& e) {
    pulse::log::error("[Roulette] decide error: {}", e.what());
    callback(failError(drogon::k500InternalServerError,
                       "Failed to record decision"));
  }
}

// ===========================================================================
// POST /api/v1/roulette/leave  — leave / cancel
// ===========================================================================
void RouletteController::leave(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    const auto user = req->getAttributes()->get<Json::Value>("user");
    const std::string userId = user["userId"].asString();

    auto col = pulse::db::collection(pulse::models::roulette::kCollection);

    // Roulette.findOne({ 'users.user': userId,
    //                    status: { $in: ['waiting','matched'] } })
    auto found = col.find_one(make_document(
        kvp("users.user", pulse::bsonjson::oid(userId)),
        kvp("status", make_document(kvp("$in",
            bld::make_array("waiting", "matched"))))));

    if (found) {
      // session.status = 'expired'; session.outcome = 'expired'; await save();
      col.update_one(
          make_document(kvp("_id", found->view()["_id"].get_oid().value)),
          make_document(kvp("$set", make_document(
              kvp("status", "expired"),
              kvp("outcome", "expired")))));
    }

    callback(okMessage("Left roulette"));
  } catch (const std::exception& e) {
    pulse::log::error("[Roulette] leave error: {}", e.what());
    callback(failError(drogon::k500InternalServerError, "Failed to leave"));
  }
}

// ===========================================================================
// GET /api/v1/roulette/history  — getHistory
// ===========================================================================
void RouletteController::getHistory(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    const auto user = req->getAttributes()->get<Json::Value>("user");
    const std::string userId = user["userId"].asString();

    // const limit = parseInt(req.query.limit) || 20;
    int limit = 20;
    {
      auto p = jsParseInt(req->getParameter("limit"));
      if (p && *p != 0) limit = static_cast<int>(*p);
    }

    // const history = await Roulette.getUserHistory(userId, limit);
    std::vector<Json::Value> history =
        pulse::models::roulette::getUserHistory(userId, limit);

    // data: history.map(h => ({ sessionId, partner, outcome, messageCount, date }))
    Json::Value data(Json::arrayValue);
    for (auto& h : history) {
      // getUserHistory already populated users.user; if not, populate here so
      // the partner lookup (u.user._id) works as the JS expects.
      populateSessionUsers(h);

      Json::Value item(Json::objectValue);
      item["sessionId"] = h.isMember("_id") ? h["_id"] : Json::Value(Json::nullValue);

      // partner: h.users.find(u => u.user._id?.toString() !== userId)?.user
      item["partner"] = findPartner(h, userId);

      item["outcome"] = h.isMember("outcome") ? h["outcome"]
                                              : Json::Value(Json::nullValue);

      // messageCount: h.messages?.length || 0
      int messageCount = 0;
      if (h.isMember("messages") && h["messages"].isArray()) {
        messageCount = static_cast<int>(h["messages"].size());
      }
      item["messageCount"] = messageCount;

      item["date"] = h.isMember("createdAt") ? h["createdAt"]
                                             : Json::Value(Json::nullValue);

      data.append(std::move(item));
    }

    callback(okData(std::move(data)));
  } catch (const std::exception& e) {
    pulse::log::error("[Roulette] getHistory error: {}", e.what());
    callback(failError(drogon::k500InternalServerError, "Failed to get history"));
  }
}

} // namespace pulse::controllers
