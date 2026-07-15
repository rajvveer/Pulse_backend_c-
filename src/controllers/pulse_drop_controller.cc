// pulse_drop_controller.cc — port of src/controllers/pulseDropController.js
// (route group src/routes/pulseDropRoutes.js, mounted at /api/v1/pulse-drops).
//
// Ground truth: pulseDropController.js. Every handler mirrors its Express
// counterpart: the same validation, the same res.status(...).json({...}) shapes
// (which here use the legacy { success:false, message } error shape — NOT the
// { error, code } shape — exactly like the JS source), and the same try/catch
// translating any thrown error into 500 { success:false, message: error.message }.
//
// Model logic is NOT reimplemented here — the handlers call the existing ports
// in pulse::models::pulsedrop / pulse::models::post and read/insert documents
// through pulse::db::collection(...) (the JS used PulseDrop.findById / Post.create
// / Post.find directly in the controller, so the equivalent direct collection
// access lives here too).
#include "pulse/controllers/pulse_drop_controller.hpp"

#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/http_response.hpp"
#include "pulse/logger.hpp"
#include "pulse/models/pulsedrop.hpp"
#include "pulse/models/post.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/oid.hpp>

#include <mongocxx/collection.hpp>
#include <mongocxx/cursor.hpp>
#include <mongocxx/options/find.hpp>

#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <chrono>

namespace pulse::controllers {

namespace {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;

// Render epoch-millis as an ISO-8601 UTC string (YYYY-MM-DDTHH:MM:SS.mmmZ),
// matching the format pulse::models::pulsedrop stamps for its date fields. Used
// to materialize createDrop's `expiresAt: new Date(Date.now() + durationMs)`.
std::string isoFromMillis(long long ms) {
  std::time_t secs = static_cast<std::time_t>(ms / 1000);
  int millis = static_cast<int>(ms % 1000);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &secs);
#else
  gmtime_r(&secs, &tm);
#endif
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                tm.tm_min, tm.tm_sec, millis);
  return std::string(buf);
}

// JS error shape used throughout pulseDropController.js:
//   res.status(code).json({ success: false, message })
// This is intentionally NOT pulse::http::error (which adds an "error"/"code"
// pair). We build the exact { success:false, message } body the JS emits.
drogon::HttpResponsePtr failMessage(drogon::HttpStatusCode code,
                                    const std::string& message) {
  Json::Value body(Json::objectValue);
  body["success"] = false;
  body["message"] = message;
  return pulse::http::json(code, std::move(body));
}

// JS success shape: res.json({ success: true, data }) at the given status.
drogon::HttpResponsePtr okData(Json::Value data,
                               drogon::HttpStatusCode code = drogon::k200OK) {
  return pulse::http::ok(std::move(data), code);
}

// parseInt(value, 10) with a default — mirrors `const { x = def } = req.query`
// followed by parseInt(x). Returns `def` when the parameter is absent; for a
// present-but-non-numeric value, std::strtol yields 0 (parity is best-effort
// here as the JS path is only ever hit with numeric query strings).
int queryInt(const drogon::HttpRequestPtr& req, const char* name, int def) {
  std::string v = req->getParameter(name);
  if (v.empty()) return def;
  char* end = nullptr;
  long n = std::strtol(v.c_str(), &end, 10);
  if (end == v.c_str() || *end != '\0') return def;
  if (std::string(name) == "page")
    return static_cast<int>(std::clamp<long>(n, 1, 1000000));
  return static_cast<int>(std::clamp<long>(n, 1, 100));
}

// Read the authenticated userId set by AuthFilter on the request attributes.
std::string authUserId(const drogon::HttpRequestPtr& req) {
  try {
    auto user = req->getAttributes()->get<Json::Value>("user");
    if (user.isObject() && user.isMember("userId") && user["userId"].isString())
      return user["userId"].asString();
  } catch (...) {
  }
  return std::string();
}

// Read a string field from a JSON body object (absent/non-string -> "").
std::string bodyString(const std::shared_ptr<Json::Value>& body, const char* key) {
  if (body && body->isObject() && body->isMember(key) && (*body)[key].isString())
    return (*body)[key].asString();
  return std::string();
}

} // namespace

