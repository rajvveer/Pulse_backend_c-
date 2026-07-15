// alter_ego_controller.cc — implementation of pulse::controllers::AlterEgoController.
//
// 1:1 port of src/controllers/alterEgoController.js. The DB query/mutation logic
// lives in pulse::models::alterego (getOrCreate / updateTraining /
// recordGuessResult / learnFromUser / recordReply) and the AI generation in
// pulse::alterEgoAI() — these are CALLED here, not reimplemented. Each handler
// mirrors its Express counterpart: read req.user, parse the same body/query
// fields, validate exactly as the JS did, and on any thrown error reply with
// res.status(500).json({ success:false, message: error.message }).
//
// Response shapes are reproduced verbatim from the JS res.json() calls. Note the
// AlterEgo controller uses the `{ success, data }` / `{ success, message }`
// shapes (NOT the `{ success, error, code }` error contract used elsewhere), so
// error replies are built explicitly with a top-level `message` field.
#include "pulse/controllers/alter_ego_controller.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string>
#include <utility>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/oid.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/collection.hpp>

#include "pulse/bson_json.hpp"
#include "pulse/db.hpp"
#include "pulse/http_response.hpp"
#include "pulse/logger.hpp"
#include "pulse/models/alterego.hpp"
#include "pulse/services/alter_ego_ai_service.hpp"

using namespace pulse::controllers;
namespace alterego = pulse::models::alterego;

