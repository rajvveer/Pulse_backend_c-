// social_dna_controller.cc — port of src/controllers/socialDNAController.js
// (route group src/routes/socialDNARoutes.js, mounted at /api/v1/social-dna).
//
// Ground truth: socialDNAController.js. Every handler mirrors its Express
// counterpart: the same authed-user/param/query reads, the same
// res.json({ success, data, ... }) success shapes, and the same try/catch that
// translates any thrown error into the LEGACY error shape:
//   res.status(500).json({ success: false, error: '<message>' })  — NO `code`.
// These bodies are built directly (NOT pulse::http::error, which adds a `code`).
//
// Model/algorithm logic is NOT reimplemented: getOrCreate / getShareCardData
// come from pulse::models::socialdna; the DNA-match scoring comes from the
// kernel pulse::algos::dnaMatch (the in-process equivalent of the Node native
// addon `addon.dnaMatch`). The DB-bound orchestration that the JS DNAMatchAlgo
// wrapper performed around that kernel (getCompatibility / findTwins) lives here
// in file-local helpers, exactly as the JS wrapper did it, because there is no
// separate DNA-match service port to call.
#include "pulse/controllers/social_dna_controller.hpp"

#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/http_response.hpp"
#include "pulse/logger.hpp"
#include "pulse/algorithms.hpp"
#include "pulse/models/socialdna.hpp"
#include "pulse/models/user.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/oid.hpp>

#include <mongocxx/collection.hpp>
#include <mongocxx/cursor.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/find_one_and_update.hpp>
#include <mongocxx/exception/exception.hpp>

#include <json/json.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace pulse::controllers {

