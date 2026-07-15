// whisper_controller.cc — implementation of WhisperController (see
// whisper_controller.hpp). Ground truth: src/controllers/whisperController.js
// and src/routes/whisperRoutes.js.
//
// 1:1 functional parity with the Express handlers:
//   * same input validation (same checks, same status codes, same message text),
//   * same res.json shapes — note these endpoints use { success, message } /
//     { success, data } bodies (NOT the { success, error, code } error contract),
//   * same DB/model logic, delegated to pulse::models::whisper (getNearby, vote,
//     addReply) and direct collection access for create/report (the JS used
//     Whisper.create / whisper.save, for which the model exposes applyDefaults +
//     sanitizeForOutput but no generic create helper).
#include "pulse/controllers/whisper_controller.hpp"

#include "pulse/http_response.hpp"
#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"
#include "pulse/models/whisper.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/oid.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/options/find_one_and_update.hpp>

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>

using namespace drogon;

namespace pulse::controllers {

namespace {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;
using bld::make_array;

// ---------------------------------------------------------------------------
// Response helpers — the whisper controller speaks the legacy
// { success: false, message } / { success: true, data | message } body shape,
// not the { success, error, code } contract used by http::error. Build those
// shapes verbatim so status codes and field names match the JS res.json.
// ---------------------------------------------------------------------------

// res.status(code).json({ success: false, message })
HttpResponsePtr msgError(HttpStatusCode code, const std::string& message) {
  Json::Value body(Json::objectValue);
  body["success"] = false;
  body["message"] = message;
  return pulse::http::json(code, std::move(body));
}

// res.status(code).json({ success: true, data })  (mirrors http::ok shape)
HttpResponsePtr dataOk(Json::Value data, HttpStatusCode code = k200OK) {
  return pulse::http::ok(std::move(data), code);
}

// res.json({ success: true, message })
HttpResponsePtr msgOk(const std::string& message) {
  Json::Value body(Json::objectValue);
  body["success"] = true;
  body["message"] = message;
  return pulse::http::json(k200OK, std::move(body));
}

// ---------------------------------------------------------------------------
// Local helpers ported from the JS controller.
// ---------------------------------------------------------------------------

// Helper: Calculate distance between two points (Haversine). Ported verbatim
// from whisperController.js calculateDistance(lat1, lng1, lat2, lng2).
double calculateDistance(double lat1, double lng1, double lat2, double lng2) {
  const double R = 6371.0;  // Earth's radius in km
  const double pi = 3.14159265358979323846;
  const double dLat = (lat2 - lat1) * pi / 180.0;
  const double dLng = (lng2 - lng1) * pi / 180.0;
  const double a = std::sin(dLat / 2.0) * std::sin(dLat / 2.0) +
                   std::cos(lat1 * pi / 180.0) * std::cos(lat2 * pi / 180.0) *
                       std::sin(dLng / 2.0) * std::sin(dLng / 2.0);
  const double c = 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
  return R * c;
}

// `${dist.toFixed(1)} km` — one-decimal fixed formatting, matching JS toFixed(1).
std::string distanceLabel(double distKm) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.1f km", distKm);
  return std::string(buf);
}

// JS: !content?.trim() — truthy only if content is a non-empty string after trim.
std::string trimStr(const std::string& s) {
  std::size_t b = 0, e = s.size();
  auto isws = [](char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
           ch == '\f' || ch == '\v';
  };
  while (b < e && isws(s[b])) ++b;
  while (e > b && isws(s[e - 1])) --e;
  return s.substr(b, e - b);
}

// JS query/body string-truthiness for lng/lat: present and non-empty.
// (req.query.lng / req.body.lng are strings or numbers; "" / undefined are falsy,
//  and 0 from req.body would be the number 0 — but the JS treats the raw value's
//  truthiness, where the numeric 0 is falsy too. We mirror "missing or empty/zero
//  string" as not-provided for query params, and for body we honor JSON types.)
bool truthyStr(const std::string& s) { return !s.empty(); }

// JSON value truthiness for body fields lng/lat (JS: if (!lng || !lat)).
// A JSON number 0 is falsy; null/absent is falsy; non-empty string is truthy;
// empty string is falsy.
bool truthyJson(const Json::Value& v) {
  if (v.isNull()) return false;
  if (v.isBool()) return v.asBool();
  if (v.isNumeric()) return v.asDouble() != 0.0;
  if (v.isString()) return !v.asString().empty();
  return true;  // objects/arrays are truthy in JS
}

// parseFloat-style coercion from a JSON value (string or number).
double toDouble(const Json::Value& v) {
  if (v.isNumeric()) return v.asDouble();
  if (v.isString()) {
    try { return std::stod(v.asString()); } catch (...) { return 0.0; }
  }
  return 0.0;
}

} // namespace