namespace {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;

// req.user.userId from the AuthFilter-populated attribute.
std::string authedUserId(const drogon::HttpRequestPtr& req) {
  const auto& user = req->getAttributes()->get<Json::Value>("user");
  return user["userId"].asString();
}

// The Express catch in every handler:
//   res.status(500).json({ success: false, message: error.message }).
pulse::http::HttpResponsePtr serverError(const std::string& message) {
  Json::Value body(Json::objectValue);
  body["success"] = false;
  body["message"] = message;
  return pulse::http::json(drogon::k500InternalServerError, std::move(body));
}

// A `{ success: false, message }` reply with an explicit status code (used for
// the 404 'Alter Ego not found' and the 400 validation responses).
pulse::http::HttpResponsePtr fail(drogon::HttpStatusCode status,
                                  const std::string& message) {
  Json::Value body(Json::objectValue);
  body["success"] = false;
  body["message"] = message;
  return pulse::http::json(status, std::move(body));
}

// AlterEgo.findOne({ user: userId }) — returns the ego document as JSON, or
// std::nullopt when the user has no alter ego (mirrors a null Mongoose result).
// This is a direct collection read the model layer did not expose a helper for
// (getOrCreate would create one, which these handlers must NOT do).
std::optional<Json::Value> findEgoByUser(const std::string& userId) {
  auto userOid = pulse::bsonjson::tryOid(userId);
  if (!userOid) return std::nullopt;  // invalid id matches no document
  auto col = pulse::db::collection(alterego::kCollection);
  auto doc = col.find_one(make_document(kvp("user", *userOid)));
  if (!doc) return std::nullopt;
  return pulse::bsonjson::toJson(doc->view());
}

// JS truthiness for a string body field: present, a string, and non-empty.
bool truthyString(const Json::Value& obj, const char* key) {
  return obj.isObject() && obj.isMember(key) && obj[key].isString() &&
         !obj[key].asString().empty();
}

// JS `message?.trim()` truthiness: present non-empty string after trimming.
bool nonBlankAfterTrim(const std::string& s) {
  return std::any_of(s.begin(), s.end(), [](unsigned char c) {
    return !std::isspace(c);
  });
}

// JS parseInt(str, 10): optional sign + leading digits, NaN (nullopt) on none.
std::optional<long> jsParseInt(const std::string& s) {
  size_t i = 0;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
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
  if (i == start) return std::nullopt;
  return neg ? -value : value;
}

// ── buildSystemPrompt(ego) — ported from src/models/AlterEgo.js. The JS model's
// generateResponse() delegates prompt assembly to this helper; it is part of the
// generate flow (the data model doc fields drive it), so it is reproduced here
// 1:1 over the ego JSON. Reads personality, training, name.
std::string buildSystemPrompt(const Json::Value& ego) {
  const std::string personality =
      ego.isMember("personality") && ego["personality"].isString()
          ? ego["personality"].asString()
          : std::string();
  const std::string name = ego.isMember("name") && ego["name"].isString()
                               ? ego["name"].asString()
                               : std::string();
  const Json::Value training =
      ego.isMember("training") && ego["training"].isObject()
          ? ego["training"]
          : Json::Value(Json::objectValue);

  // const personalityTraits = { ... };  personalityTraits[personality]
  std::string traits;
  if (personality == "friendly")
    traits = "warm, approachable, uses friendly language and emojis";
  else if (personality == "funny")
    traits = "witty, makes puns and jokes, keeps things light";
  else if (personality == "professional")
    traits = "formal, polished, articulate";
  else if (personality == "mysterious")
    traits = "enigmatic, uses cryptic language, intriguing";
  else if (personality == "chill")
    traits = "relaxed, casual, uses slang, laid-back vibes";
  // else: personalityTraits[personality] === undefined -> JS interpolates
  // the string "undefined".
  else
    traits = "undefined";

  std::string prompt = "You are " + name + ", an AI alter ego with a " +
                       personality + " personality. ";
  prompt += "You are " + traits + ". ";

  // Helper: join a JSON string array with a separator (mirrors Array.join).
  auto joinArr = [](const Json::Value& arr, const std::string& sep) {
    std::string out;
    if (!arr.isArray()) return out;
    for (Json::ArrayIndex i = 0; i < arr.size(); ++i) {
      if (i) out += sep;
      out += arr[i].asString();
    }
    return out;
  };

  // if (training.favoriteTopics?.length > 0)
  if (training.isMember("favoriteTopics") &&
      training["favoriteTopics"].isArray() &&
      training["favoriteTopics"].size() > 0) {
    prompt += "You love talking about: " +
              joinArr(training["favoriteTopics"], ", ") + ". ";
  }
  // if (training.humorStyle)
  if (training.isMember("humorStyle") && training["humorStyle"].isString() &&
      !training["humorStyle"].asString().empty()) {
    prompt += "Your humor style: " + training["humorStyle"].asString() + ". ";
  }
  // if (training.phrases?.length > 0)
  if (training.isMember("phrases") && training["phrases"].isArray() &&
      training["phrases"].size() > 0) {
    prompt += "You often say things like: \"" +
              joinArr(training["phrases"], "\", \"") + "\". ";
  }
  // if (training.emojis?.length > 0)
  if (training.isMember("emojis") && training["emojis"].isArray() &&
      training["emojis"].size() > 0) {
    prompt += "You like using these emojis: " +
              joinArr(training["emojis"], " ") + ". ";
  }
  // if (training.hotTakes?.length > 0)
  if (training.isMember("hotTakes") && training["hotTakes"].isArray() &&
      training["hotTakes"].size() > 0) {
    prompt += "Your hot takes include: \"" +
              joinArr(training["hotTakes"], "\", \"") + "\". ";
  }

  prompt += "Keep responses concise (1-3 sentences). Match the user's energy. ";
  prompt +=
      "You are responding on behalf of your owner in a social media app called "
      "Pulse. Never reveal you are an AI.";

  return prompt;
}

// Build the AI context.history from the ego document, mirroring
// generateResponse():
//   history: this.conversations.length > 0
//     ? this.conversations[this.conversations.length - 1]?.messages?.slice(-6) || []
//     : []
pulse::AlterEgoContext buildHistoryContext(const Json::Value& ego) {
  pulse::AlterEgoContext ctx;
  if (!ego.isMember("conversations") || !ego["conversations"].isArray() ||
      ego["conversations"].empty()) {
    return ctx;
  }
  const Json::Value& last = ego["conversations"][ego["conversations"].size() - 1];
  if (!last.isObject() || !last.isMember("messages") ||
      !last["messages"].isArray()) {
    return ctx;  // ?.messages?.slice(-6) || []
  }
  const Json::Value& messages = last["messages"];
  // slice(-6): last up-to-6 elements.
  const Json::ArrayIndex total = messages.size();
  const Json::ArrayIndex start = total > 6 ? total - 6 : 0;
  for (Json::ArrayIndex i = start; i < total; ++i) {
    const Json::Value& m = messages[i];
    pulse::AlterEgoMessage am;
    am.role = m.isObject() && m.isMember("role") && m["role"].isString()
                  ? m["role"].asString()
                  : std::string();
    am.content = m.isObject() && m.isMember("content") && m["content"].isString()
                     ? m["content"].asString()
                     : std::string();
    ctx.history.push_back(std::move(am));
  }
  return ctx;
}

}  // namespace