namespace {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;

// ---------------------------------------------------------------------------
// Legacy error response: res.status(code).json({ success: false, error: msg }).
// socialDNAController.js never emits a `code` on error, so we build the exact
// { success:false, error } body the JS produces (NOT pulse::http::error).
// ---------------------------------------------------------------------------
pulse::http::HttpResponsePtr legacyError(drogon::HttpStatusCode code,
                                         const std::string& message) {
  Json::Value body(Json::objectValue);
  body["success"] = false;
  body["error"] = message;
  return pulse::http::json(code, std::move(body));
}

// Numeric accessor tolerant of int/double/missing (BSON round-trip leaves
// numbers as either int or double).
double numOr(const Json::Value& obj, const char* key, double fallback) {
  if (!obj.isObject() || !obj.isMember(key)) return fallback;
  const Json::Value& v = obj[key];
  return v.isNumeric() ? v.asDouble() : fallback;
}

// interactionCountOf(dna) = dna.interactionCount != null ? .. : dna.totalSignals.
// SocialDNA has no interactionCount field, so this resolves to totalSignals.
double interactionCountOf(const Json::Value& dna) {
  if (dna.isObject() && dna.isMember("interactionCount") &&
      dna["interactionCount"].isNumeric()) {
    return dna["interactionCount"].asDouble();
  }
  return numOr(dna, "totalSignals", 0);
}

// Build the per-user scoring slice the addon payload expects:
//   { strands, totalSignals, dominantVibe, interactionCount }
Json::Value scoringSlice(const Json::Value& dna) {
  Json::Value s(Json::objectValue);
  s["strands"] = dna.isMember("strands") ? dna["strands"] : Json::Value(Json::objectValue);
  s["totalSignals"] = static_cast<int>(numOr(dna, "totalSignals", 0));
  s["dominantVibe"] = dna.isMember("dominantVibe") ? dna["dominantVibe"] : Json::Value("");
  s["interactionCount"] = static_cast<int>(interactionCountOf(dna));
  return s;
}

// Hard caps from DNAMatchAlgo.js so the request path can never trigger a
// full-collection scan.
constexpr int kMaxCandidatesScanned = 1000;
constexpr int kMaxTwinsReturned = 50;
// JS.CONFIG values used by the wrapper.
constexpr int kMaxTwins = 20;             // CONFIG.MAX_TWINS (default limit)
constexpr int kMinSignalsForMatch = 10;   // CONFIG.MIN_SIGNALS_FOR_MATCH

// Projection for .populate('twins.user' / 'user',
//   'username profile.displayName profile.avatar isVerified'). _id is always
// included by Mongoose.
bsoncxx::document::value twinUserProjection() {
  bld::document proj;
  proj.append(kvp("username", 1));
  proj.append(kvp("profile.displayName", 1));
  proj.append(kvp("profile.avatar", 1));
  proj.append(kvp("isVerified", 1));
  return proj.extract();
}

// Fetch a single populated User doc (sanitized like Mongoose res.json). Returns
// the raw hex id when the ref cannot be resolved (mirrors an unpopulatable ref
// staying as its id), and null Json on an invalid id.
Json::Value populateUser(const std::string& hexId) {
  auto oid = pulse::bsonjson::tryOid(hexId);
  if (!oid) return Json::Value(hexId);
  mongocxx::options::find opts;
  opts.projection(twinUserProjection().view());
  auto col = pulse::db::collection(pulse::models::user::kCollection);
  auto res = col.find_one(make_document(kvp("_id", *oid)), opts);
  if (!res) return Json::Value(Json::nullValue);  // ref points to a missing user
  return pulse::models::user::sanitizeForOutput(
      pulse::bsonjson::toJson(res->view()));
}

// ---------------------------------------------------------------------------
// DNAMatchAlgo.getCompatibility(userIdA, userIdB) — fetch both DNAs, score in
// the native kernel. (The JS native path is always taken here; the in-process
// kernel never "fails" the way the optional Node addon could, so there is no
// fallback branch to mirror.)
// ---------------------------------------------------------------------------
Json::Value getCompatibilityResult(const std::string& userIdA,
                                    const std::string& userIdB) {
  Json::Value dnaA = pulse::models::socialdna::getOrCreate(userIdA);
  Json::Value dnaB = pulse::models::socialdna::getOrCreate(userIdB);

  Json::Value payload(Json::objectValue);
  payload["mode"] = "compatibility";
  payload["a"] = scoringSlice(dnaA);
  payload["b"] = scoringSlice(dnaB);

  Json::CharReaderBuilder rb;
  Json::Value out;
  std::string errs;
  const std::string resultStr = pulse::algos::dnaMatch(payload.toStyledString());
  std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
  reader->parse(resultStr.data(), resultStr.data() + resultStr.size(), &out, &errs);
  return out;
}

// ---------------------------------------------------------------------------
// DNAMatchAlgo.findTwins(userId, limit) — serve precomputed twins, else scan a
// BOUNDED candidate set and score it in the native kernel. Ported 1:1 from the
// loaded DNAMatchAlgo.js wrapper (the full-scan fix).
// ---------------------------------------------------------------------------
Json::Value findTwinsResult(const std::string& userId, int rawLimit) {
  // safeLimit = min(max(parseInt(limit) || MAX_TWINS, 1), MAX_TWINS_RETURNED)
  int base = (rawLimit != 0) ? rawLimit : kMaxTwins;
  int safeLimit = std::min(std::max(base, 1), kMaxTwinsReturned);

  Json::Value userDNA = pulse::models::socialdna::getOrCreate(userId);
  if (numOr(userDNA, "totalSignals", 0) < kMinSignalsForMatch) {
    return Json::Value(Json::arrayValue);
  }

  // 1) Serve PRECOMPUTED twins if the weekly job already populated them.
  if (userDNA.isMember("twins") && userDNA["twins"].isArray() &&
      !userDNA["twins"].empty()) {
    Json::Value list(Json::arrayValue);
    for (const auto& t : userDNA["twins"]) {
      if (static_cast<int>(list.size()) >= safeLimit) break;
      if (!t.isObject() || !t.isMember("user")) continue;
      // populate twins.user; filter(t => t.user) drops unresolvable refs.
      Json::Value populated = t["user"].isString()
                                  ? populateUser(t["user"].asString())
                                  : t["user"];
      if (populated.isNull()) continue;
      Json::Value entry(Json::objectValue);
      entry["user"] = populated;
      entry["matchPercent"] = t.isMember("matchPercent") ? t["matchPercent"]
                                                         : Json::Value(0);
      list.append(entry);
    }
    if (!list.empty()) return list;
  }

  // 2) On-demand: scan a BOUNDED candidate set (never the whole collection).
  //    Narrow by dominant vibe first (indexed), then top up with any mature DNA
  //    up to the cap.
  auto col = pulse::db::collection(pulse::models::socialdna::kCollection);
  const bsoncxx::oid userOid = pulse::bsonjson::oid(userId);
  const std::string userDominant =
      userDNA.isMember("dominantVibe") ? userDNA["dominantVibe"].asString() : "";

  std::vector<Json::Value> candidates;
  std::set<std::string> seen;

  // SocialDNA.find({ user: { $ne }, dominantVibe, totalSignals: { $gte } })
  //   .limit(MAX_CANDIDATES_SCANNED).populate('user', <projection>)
  {
    mongocxx::options::find opts;
    opts.limit(kMaxCandidatesScanned);
    auto cursor = col.find(
        make_document(
            kvp("user", make_document(kvp("$ne", userOid))),
            kvp("dominantVibe", userDominant),
            kvp("totalSignals", make_document(kvp("$gte", kMinSignalsForMatch)))),
        opts);
    for (auto&& doc : cursor) {
      Json::Value c = pulse::bsonjson::toJson(doc);
      if (c.isMember("_id") && c["_id"].isString()) seen.insert(c["_id"].asString());
      if (c.isMember("user") && c["user"].isString())
        c["user"] = populateUser(c["user"].asString());
      candidates.push_back(c);
    }
  }

  // if (candidates.length < safeLimit) top up with any mature DNA, dedup by _id.
  if (static_cast<int>(candidates.size()) < safeLimit) {
    int remaining = kMaxCandidatesScanned - static_cast<int>(candidates.size());
    if (remaining > 0) {
      mongocxx::options::find opts;
      opts.limit(remaining);
      auto cursor = col.find(
          make_document(
              kvp("user", make_document(kvp("$ne", userOid))),
              kvp("totalSignals",
                  make_document(kvp("$gte", kMinSignalsForMatch)))),
          opts);
      for (auto&& doc : cursor) {
        Json::Value e = pulse::bsonjson::toJson(doc);
        std::string id = (e.isMember("_id") && e["_id"].isString())
                             ? e["_id"].asString()
                             : "";
        if (!id.empty() && seen.count(id)) continue;
        if (e.isMember("user") && e["user"].isString())
          e["user"] = populateUser(e["user"].asString());
        candidates.push_back(e);
      }
    }
  }

  // Score the bounded candidate set in the native kernel (batch mode).
  Json::Value payload(Json::objectValue);
  payload["mode"] = "batch";
  payload["user"] = scoringSlice(userDNA);
  Json::Value candArr(Json::arrayValue);
  for (const auto& c : candidates) {
    Json::Value cand(Json::objectValue);
    cand["user"] = c.isMember("user") ? c["user"] : Json::Value(Json::nullValue);
    cand["strands"] = c.isMember("strands") ? c["strands"]
                                            : Json::Value(Json::objectValue);
    cand["totalSignals"] = static_cast<int>(numOr(c, "totalSignals", 0));
    cand["dominantVibe"] =
        c.isMember("dominantVibe") ? c["dominantVibe"] : Json::Value("");
    cand["interactionCount"] = static_cast<int>(interactionCountOf(c));
    candArr.append(cand);
  }
  payload["candidates"] = candArr;
  payload["limit"] = safeLimit;

  Json::CharReaderBuilder rb;
  Json::Value out;
  std::string errs;
  const std::string resultStr = pulse::algos::dnaMatch(payload.toStyledString());
  std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
  reader->parse(resultStr.data(), resultStr.data() + resultStr.size(), &out, &errs);
  return out;
}

}  // namespace