// ---------------------------------------------------------------------------
// GET /api/v1/whispers/nearby
// ---------------------------------------------------------------------------
void WhisperController::getNearby(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    // const { lng, lat, radius = 5, limit = 50 } = req.query;
    const std::string lngStr = req->getParameter("lng");
    const std::string latStr = req->getParameter("lat");
    const std::string radiusStr = req->getParameter("radius");
    const std::string limitStr = req->getParameter("limit");

    if (lngStr.empty() || latStr.empty()) {
      callback(msgError(k400BadRequest, "Location coordinates required"));
      return;
    }

    double lng = 0.0, lat = 0.0;
    try {
      size_t lngEnd = 0, latEnd = 0;
      lng = std::stod(lngStr, &lngEnd);
      lat = std::stod(latStr, &latEnd);
      if (lngEnd != lngStr.size() || latEnd != latStr.size() ||
          !std::isfinite(lng) || !std::isfinite(lat) ||
          lng < -180.0 || lng > 180.0 || lat < -90.0 || lat > 90.0) {
        throw std::invalid_argument("coordinate range");
      }
    } catch (...) {
      callback(msgError(k400BadRequest, "Invalid location coordinates"));
      return;
    }
    // radius = 5, limit = 50 defaults (parseFloat / parseInt of provided value).
    double parsedRadius = 5.0;
    int parsedLimit = 50;
    try {
      if (truthyStr(radiusStr)) {
        size_t end = 0;
        parsedRadius = std::stod(radiusStr, &end);
        if (end != radiusStr.size() || !std::isfinite(parsedRadius) ||
            parsedRadius <= 0.0)
          throw std::invalid_argument("radius");
      }
      if (truthyStr(limitStr)) {
        size_t end = 0;
        parsedLimit = std::stoi(limitStr, &end);
        if (end != limitStr.size() || parsedLimit <= 0)
          throw std::invalid_argument("limit");
      }
    } catch (...) {
      callback(msgError(k400BadRequest, "Invalid radius or limit"));
      return;
    }
    const double radius = std::clamp(parsedRadius, 0.1, 50.0);
    const int limit = std::clamp(parsedLimit, 1, 100);

    Json::Value whispers = pulse::models::whisper::getNearby(lng, lat, radius, limit);

    // Add distance info:
    //   const dist = calculateDistance(lat, lng,
    //       w.location.coordinates[1], w.location.coordinates[0]);
    //   return { ...wObj, distance: `${dist.toFixed(1)} km` };
    Json::Value withDistance(Json::arrayValue);
    for (const Json::Value& w : whispers) {
      Json::Value wObj = w;  // already projected to anonymous output by getNearby
      double wLat = 0.0, wLng = 0.0;
      if (wObj.isMember("location") && wObj["location"].isObject() &&
          wObj["location"].isMember("coordinates") &&
          wObj["location"]["coordinates"].isArray() &&
          wObj["location"]["coordinates"].size() >= 2) {
        wLng = wObj["location"]["coordinates"][0u].asDouble();  // [0] = lng
        wLat = wObj["location"]["coordinates"][1u].asDouble();  // [1] = lat
      }
      const double dist = calculateDistance(lat, lng, wLat, wLng);
      wObj["distance"] = distanceLabel(dist);
      withDistance.append(wObj);
    }

    // res.json({ success: true, data: withDistance });
    callback(dataOk(withDistance));
  } catch (const std::exception& error) {
    pulse::log::error("Get whispers error: {}", error.what());
    callback(msgError(k500InternalServerError, error.what()));
  }
}