// ── GET /api/v1/alter-ego/me ─────────────────────────────────────────────────
void AlterEgoController::getMyEgo(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    const std::string userId = authedUserId(req);
    Json::Value ego = alterego::getOrCreate(userId);

    // res.json({ success: true, data: ego });
    callback(pulse::http::ok(std::move(ego)));
  } catch (const std::exception& e) {
    pulse::log::error("Get ego error: {}", e.what());
    callback(serverError(e.what()));
  }
}

// ── PUT /api/v1/alter-ego ────────────────────────────────────────────────────
void AlterEgoController::update(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    const std::string userId = authedUserId(req);
    auto bodyPtr = req->getJsonObject();
    const Json::Value body =
        bodyPtr ? *bodyPtr : Json::Value(Json::objectValue);

    auto egoOpt = findEgoByUser(userId);
    if (!egoOpt) {
      // res.status(404).json({ success:false, message:'Alter Ego not found' })
      callback(fail(drogon::k404NotFound, "Alter Ego not found"));
      return;
    }
    Json::Value ego = *egoOpt;

    // Apply the same conditional field updates the JS did onto the loaded doc,
    // then persist (ego.save()) via a single $set, and return the saved doc.
    bld::document setDoc;
    bool changed = false;

    // if (name) ego.name = name;
    if (truthyString(body, "name")) {
      ego["name"] = body["name"];
      setDoc.append(kvp("name", body["name"].asString()));
      changed = true;
    }
    // if (personality) ego.personality = personality;
    if (truthyString(body, "personality")) {
      ego["personality"] = body["personality"];
      setDoc.append(kvp("personality", body["personality"].asString()));
      changed = true;
    }
    // if (typeof isActive === 'boolean') ego.isActive = isActive;
    if (body.isMember("isActive") && body["isActive"].isBool()) {
      ego["isActive"] = body["isActive"];
      setDoc.append(kvp("isActive", body["isActive"].asBool()));
      changed = true;
    }
    // if (typeof autoReplyDM === 'boolean') ego.autoReplyDM = autoReplyDM;
    if (body.isMember("autoReplyDM") && body["autoReplyDM"].isBool()) {
      ego["autoReplyDM"] = body["autoReplyDM"];
      setDoc.append(kvp("autoReplyDM", body["autoReplyDM"].asBool()));
      changed = true;
    }
    // if (typeof autoReplyComments === 'boolean') ego.autoReplyComments = ...;
    if (body.isMember("autoReplyComments") &&
        body["autoReplyComments"].isBool()) {
      ego["autoReplyComments"] = body["autoReplyComments"];
      setDoc.append(kvp("autoReplyComments", body["autoReplyComments"].asBool()));
      changed = true;
    }

    // await ego.save();  (timestamps:true bumps updatedAt on any save)
    const std::string nowIso = pulse::bsonjson::nowIso8601();
    setDoc.append(kvp("updatedAt", bsoncxx::types::b_date{
                                       std::chrono::system_clock::now()}));
    ego["updatedAt"] = nowIso;
    (void)changed;

    auto col = pulse::db::collection(alterego::kCollection);
    const std::string egoId = ego["_id"].asString();
    col.update_one(make_document(kvp("_id", pulse::bsonjson::oid(egoId))),
                   make_document(kvp("$set", setDoc.extract())));

    // res.json({ success: true, data: ego });
    callback(pulse::http::ok(std::move(ego)));
  } catch (const std::exception& e) {
    pulse::log::error("Update ego error: {}", e.what());
    callback(serverError(e.what()));
  }
}

// ── POST /api/v1/alter-ego/train ─────────────────────────────────────────────
void AlterEgoController::train(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    const std::string userId = authedUserId(req);
    // const trainingData = req.body;
    auto bodyPtr = req->getJsonObject();
    const Json::Value trainingData =
        bodyPtr ? *bodyPtr : Json::Value(Json::objectValue);

    auto egoOpt = findEgoByUser(userId);
    if (!egoOpt) {
      callback(fail(drogon::k404NotFound, "Alter Ego not found"));
      return;
    }

    // await ego.updateTraining(trainingData);
    const std::string egoId = (*egoOpt)["_id"].asString();
    auto updated =
        alterego::updateTraining(pulse::bsonjson::oid(egoId), trainingData);
    // The doc existed (we just found it); updateTraining returns the new doc.
    Json::Value ego = updated ? *updated : *egoOpt;

    // res.json({ success:true, data:{ trainingLevel, training } });
    Json::Value data(Json::objectValue);
    data["trainingLevel"] = ego.isMember("trainingLevel")
                                ? ego["trainingLevel"]
                                : Json::Value(0);
    data["training"] = ego.isMember("training") ? ego["training"]
                                                : Json::Value(Json::objectValue);
    callback(pulse::http::ok(std::move(data)));
  } catch (const std::exception& e) {
    pulse::log::error("Train ego error: {}", e.what());
    callback(serverError(e.what()));
  }
}