// ===========================================================================
//  GET MY DNA
// ===========================================================================
void SocialDNAController::getMyDNA(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    const Json::Value user = req->getAttributes()->get<Json::Value>("user");
    Json::Value dna =
        pulse::models::socialdna::getOrCreate(user["userId"].asString());

    Json::Value data(Json::objectValue);
    data["strands"] = dna.isMember("strands") ? dna["strands"]
                                              : Json::Value(Json::objectValue);
    data["dominantVibe"] =
        dna.isMember("dominantVibe") ? dna["dominantVibe"] : Json::Value();
    data["totalSignals"] =
        dna.isMember("totalSignals") ? dna["totalSignals"] : Json::Value();
    data["streak"] = dna.isMember("streak") ? dna["streak"] : Json::Value();
    data["weeksTracked"] = dna.isMember("totalWeeksTracked")
                               ? dna["totalWeeksTracked"]
                               : Json::Value();
    data["latestInsights"] = dna.isMember("latestInsights")
                                 ? dna["latestInsights"]
                                 : Json::Value();
    data["lastComputedAt"] =
        dna.isMember("lastComputedAt") ? dna["lastComputedAt"] : Json::Value();

    callback(pulse::http::ok(std::move(data)));
  } catch (const std::exception& error) {
    pulse::log::error("[SocialDNA] getMyDNA error: {}", error.what());
    callback(legacyError(drogon::k500InternalServerError,
                         "Failed to get DNA profile"));
  }
}