// ---------------------------------------------------------------------------
// POST /api/v1/whispers
// ---------------------------------------------------------------------------
void WhisperController::create(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    auto bodyPtr = req->getJsonObject();
    const Json::Value body = bodyPtr ? *bodyPtr : Json::Value(Json::objectValue);

    // const { content, lng, lat, city, region } = req.body;
    const Json::Value contentV = body.get("content", Json::Value());
    const Json::Value lngV = body.get("lng", Json::Value());
    const Json::Value latV = body.get("lat", Json::Value());

    // const userId = req.user.userId;
    const Json::Value user = req->getAttributes()->get<Json::Value>("user");
    const std::string userId = user.get("userId", "").asString();

    // if (!content?.trim()) -> 400 Content is required
    const std::string content = contentV.isString() ? trimStr(contentV.asString()) : std::string();
    if (content.empty() || content.size() > 2000) {
      callback(msgError(k400BadRequest,
                        "Content must be between 1 and 2000 characters"));
      return;
    }
    if (!lngV.isNumeric() || !latV.isNumeric()) {
      callback(msgError(k400BadRequest, "Location is required"));
      return;
    }

    const double lng = toDouble(lngV);
    const double lat = toDouble(latV);
    if (!std::isfinite(lng) || !std::isfinite(lat) ||
        lng < -180.0 || lng > 180.0 || lat < -90.0 || lat > 90.0) {
      callback(msgError(k400BadRequest, "Invalid location coordinates"));
      return;
    }
    for (const char* field : {"city", "region"}) {
      if (body.isMember(field) &&
          (!body[field].isString() || body[field].asString().size() > 100)) {
        callback(msgError(k400BadRequest, "Invalid location label"));
        return;
      }
    }

    // Whisper.create({ content: content.trim(), author: userId,
    //   location: { type:'Point', coordinates:[lng,lat] }, city, region })
    Json::Value doc(Json::objectValue);
    doc["content"] = content;  // already trimmed
    Json::Value location(Json::objectValue);
    location["type"] = "Point";
    Json::Value coords(Json::arrayValue);
    coords.append(lng);
    coords.append(lat);
    location["coordinates"] = coords;
    doc["location"] = location;
    if (body.isMember("city")) doc["city"] = body["city"];
    if (body.isMember("region")) doc["region"] = body["region"];
    doc = pulse::models::whisper::applyDefaults(std::move(doc));

    // Build the BSON insert document. author must be stored as a real ObjectId.
    bld::document builder{};
    builder.append(kvp("content", doc["content"].asString()));
    builder.append(kvp("location", make_document(
        kvp("type", doc["location"]["type"].asString()),
        kvp("coordinates", make_array(lng, lat)))));
    if (doc.isMember("city") && doc["city"].isString())
      builder.append(kvp("city", doc["city"].asString()));
    if (doc.isMember("region") && doc["region"].isString())
      builder.append(kvp("region", doc["region"].asString()));
    if (auto authorOid = pulse::bsonjson::tryOid(userId)) {
      builder.append(kvp("author", *authorOid));
    } else {
      builder.append(kvp("author", userId));
    }
    builder.append(kvp("upvotes", static_cast<std::int64_t>(doc["upvotes"].asInt64())));
    builder.append(kvp("downvotes", static_cast<std::int64_t>(doc["downvotes"].asInt64())));
    builder.append(kvp("score", static_cast<std::int64_t>(doc["score"].asInt64())));
    builder.append(kvp("voters", make_array()));
    builder.append(kvp("replies", make_array()));
    builder.append(kvp("reports", static_cast<std::int64_t>(doc["reports"].asInt64())));
    builder.append(kvp("isHidden", doc["isHidden"].asBool()));
    const auto now = std::chrono::system_clock::now();
    builder.append(kvp("expiresAt", bsoncxx::types::b_date{
        now + std::chrono::milliseconds(pulse::models::whisper::kExpiryTtlMs)}));
    builder.append(kvp("createdAt", bsoncxx::types::b_date{now}));
    builder.append(kvp("updatedAt", bsoncxx::types::b_date{now}));

    auto insertDoc = builder.extract();
    auto result = pulse::db::collection(pulse::models::whisper::kCollection)
                      .insert_one(insertDoc.view());

    // const safe = whisper.toObject(); delete safe.author;
    Json::Value safe = pulse::bsonjson::toJson(insertDoc.view());
    if (result && result->inserted_id().type() == bsoncxx::type::k_oid) {
      safe["_id"] = pulse::bsonjson::oidToHex(result->inserted_id().get_oid().value);
    }
    safe.removeMember("author");

    // res.status(201).json({ success: true, data: safe });
    callback(dataOk(safe, k201Created));
  } catch (const std::exception& error) {
    pulse::log::error("Create whisper error: {}", error.what());
    callback(msgError(k500InternalServerError, error.what()));
  }
}