// =============================================================================
// GET /api/v1/pulse-drops  ->  exports.getActive
//
//   const { limit = 20 } = req.query;
//   const drops = await PulseDrop.getActiveDrops(parseInt(limit));
//   const withTime = drops.map(d => ({ ...d.toObject(), timeRemaining: d.getTimeRemaining() }));
//   res.json({ success: true, data: withTime });
// catch -> res.status(500).json({ success:false, message: error.message })
// =============================================================================
void PulseDropController::getActive(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    const int limit = queryInt(req, "limit", 20);

    // PulseDrop.getActiveDrops(limit) — find/sort/limit query (already ported).
    Json::Value drops = pulse::models::pulsedrop::getActiveDrops(limit);

    // drops.map(d => ({ ...d.toObject(), timeRemaining: d.getTimeRemaining() }))
    Json::Value withTime(Json::arrayValue);
    for (const auto& d : drops) {
      Json::Value entry = d;  // ...d.toObject() (full doc, version key included)
      std::string expiresAt = entry.isMember("expiresAt") && entry["expiresAt"].isString()
                                  ? entry["expiresAt"].asString()
                                  : std::string();
      entry["timeRemaining"] = pulse::models::pulsedrop::getTimeRemaining(expiresAt);
      withTime.append(std::move(entry));
    }

    callback(okData(std::move(withTime)));
  } catch (const std::exception& e) {
    pulse::log::error("Get drops error: {}", e.what());
    callback(failMessage(drogon::k500InternalServerError, e.what()));
  }
}

// =============================================================================
// GET /api/v1/pulse-drops/{dropId}  ->  exports.getById
//
//   const drop = await PulseDrop.findById(dropId)
//       .populate('triggerPost', ...).populate('featuredResponses', ...)
//       .populate('participants.user', ...);
//   if (!drop) return 404 { success:false, message:'Drop not found' };
//   res.json({ success:true, data: { ...drop.toObject(), timeRemaining: drop.getTimeRemaining() } });
// catch -> 500 { success:false, message: error.message }
// =============================================================================
void PulseDropController::getById(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string dropId) {
  try {
    auto oid = pulse::bsonjson::tryOid(dropId);
    if (!oid) {
      callback(failMessage(drogon::k400BadRequest, "Invalid drop ID"));
      return;
    }

    auto col = pulse::db::collection(pulse::models::pulsedrop::kCollection);
    auto found = col.find_one(make_document(kvp("_id", *oid)));

    // if (!drop) -> 404
    if (!found) {
      callback(failMessage(drogon::k404NotFound, "Drop not found"));
      return;
    }

    // { ...drop.toObject(), timeRemaining }
    Json::Value data = pulse::bsonjson::toJson(found->view());
    std::string expiresAt = data.isMember("expiresAt") && data["expiresAt"].isString()
                                ? data["expiresAt"].asString()
                                : std::string();
    data["timeRemaining"] = pulse::models::pulsedrop::getTimeRemaining(expiresAt);

    callback(okData(std::move(data)));
  } catch (const std::exception& e) {
    pulse::log::error("Get drop error: {}", e.what());
    callback(failMessage(drogon::k500InternalServerError, e.what()));
  }
}