// ── POST /api/v1/alter-ego/generate ──────────────────────────────────────────
void AlterEgoController::generateResponse(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    const std::string userId = authedUserId(req);
    auto bodyPtr = req->getJsonObject();
    const Json::Value body =
        bodyPtr ? *bodyPtr : Json::Value(Json::objectValue);

    // const { message, context = {} } = req.body;
    const std::string message =
        body.isMember("message") && body["message"].isString()
            ? body["message"].asString()
            : std::string();
    const Json::Value context =
        body.isMember("context") && body["context"].isObject()
            ? body["context"]
            : Json::Value(Json::objectValue);

    // if (!message?.trim()) -> 400 { success:false, message:'Message required' }
    if (message.empty() || !nonBlankAfterTrim(message)) {
      callback(fail(drogon::k400BadRequest, "Message required"));
      return;
    }

    auto egoOpt = findEgoByUser(userId);
    if (!egoOpt) {
      callback(fail(drogon::k404NotFound, "Alter Ego not found"));
      return;
    }
    Json::Value ego = *egoOpt;

    // const response = await ego.generateResponse(message, context);
    //   -> buildSystemPrompt(ego) + callAI(prompt, message, { ...context,
    //      history: last conversation's last 6 messages }), then persist.
    const std::string systemPrompt = buildSystemPrompt(ego);
    pulse::AlterEgoContext aiCtx = buildHistoryContext(ego);

    const std::string response =
        pulse::alterEgoAI().generateAIResponse(systemPrompt, message, aiCtx);

    // Persistence side-effects (totalReplies++, lastActive, activityLog.unshift,
    // cap 100). context.action || 'dm_reply'; context.targetUserId || null.
    const std::string action =
        context.isMember("action") && context["action"].isString()
            ? context["action"].asString()
            : std::string();
    const std::string targetUserId =
        context.isMember("targetUserId") && context["targetUserId"].isString()
            ? context["targetUserId"].asString()
            : std::string();
    const std::string egoId = ego["_id"].asString();
    alterego::recordReply(pulse::bsonjson::oid(egoId), action, targetUserId,
                          message, response);

    // res.json({ success: true, data: { response } });
    Json::Value data(Json::objectValue);
    data["response"] = response;
    callback(pulse::http::ok(std::move(data)));
  } catch (const std::exception& e) {
    pulse::log::error("Generate response error: {}", e.what());
    callback(serverError(e.what()));
  }
}

// ── POST /api/v1/alter-ego/toggle ────────────────────────────────────────────
void AlterEgoController::toggle(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    const std::string userId = authedUserId(req);

    auto egoOpt = findEgoByUser(userId);
    if (!egoOpt) {
      callback(fail(drogon::k404NotFound, "Alter Ego not found"));
      return;
    }
    Json::Value ego = *egoOpt;

    const bool isActive =
        ego.isMember("isActive") && ego["isActive"].isBool()
            ? ego["isActive"].asBool()
            : false;
    const long trainingLevel =
        ego.isMember("trainingLevel") && ego["trainingLevel"].isNumeric()
            ? ego["trainingLevel"].asLargestInt()
            : 0;

    // if (!ego.isActive && ego.trainingLevel < 1) -> 400
    if (!isActive && trainingLevel < 1) {
      callback(fail(drogon::k400BadRequest,
                    "Complete at least 1 training step to activate"));
      return;
    }

    // ego.isActive = !ego.isActive;  await ego.save();
    const bool newActive = !isActive;
    ego["isActive"] = newActive;
    const std::string nowIso = pulse::bsonjson::nowIso8601();
    ego["updatedAt"] = nowIso;

    auto col = pulse::db::collection(alterego::kCollection);
    const std::string egoId = ego["_id"].asString();
    col.update_one(
        make_document(kvp("_id", pulse::bsonjson::oid(egoId))),
        make_document(kvp("$set", make_document(kvp("isActive", newActive),
                                                kvp("updatedAt", bsoncxx::types::b_date{
                                                    std::chrono::system_clock::now()})))));

    // res.json({ success: true, data: { isActive: ego.isActive } });
    Json::Value data(Json::objectValue);
    data["isActive"] = newActive;
    callback(pulse::http::ok(std::move(data)));
  } catch (const std::exception& e) {
    pulse::log::error("Toggle ego error: {}", e.what());
    callback(serverError(e.what()));
  }
}