// ===========================================================================
//  GET DNA SHARE CARD DATA
// ===========================================================================
void SocialDNAController::getShareCard(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    const Json::Value user = req->getAttributes()->get<Json::Value>("user");
    Json::Value dna =
        pulse::models::socialdna::getOrCreate(user["userId"].asString());
    Json::Value cardData = pulse::models::socialdna::getShareCardData(dna);

    callback(pulse::http::ok(std::move(cardData)));
  } catch (const std::exception& error) {
    pulse::log::error("[SocialDNA] getShareCard error: {}", error.what());
    callback(legacyError(drogon::k500InternalServerError,
                         "Failed to generate share card"));
  }
}

// ===========================================================================
//  RECORD CARD SHARE (viral tracking)
// ===========================================================================
void SocialDNAController::recordShare(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    const Json::Value user = req->getAttributes()->get<Json::Value>("user");

    // SocialDNA.findOneAndUpdate({ user }, { $inc: { cardShareCount: 1 } })
    auto col = pulse::db::collection(pulse::models::socialdna::kCollection);
    const bsoncxx::oid userOid =
        pulse::bsonjson::oid(user["userId"].asString());
    col.find_one_and_update(
        make_document(kvp("user", userOid)),
        make_document(kvp("$inc", make_document(kvp("cardShareCount", 1)))));

    Json::Value body(Json::objectValue);
    body["success"] = true;
    body["message"] = "Share recorded";
    callback(pulse::http::json(drogon::k200OK, std::move(body)));
  } catch (const std::exception& error) {
    pulse::log::error("[SocialDNA] recordShare error: {}", error.what());
    callback(legacyError(drogon::k500InternalServerError,
                         "Failed to record share"));
  }
}

// ===========================================================================
//  GET DNA EVOLUTION (weekly snapshots)
// ===========================================================================
void SocialDNAController::getEvolution(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    const Json::Value user = req->getAttributes()->get<Json::Value>("user");
    Json::Value dna =
        pulse::models::socialdna::getOrCreate(user["userId"].asString());

    // const limit = parseInt(req.query.weeks) || 12;
    int limit = 12;
    {
      const std::string weeks = req->getParameter("weeks");
      if (!weeks.empty()) {
        int parsed = std::atoi(weeks.c_str());
        if (parsed != 0) limit = parsed;
      }
    }

    // dna.snapshots.slice(-limit).map(...)
    const Json::Value& snaps =
        (dna.isMember("snapshots") && dna["snapshots"].isArray())
            ? dna["snapshots"]
            : Json::Value(Json::arrayValue);
    long long total = static_cast<long long>(snaps.size());
    // dna.snapshots.slice(-limit): the JS slice start is `-limit`. For a negative
    // start, the effective index is max(0, length + start); for a non-negative
    // start (i.e. limit <= 0), it is min(start, length). Replicate exactly so
    // e.g. ?weeks=-5 (limit=-5 -> slice(5)) behaves like the JS.
    long long sliceStart = -static_cast<long long>(limit);
    long long start = (sliceStart < 0) ? std::max<long long>(0, total + sliceStart)
                                       : std::min<long long>(sliceStart, total);

    Json::Value snapshots(Json::arrayValue);
    for (long long i = start; i < total; ++i) {
      const Json::Value& s = snaps[static_cast<Json::ArrayIndex>(i)];
      Json::Value m(Json::objectValue);
      m["weekStart"] = s.isMember("weekStart") ? s["weekStart"] : Json::Value();
      m["weekEnd"] = s.isMember("weekEnd") ? s["weekEnd"] : Json::Value();
      m["strands"] = s.isMember("strands") ? s["strands"]
                                           : Json::Value(Json::objectValue);
      m["dominantVibe"] =
          s.isMember("dominantVibe") ? s["dominantVibe"] : Json::Value();
      m["insights"] =
          s.isMember("insights") ? s["insights"] : Json::Value(Json::arrayValue);
      m["totalInteractions"] =
          s.isMember("totalInteractions") ? s["totalInteractions"] : Json::Value();
      snapshots.append(m);
    }

    Json::Value data(Json::objectValue);
    data["snapshots"] = snapshots;
    data["currentStrands"] = dna.isMember("strands")
                                 ? dna["strands"]
                                 : Json::Value(Json::objectValue);
    data["totalWeeks"] = dna.isMember("totalWeeksTracked")
                             ? dna["totalWeeksTracked"]
                             : Json::Value();

    callback(pulse::http::ok(std::move(data)));
  } catch (const std::exception& error) {
    pulse::log::error("[SocialDNA] getEvolution error: {}", error.what());
    callback(legacyError(drogon::k500InternalServerError,
                         "Failed to get evolution data"));
  }
}