// ---------------------------------------------------------------------------
// POST /api/v1/whispers/:whisperId/vote
// ---------------------------------------------------------------------------
void WhisperController::vote(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    std::string whisperId) {
  try {
    auto bodyPtr = req->getJsonObject();
    const Json::Value body = bodyPtr ? *bodyPtr : Json::Value(Json::objectValue);

    // const { voteType } = req.body;  // 'up' or 'down'
    const std::string voteType = body.get("voteType", "").asString();

    // const userId = req.user.userId;
    const Json::Value user = req->getAttributes()->get<Json::Value>("user");
    const std::string userId = user.get("userId", "").asString();

    // if (!['up','down'].includes(voteType)) -> 400 Invalid vote type
    if (voteType != "up" && voteType != "down") {
      callback(msgError(k400BadRequest, "Invalid vote type"));
      return;
    }
    if (!pulse::bsonjson::tryOid(whisperId)) {
      callback(msgError(k400BadRequest, "Invalid whisper ID"));
      return;
    }

    // const result = await Whisper.vote(whisperId, userId, voteType);
    // The JS static throws new Error('Whisper not found') when absent; that
    // propagates to the catch -> 500 { message: error.message }.
    auto result = pulse::models::whisper::vote(whisperId, userId, voteType);
    if (!result) {
      callback(msgError(k404NotFound, "Whisper not found"));
      return;
    }

    Json::Value data(Json::objectValue);
    data["upvotes"] = static_cast<Json::Int64>(result->upvotes);
    data["downvotes"] = static_cast<Json::Int64>(result->downvotes);
    data["score"] = static_cast<Json::Int64>(result->score);

    // res.json({ success: true, data: result });
    callback(dataOk(data));
  } catch (const std::exception& error) {
    pulse::log::error("Vote error: {}", error.what());
    callback(msgError(k500InternalServerError, error.what()));
  }
}