// ── GET /api/v1/alter-ego/stats ──────────────────────────────────────────────
void AlterEgoController::getStats(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    const std::string userId = authedUserId(req);

    auto egoOpt = findEgoByUser(userId);
    if (!egoOpt) {
      callback(fail(drogon::k404NotFound, "Alter Ego not found"));
      return;
    }
    Json::Value ego = *egoOpt;

    // res.json({ success:true, data:{ totalReplies, trainingLevel, isActive,
    //   lastActive, personality, guessWhoStats, aiProvider } });
    Json::Value data(Json::objectValue);
    data["totalReplies"] =
        ego.isMember("totalReplies") ? ego["totalReplies"] : Json::Value(0);
    data["trainingLevel"] =
        ego.isMember("trainingLevel") ? ego["trainingLevel"] : Json::Value(0);
    data["isActive"] =
        ego.isMember("isActive") ? ego["isActive"] : Json::Value(false);
    // lastActive: ego.lastActive. When the Date is unset Mongoose has no such
    // field, so ego.lastActive is undefined and JSON.stringify drops the key —
    // omit it here to match (only emit it when the doc actually carries it).
    if (ego.isMember("lastActive") && !ego["lastActive"].isNull()) {
      data["lastActive"] = ego["lastActive"];
    }
    data["personality"] = ego.isMember("personality")
                              ? ego["personality"]
                              : Json::Value(Json::nullValue);
    data["guessWhoStats"] = ego.isMember("guessWhoStats")
                                ? ego["guessWhoStats"]
                                : Json::Value(Json::objectValue);
    // aiProvider: getActiveProvider()
    data["aiProvider"] = pulse::alterEgoAI().getActiveProvider();
    callback(pulse::http::ok(std::move(data)));
  } catch (const std::exception& e) {
    pulse::log::error("Get stats error: {}", e.what());
    callback(serverError(e.what()));
  }
}

// ── POST /api/v1/alter-ego/learn ─────────────────────────────────────────────
void AlterEgoController::learn(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    const std::string userId = authedUserId(req);
    auto bodyPtr = req->getJsonObject();
    const Json::Value body =
        bodyPtr ? *bodyPtr : Json::Value(Json::objectValue);

    // const { trigger, response } = req.body;
    const std::string trigger =
        body.isMember("trigger") && body["trigger"].isString()
            ? body["trigger"].asString()
            : std::string();
    const std::string response =
        body.isMember("response") && body["response"].isString()
            ? body["response"].asString()
            : std::string();

    // if (!trigger || !response) -> 400 { message:'Trigger and response required' }
    if (!truthyString(body, "trigger") || !truthyString(body, "response")) {
      callback(fail(drogon::k400BadRequest, "Trigger and response required"));
      return;
    }

    auto egoOpt = findEgoByUser(userId);
    if (!egoOpt) {
      callback(fail(drogon::k404NotFound, "Alter Ego not found"));
      return;
    }

    // await ego.learnFromUser(trigger, response);
    const std::string egoId = (*egoOpt)["_id"].asString();
    alterego::learnFromUser(pulse::bsonjson::oid(egoId), trigger, response);

    // res.json({ success: true, message: 'Learned!' });
    Json::Value extra(Json::objectValue);
    extra["message"] = "Learned!";
    callback(pulse::http::success(std::move(extra)));
  } catch (const std::exception& e) {
    pulse::log::error("Learn error: {}", e.what());
    callback(serverError(e.what()));
  }
}