// =============================================================================
// POST /api/v1/pulse-drops/{dropId}/join  ->  exports.join
//
//   const { responsePostId } = req.body;
//   const userId = req.user.userId;
//   const drop = await PulseDrop.findById(dropId);
//   if (!drop) return 404 { success:false, message:'Drop not found' };
//   if (drop.status !== 'active') return 400 { success:false, message:'This drop has expired' };
//   await drop.join(userId, responsePostId);
//   res.json({ success:true, data: { participantCount, responseCount } });
// catch -> 500 { success:false, message: error.message }
// =============================================================================
void PulseDropController::join(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string dropId) {
  try {
    auto body = req->getJsonObject();
    const std::string responsePostId = bodyString(body, "responsePostId");
    const std::string userId = authUserId(req);

    auto oid = pulse::bsonjson::tryOid(dropId);
    if (!oid) {
      callback(failMessage(drogon::k400BadRequest, "Invalid drop ID"));
      return;
    }

    auto col = pulse::db::collection(pulse::models::pulsedrop::kCollection);
    auto found = col.find_one(make_document(kvp("_id", *oid)));

    // if (!drop) -> 404
    if (!found) {
      callback(failMessage(drogon::k404NotFound, "Drop not found"));
      return;
    }

    // if (drop.status !== 'active') -> 400
    std::string status;
    {
      auto view = found->view();
      auto it = view.find("status");
      if (it != view.end() && it->type() == bsoncxx::type::k_string)
        status = std::string(it->get_string().value);
    }
    if (status != pulse::models::pulsedrop::kStatusActive) {
      callback(failMessage(drogon::k400BadRequest, "This drop has expired"));
      return;
    }

    if (!responsePostId.empty()) {
      auto responseOid = pulse::bsonjson::tryOid(responsePostId);
      auto userOid = pulse::bsonjson::tryOid(userId);
      if (!responseOid || !userOid) {
        callback(failMessage(drogon::k400BadRequest, "Invalid response post"));
        return;
      }
      // A response reference must be an active post owned by the caller and
      // created for this drop. This prevents arbitrary ObjectId attachment and
      // response-count inflation using unrelated posts.
      auto postCol = pulse::db::collection(pulse::models::post::kCollection);
      auto response = postCol.find_one(make_document(
          kvp("_id", *responseOid), kvp("author", *userOid),
          kvp("isActive", true), kvp("metadata.pulseDropId", dropId)));
      if (!response) {
        callback(failMessage(drogon::k400BadRequest,
                             "Response post does not belong to this drop"));
        return;
      }
    }

    // await drop.join(userId, responsePostId);  (responsePostId "" == JS null)
    auto result =
        pulse::models::pulsedrop::join(dropId, userId, responsePostId);
    if (!result.found) {
      callback(failMessage(drogon::k409Conflict,
                           "Drop is no longer active"));
      return;
    }

    // res.json({ success:true, data: { participantCount, responseCount } })
    // The JS reads drop.participantCount / drop.responseCount AFTER join()
    // mutates the in-memory document — JoinResult carries those post-join values.
    Json::Value data(Json::objectValue);
    data["participantCount"] = static_cast<Json::Int64>(result.participantCount);
    data["responseCount"] = static_cast<Json::Int64>(result.responseCount);

    callback(okData(std::move(data)));
  } catch (const std::exception& e) {
    pulse::log::error("Join drop error: {}", e.what());
    callback(failMessage(drogon::k500InternalServerError, e.what()));
  }
}