// ---------------------------------------------------------------------------
// POST /api/v1/whispers/:whisperId/reply
// ---------------------------------------------------------------------------
void WhisperController::reply(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    std::string whisperId) {
  try {
    auto bodyPtr = req->getJsonObject();
    const Json::Value body = bodyPtr ? *bodyPtr : Json::Value(Json::objectValue);

    // const { content } = req.body;
    const std::string content = body.get("content", "").asString();

    if (content.empty() || content.size() > 2000) {
      callback(msgError(k400BadRequest,
                        "Reply content must be between 1 and 2000 characters"));
      return;
    }

    // const userId = req.user.userId;
    const Json::Value user = req->getAttributes()->get<Json::Value>("user");
    const std::string userId = user.get("userId", "").asString();

    // const whisper = await Whisper.findById(whisperId);
    // if (!whisper) -> 404 Whisper not found
    auto whisperOid = pulse::bsonjson::tryOid(whisperId);
    if (!whisperOid) {
      callback(msgError(k400BadRequest, "Invalid whisper ID"));
      return;
    }
    auto existing = pulse::db::collection(pulse::models::whisper::kCollection)
                        .find_one(make_document(kvp("_id", *whisperOid)));
    if (!existing) {
      callback(msgError(k404NotFound, "Whisper not found"));
      return;
    }

    // const reply = await whisper.addReply(content, userId);
    auto replyDoc = pulse::models::whisper::addReply(whisperId, content, userId);
    if (!replyDoc) {
      // Concurrent delete between findById and save — mirror not-found 404.
      callback(msgError(k404NotFound, "Whisper not found"));
      return;
    }

    // const safe = { ...reply.toObject() }; delete safe.author;
    Json::Value safe = *replyDoc;
    safe.removeMember("author");

    // res.status(201).json({ success: true, data: safe });
    callback(dataOk(safe, k201Created));
  } catch (const std::exception& error) {
    pulse::log::error("Reply error: {}", error.what());
    callback(msgError(k500InternalServerError, error.what()));
  }
}

// ---------------------------------------------------------------------------
// POST /api/v1/whispers/:whisperId/report
// ---------------------------------------------------------------------------
void WhisperController::report(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    std::string whisperId) {
  try {
    // const whisper = await Whisper.findById(whisperId);
    // if (!whisper) -> 404 Whisper not found
    auto whisperOid = pulse::bsonjson::tryOid(whisperId);
    if (!whisperOid) {
      callback(msgError(k400BadRequest, "Invalid whisper ID"));
      return;
    }
    auto col = pulse::db::collection(pulse::models::whisper::kCollection);
    auto existing = col.find_one(make_document(
        kvp("_id", *whisperOid),
        kvp("expiresAt", make_document(kvp(
            "$gt", bsoncxx::types::b_date{std::chrono::system_clock::now()}))),
        kvp("isHidden", make_document(kvp("$ne", true)))));
    if (!existing) {
      callback(msgError(k404NotFound, "Whisper not found"));
      return;
    }

    const Json::Value auth = req->getAttributes()->get<Json::Value>("user");
    auto reporterOid = pulse::bsonjson::tryOid(auth.get("userId", "").asString());
    if (!reporterOid) {
      callback(msgError(k401Unauthorized, "Authentication required"));
      return;
    }

    // Record each reporter once and increment atomically. The predicate makes
    // duplicate/concurrent reports by the same account idempotent.
    mongocxx::options::find_one_and_update opts{};
    opts.return_document(mongocxx::options::return_document::k_after);
    auto updated = col.find_one_and_update(
        make_document(kvp("_id", *whisperOid),
                      kvp("expiresAt", make_document(kvp(
                          "$gt", bsoncxx::types::b_date{
                                     std::chrono::system_clock::now()}))),
                      kvp("isHidden", make_document(kvp("$ne", true))),
                      kvp("reportedBy", make_document(kvp("$ne", *reporterOid)))),
        make_document(
            kvp("$addToSet", make_document(kvp("reportedBy", *reporterOid))),
            kvp("$inc", make_document(kvp("reports", 1))),
            kvp("$set", make_document(kvp(
                "updatedAt", bsoncxx::types::b_date{std::chrono::system_clock::now()})))),
        opts);
    if (!updated) {
      callback(msgOk("Already reported"));
      return;
    }

    Json::Value whisper = pulse::bsonjson::toJson(updated->view());
    if (whisper.get("reports", 0).asInt64() >= 5) {
      col.update_one(make_document(kvp("_id", *whisperOid)),
                     make_document(kvp("$set", make_document(kvp("isHidden", true)))));
    }

    // res.json({ success: true, message: 'Reported' });
    callback(msgOk("Reported"));
  } catch (const std::exception& error) {
    pulse::log::error("Report error: {}", error.what());
    callback(msgError(k500InternalServerError, error.what()));
  }
}

} // namespace pulse::controllers