// ===========================================================================
//  GET COMPATIBILITY WITH ANOTHER USER
// ===========================================================================
void SocialDNAController::getCompatibility(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    std::string targetUserId) {
  try {
    // if (!targetUserId) return res.status(400).json({ success:false, error })
    if (targetUserId.empty()) {
      callback(legacyError(drogon::k400BadRequest, "Target user ID required"));
      return;
    }

    const Json::Value user = req->getAttributes()->get<Json::Value>("user");
    Json::Value result =
        getCompatibilityResult(user["userId"].asString(), targetUserId);

    callback(pulse::http::ok(std::move(result)));
  } catch (const std::exception& error) {
    pulse::log::error("[SocialDNA] getCompatibility error: {}", error.what());
    callback(legacyError(drogon::k500InternalServerError,
                         "Failed to calculate compatibility"));
  }
}

// ===========================================================================
//  FIND MY DNA TWINS
// ===========================================================================
void SocialDNAController::findTwins(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    // const limit = parseInt(req.query.limit) || 20;
    int limit = 20;
    {
      const std::string limitParam = req->getParameter("limit");
      if (!limitParam.empty()) {
        int parsed = std::atoi(limitParam.c_str());
        if (parsed != 0) limit = parsed;
      }
    }

    const Json::Value user = req->getAttributes()->get<Json::Value>("user");
    Json::Value twins = findTwinsResult(user["userId"].asString(), limit);

    callback(pulse::http::ok(std::move(twins)));
  } catch (const std::exception& error) {
    pulse::log::error("[SocialDNA] findTwins error: {}", error.what());
    callback(legacyError(drogon::k500InternalServerError,
                         "Failed to find DNA twins"));
  }
}

// ===========================================================================
//  GET ANOTHER USER'S DNA (public profile view)
// ===========================================================================
void SocialDNAController::getUserDNA(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    std::string userId) {
  try {
    // const dna = await SocialDNA.findOne({ user: userId });
    // Mongoose casts the raw `userId` param to an ObjectId; an invalid id throws
    // a CastError that the try/catch below turns into the 500. pulse::bsonjson::oid
    // throws on an invalid hex, preserving that behavior.
    auto col = pulse::db::collection(pulse::models::socialdna::kCollection);
    const bsoncxx::oid userOid = pulse::bsonjson::oid(userId);
    auto found = col.find_one(make_document(kvp("user", userOid)));

    if (!found) {
      // res.json({ success:true, data:null, message:'User has no DNA profile yet' })
      Json::Value body(Json::objectValue);
      body["success"] = true;
      body["data"] = Json::Value(Json::nullValue);
      body["message"] = "User has no DNA profile yet";
      callback(pulse::http::json(drogon::k200OK, std::move(body)));
      return;
    }

    Json::Value dna = pulse::bsonjson::toJson(found->view());

    // Return public DNA data (no raw signals).
    Json::Value data(Json::objectValue);
    data["strands"] = dna.isMember("strands") ? dna["strands"]
                                              : Json::Value(Json::objectValue);
    data["dominantVibe"] =
        dna.isMember("dominantVibe") ? dna["dominantVibe"] : Json::Value();
    data["weeksTracked"] = dna.isMember("totalWeeksTracked")
                               ? dna["totalWeeksTracked"]
                               : Json::Value();
    data["streak"] = dna.isMember("streak") ? dna["streak"] : Json::Value();

    callback(pulse::http::ok(std::move(data)));
  } catch (const std::exception& error) {
    pulse::log::error("[SocialDNA] getUserDNA error: {}", error.what());
    callback(legacyError(drogon::k500InternalServerError,
                         "Failed to get user DNA"));
  }
}

}  // namespace pulse::controllers