// =============================================================================
// POST /api/v1/pulse-drops/{dropId}/respond  ->  exports.createResponse
//
//   const { content, media } = req.body;
//   const userId = req.user.userId;
//   const drop = await PulseDrop.findById(dropId);
//   if (!drop || drop.status !== 'active') return 404 { success:false, message:'Active drop not found' };
//   const post = await Post.create({ author:userId, content, media,
//                                    type:'pulse_drop_response',
//                                    metadata:{ pulseDropId:dropId } });
//   await drop.join(userId, post._id);
//   res.status(201).json({ success:true, data: post });
// catch -> 500 { success:false, message: error.message }
// =============================================================================
void PulseDropController::createResponse(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string dropId) {
  try {
    auto body = req->getJsonObject();
    const std::string userId = authUserId(req);

    if (!body || !body->isObject() || !body->isMember("content") ||
        !(*body)["content"].isObject()) {
      callback(failMessage(drogon::k400BadRequest,
                           "Response content is required"));
      return;
    }
    const Json::Value& responseContent = (*body)["content"];
    const bool hasText = responseContent.isMember("text") &&
                         responseContent["text"].isString() &&
                         !responseContent["text"].asString().empty();
    const bool hasContentMedia = responseContent.isMember("media") &&
                                 responseContent["media"].isArray() &&
                                 !responseContent["media"].empty();
    const bool hasTopMedia = body->isMember("media") &&
                             (*body)["media"].isArray() &&
                             !(*body)["media"].empty();
    if ((responseContent.isMember("text") &&
         (!responseContent["text"].isString() ||
          responseContent["text"].asString().size() > 5000)) ||
        (responseContent.isMember("media") &&
         (!responseContent["media"].isArray() ||
          responseContent["media"].size() > 10)) ||
        (body->isMember("media") &&
         (!(*body)["media"].isArray() || (*body)["media"].size() > 10)) ||
        (!hasText && !hasContentMedia && !hasTopMedia)) {
      callback(failMessage(drogon::k400BadRequest,
                           "Invalid response content"));
      return;
    }

    auto oid = pulse::bsonjson::tryOid(dropId);
    if (!oid) {
      callback(failMessage(drogon::k400BadRequest, "Invalid drop ID"));
      return;
    }

    auto dropCol = pulse::db::collection(pulse::models::pulsedrop::kCollection);
    auto found = dropCol.find_one(make_document(
        kvp("_id", *oid), kvp("status", pulse::models::pulsedrop::kStatusActive),
        kvp("expiresAt", make_document(kvp(
            "$gt", bsoncxx::types::b_date{std::chrono::system_clock::now()})))));

    // if (!drop || drop.status !== 'active') -> 404 'Active drop not found'
    if (!found) {
      callback(failMessage(drogon::k404NotFound, "Active drop not found"));
      return;
    }

    // Post.create({ author, content, media, type:'pulse_drop_response',
    //               metadata:{ pulseDropId } })
    //
    // Build the insert JSON exactly as the JS object literal, then apply Post
    // schema defaults (post::applyDefaults) before persisting — the same
    // defaults Mongoose stamps on Post.create.
    Json::Value postDoc(Json::objectValue);
    postDoc["author"] = userId;
    // content / media are passed straight from the request body as the JS does.
    if (body && body->isObject() && body->isMember("content"))
      postDoc["content"] = (*body)["content"];
    if (body && body->isObject() && body->isMember("media"))
      postDoc["media"] = (*body)["media"];
    postDoc["type"] = "pulse_drop_response";
    {
      Json::Value metadata(Json::objectValue);
      metadata["pulseDropId"] = dropId;
      postDoc["metadata"] = metadata;
    }

    postDoc = pulse::models::post::applyDefaults(std::move(postDoc));

    // Persist. author must be stored as a real ObjectId; the remaining fields
    // are converted from JSON. (bsonjson::fromJson leaves "author" as a string,
    // so we splice the ObjectId in explicitly.)
    auto postCol = pulse::db::collection(pulse::models::post::kCollection);

    // Build the BSON insert document from the prepared JSON, overriding author
    // with an ObjectId.
    Json::Value forBson = postDoc;
    forBson.removeMember("author");
    forBson.removeMember("createdAt");
    forBson.removeMember("updatedAt");
    auto baseDoc = pulse::bsonjson::fromJson(forBson);

    bld::document insert{};
    insert.append(kvp("author", pulse::bsonjson::oid(userId)));
    for (const auto& el : baseDoc.view()) {
      insert.append(kvp(el.key(), el.get_value()));
    }
    const auto postNow = bsoncxx::types::b_date{std::chrono::system_clock::now()};
    insert.append(kvp("createdAt", postNow));
    insert.append(kvp("updatedAt", postNow));
    auto insertDoc = insert.extract();
    auto inserted = postCol.insert_one(insertDoc.view());

    // Reconstruct the created post as JSON (with its generated _id), matching the
    // Mongoose document returned by Post.create and shipped via res.json(data).
    Json::Value post = postDoc;
    std::string postIdHex;
    if (inserted && inserted->inserted_id().type() == bsoncxx::type::k_oid) {
      postIdHex = pulse::bsonjson::oidToHex(inserted->inserted_id().get_oid().value);
      post["_id"] = postIdHex;
    }
    post["author"] = userId;

    // await drop.join(userId, post._id);
    auto joinResult = pulse::models::pulsedrop::join(dropId, userId, postIdHex);
    if (!joinResult.found) {
      if (auto postOid = pulse::bsonjson::tryOid(postIdHex)) {
        postCol.delete_one(make_document(kvp("_id", *postOid),
                                         kvp("author", pulse::bsonjson::oid(userId))));
      }
      callback(failMessage(drogon::k409Conflict,
                           "Drop is no longer active"));
      return;
    }

    // res.status(201).json({ success:true, data: post })
    callback(okData(std::move(post), drogon::k201Created));
  } catch (const std::exception& e) {
    pulse::log::error("Create response error: {}", e.what());
    callback(failMessage(drogon::k500InternalServerError, e.what()));
  }
}