// ── GET /api/v1/alter-ego/activity ───────────────────────────────────────────
void AlterEgoController::getActivityLog(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    const std::string userId = authedUserId(req);

    // const { page = 1, limit = 20 } = req.query;
    const std::string pageRaw = req->getParameter("page");
    const std::string limitRaw = req->getParameter("limit");
    // parseInt(page) / parseInt(limit) — when the query param is absent the JS
    // default (1 / 20) applies; parseInt of the default number is itself.
    const long page = pageRaw.empty() ? 1 : jsParseInt(pageRaw).value_or(0);
    const long limit = limitRaw.empty() ? 20 : jsParseInt(limitRaw).value_or(0);
    // const skip = (parseInt(page) - 1) * parseInt(limit);
    const long skip = (page - 1) * limit;

    auto egoOpt = findEgoByUser(userId);
    if (!egoOpt) {
      callback(fail(drogon::k404NotFound, "Alter Ego not found"));
      return;
    }
    Json::Value ego = *egoOpt;

    const Json::Value& activityLog =
        ego.isMember("activityLog") && ego["activityLog"].isArray()
            ? ego["activityLog"]
            : Json::Value(Json::arrayValue);
    const long total = static_cast<long>(activityLog.size());

    // const activities = ego.activityLog.slice(skip, skip + parseInt(limit));
    // Array.slice clamps negative/over-range indices.
    long sliceStart = skip;
    long sliceEnd = skip + limit;
    if (sliceStart < 0) sliceStart = std::max<long>(total + sliceStart, 0);
    if (sliceStart > total) sliceStart = total;
    if (sliceEnd < 0) sliceEnd = std::max<long>(total + sliceEnd, 0);
    if (sliceEnd > total) sliceEnd = total;

    Json::Value activities(Json::arrayValue);
    for (long i = sliceStart; i < sliceEnd; ++i) {
      activities.append(activityLog[static_cast<Json::ArrayIndex>(i)]);
    }

    // res.json({ success:true, data:activities, pagination:{...} });
    Json::Value body(Json::objectValue);
    body["success"] = true;
    body["data"] = std::move(activities);
    Json::Value pagination(Json::objectValue);
    pagination["page"] = static_cast<Json::Int64>(page);
    pagination["limit"] = static_cast<Json::Int64>(limit);
    pagination["total"] = static_cast<Json::Int64>(total);
    pagination["hasMore"] = (skip + limit) < total;  // skip+limit < length
    body["pagination"] = std::move(pagination);
    callback(pulse::http::json(drogon::k200OK, std::move(body)));
  } catch (const std::exception& e) {
    pulse::log::error("Get activity log error: {}", e.what());
    callback(serverError(e.what()));
  }
}

// ── POST /api/v1/alter-ego/guess ─────────────────────────────────────────────
void AlterEgoController::recordGuess(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    const std::string userId = authedUserId(req);
    auto bodyPtr = req->getJsonObject();
    const Json::Value body =
        bodyPtr ? *bodyPtr : Json::Value(Json::objectValue);

    // if (typeof guessedCorrectly !== 'boolean') -> 400
    if (!body.isMember("guessedCorrectly") ||
        !body["guessedCorrectly"].isBool()) {
      callback(fail(drogon::k400BadRequest,
                    "guessedCorrectly (boolean) required"));
      return;
    }
    const bool guessedCorrectly = body["guessedCorrectly"].asBool();

    auto egoOpt = findEgoByUser(userId);
    if (!egoOpt) {
      callback(fail(drogon::k404NotFound, "Alter Ego not found"));
      return;
    }

    // const stats = await ego.recordGuessResult(guessedCorrectly);
    const std::string egoId = (*egoOpt)["_id"].asString();
    auto stats = alterego::recordGuessResult(pulse::bsonjson::oid(egoId),
                                             guessedCorrectly);

    // res.json({ success: true, data: stats });
    callback(pulse::http::ok(stats ? *stats : Json::Value(Json::objectValue)));
  } catch (const std::exception& e) {
    pulse::log::error("Record guess error: {}", e.what());
    callback(serverError(e.what()));
  }
}

// ── GET /api/v1/alter-ego/ai-status ──────────────────────────────────────────
void AlterEgoController::getAIStatus(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    // res.json({ success:true, data:{ provider, isAIEnabled } });
    const std::string provider = pulse::alterEgoAI().getActiveProvider();
    Json::Value data(Json::objectValue);
    data["provider"] = provider;
    data["isAIEnabled"] = (provider != "template");
    callback(pulse::http::ok(std::move(data)));
  } catch (const std::exception& e) {
    callback(serverError(e.what()));
  }
}