// =============================================================================
// GET /api/v1/pulse-drops/{dropId}/responses  ->  exports.getResponses
//
//   const { page = 1, limit = 20 } = req.query;
//   const drop = await PulseDrop.findById(dropId);
//   if (!drop) return 404 { success:false, message:'Drop not found' };
//   const responseIds = drop.participants.filter(p => p.response).map(p => p.response);
//   const responses = await Post.find({ _id: { $in: responseIds } })
//       .sort({ 'stats.likes': -1 })
//       .skip((page - 1) * limit)
//       .limit(parseInt(limit))
//       .populate('author', 'username profile.avatar');
//   res.json({ success:true, data: responses });
// catch -> 500 { success:false, message: error.message }
// =============================================================================
void PulseDropController::getResponses(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string dropId) {
  try {
    const int page = queryInt(req, "page", 1);
    const int limit = queryInt(req, "limit", 20);

    auto oid = pulse::bsonjson::tryOid(dropId);
    if (!oid) {
      callback(failMessage(drogon::k400BadRequest, "Invalid drop ID"));
      return;
    }

    auto dropCol = pulse::db::collection(pulse::models::pulsedrop::kCollection);
    auto found = dropCol.find_one(make_document(kvp("_id", *oid)));

    // if (!drop) -> 404
    if (!found) {
      callback(failMessage(drogon::k404NotFound, "Drop not found"));
      return;
    }

    // responseIds = drop.participants.filter(p => p.response).map(p => p.response)
    bld::array responseIds{};
    {
      auto view = found->view();
      auto it = view.find("participants");
      if (it != view.end() && it->type() == bsoncxx::type::k_array) {
        for (const auto& el : it->get_array().value) {
          if (el.type() != bsoncxx::type::k_document) continue;
          auto pdoc = el.get_document().value;
          auto rIt = pdoc.find("response");
          if (rIt != pdoc.end() && rIt->type() == bsoncxx::type::k_oid) {
            // truthy p.response (a real ObjectId; null is filtered out).
            responseIds.append(rIt->get_oid().value);
          }
        }
      }
    }

    // Post.find({ _id: { $in: responseIds } })
    //   .sort({ 'stats.likes': -1 }).skip((page-1)*limit).limit(limit)
    auto filter = make_document(
        kvp("_id", make_document(kvp("$in", responseIds.extract()))),
        kvp("isActive", true));

    mongocxx::options::find opts{};
    opts.sort(make_document(kvp("stats.likes", -1)));
    opts.skip((static_cast<std::int64_t>(page) - 1) * limit);
    opts.limit(limit);

    Json::Value responses(Json::arrayValue);
    auto postCol = pulse::db::collection(pulse::models::post::kCollection);
    auto cursor = postCol.find(filter.view(), opts);
    for (const auto& doc : cursor) {
      responses.append(pulse::bsonjson::toJson(doc));
    }

    // res.json({ success:true, data: responses })
    callback(okData(std::move(responses)));
  } catch (const std::exception& e) {
    pulse::log::error("Get responses error: {}", e.what());
    callback(failMessage(drogon::k500InternalServerError, e.what()));
  }
}

// =============================================================================
// POST /api/v1/pulse-drops/create  ->  exports.createDrop   (requireAdmin)
//
//   const { title, description, coverImage, hashtags, durationHours = 24 } = req.body;
//   const drop = await PulseDrop.create({
//       title, description, coverImage,
//       hashtags: hashtags || [],
//       triggerType: 'manual',
//       expiresAt: new Date(Date.now() + durationHours * 60 * 60 * 1000)
//   });
//   res.status(201).json({ success: true, data: drop });
// catch -> 500 { success:false, message: error.message }
// =============================================================================
void PulseDropController::createDrop(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    auto body = req->getJsonObject();

    // durationHours = 24 default.
    double durationHours = 24;
    if (body && body->isObject() && body->isMember("durationHours") &&
        (*body)["durationHours"].isNumeric()) {
      durationHours = (*body)["durationHours"].asDouble();
    }
    if (!body || !body->isObject() || !body->isMember("title") ||
        !(*body)["title"].isString() || (*body)["title"].asString().empty() ||
        (*body)["title"].asString().size() > 120) {
      callback(failMessage(drogon::k400BadRequest,
                           "A title of at most 120 characters is required"));
      return;
    }
    if (!std::isfinite(durationHours) || durationHours < 1.0 ||
        durationHours > 720.0) {
      callback(failMessage(drogon::k400BadRequest,
                           "durationHours must be between 1 and 720"));
      return;
    }
    if (body->isMember("description") &&
        (!(*body)["description"].isString() ||
         (*body)["description"].asString().size() > 2000)) {
      callback(failMessage(drogon::k400BadRequest, "Invalid description"));
      return;
    }
    if (body->isMember("coverImage") &&
        (!(*body)["coverImage"].isString() ||
         (*body)["coverImage"].asString().size() > 2048)) {
      callback(failMessage(drogon::k400BadRequest, "Invalid cover image"));
      return;
    }
    if (body->isMember("hashtags")) {
      if (!(*body)["hashtags"].isArray() || (*body)["hashtags"].size() > 20) {
        callback(failMessage(drogon::k400BadRequest, "Invalid hashtags"));
        return;
      }
      for (const auto& hashtag : (*body)["hashtags"]) {
        if (!hashtag.isString() || hashtag.asString().size() > 64) {
          callback(failMessage(drogon::k400BadRequest, "Invalid hashtags"));
          return;
        }
      }
    }

    // Build the insert JSON literal, then apply schema defaults (which also
    // stamps participantCount/responseCount/startsAt/status/etc.). We override
    // expiresAt from durationHours and triggerType='manual' as the JS does.
    Json::Value doc(Json::objectValue);
    if (body && body->isObject()) {
      if (body->isMember("title")) doc["title"] = (*body)["title"];
      if (body->isMember("description")) doc["description"] = (*body)["description"];
      if (body->isMember("coverImage")) doc["coverImage"] = (*body)["coverImage"];
    }
    // hashtags: hashtags || []
    if (body && body->isObject() && body->isMember("hashtags") &&
        (*body)["hashtags"].isArray()) {
      doc["hashtags"] = (*body)["hashtags"];
    } else {
      doc["hashtags"] = Json::Value(Json::arrayValue);
    }
    doc["triggerType"] = pulse::models::pulsedrop::kTriggerTypeManual;

    // expiresAt: new Date(Date.now() + durationHours * 60*60*1000)
    // Set before applyDefaults so the default (now + 24h) does not overwrite it.
    long long expiresMs =
        pulse::bsonjson::nowMillis() +
        static_cast<long long>(durationHours * 60.0 * 60.0 * 1000.0);
    doc["expiresAt"] = isoFromMillis(expiresMs);

    doc = pulse::models::pulsedrop::applyDefaults(std::move(doc));

    // Persist lifecycle fields as BSON Dates so active-range queries and expiry
    // jobs use the same schema as the Node backend.
    Json::Value nonDates = doc;
    nonDates.removeMember("startsAt");
    nonDates.removeMember("expiresAt");
    nonDates.removeMember("createdAt");
    nonDates.removeMember("updatedAt");
    auto baseDoc = pulse::bsonjson::fromJson(nonDates);
    bld::document insert{};
    for (const auto& el : baseDoc.view()) insert.append(kvp(el.key(), el.get_value()));
    const auto now = std::chrono::system_clock::now();
    insert.append(kvp("startsAt", bsoncxx::types::b_date{now}));
    insert.append(kvp("expiresAt", bsoncxx::types::b_date{
        std::chrono::milliseconds(expiresMs)}));
    insert.append(kvp("createdAt", bsoncxx::types::b_date{now}));
    insert.append(kvp("updatedAt", bsoncxx::types::b_date{now}));
    auto insertDoc = insert.extract();
    auto col = pulse::db::collection(pulse::models::pulsedrop::kCollection);
    auto inserted = col.insert_one(insertDoc.view());

    Json::Value out = doc;
    if (inserted && inserted->inserted_id().type() == bsoncxx::type::k_oid) {
      out["_id"] =
          pulse::bsonjson::oidToHex(inserted->inserted_id().get_oid().value);
    }

    // res.status(201).json({ success:true, data: drop })
    callback(okData(std::move(out), drogon::k201Created));
  } catch (const std::exception& e) {
    pulse::log::error("Create drop error: {}", e.what());
    callback(failMessage(drogon::k500InternalServerError, e.what()));
  }
}

} // namespace pulse::controllers
