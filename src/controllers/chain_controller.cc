// chain_controller.cc — implementation of pulse::controllers::ChainController.
//
// 1:1 port of src/controllers/chainController.js. The embedded-subdocument
// mutations (submitSegment / voteOnSegment / toggleLike) live in
// pulse::models::chainstory and are CALLED, not reimplemented. The direct
// Mongoose queries the controller issued (ChainStory.find / .findById /
// .create) are reproduced here with mongocxx, and the
// .populate('…', 'username profile.avatar') joins are performed in the
// controller layer (matching the post/feed convention). Every reply mirrors the
// Express res.json shape and status code exactly: success -> { success:true,
// data }, validation/not-found -> { success:false, message }, and the try/catch
// fallback -> res.status(500).json({ success:false, message: error.message }).
#include "pulse/controllers/chain_controller.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string>
#include <vector>

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/oid.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/options/find.hpp>

#include "pulse/bson_json.hpp"
#include "pulse/db.hpp"
#include "pulse/http_response.hpp"
#include "pulse/logger.hpp"
#include "pulse/models/chainstory.hpp"

using namespace pulse::controllers;
namespace chainstory = pulse::models::chainstory;

namespace {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;

// ── req.user.userId (set by AuthFilter on the request attributes) ────────────
std::string authedUserId(const drogon::HttpRequestPtr& req) {
  const auto& user = req->getAttributes()->get<Json::Value>("user");
  return user["userId"].asString();
}

// ── JS String.prototype.trim() ────────────────────────────────────────────────
std::string jsTrim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

// JS truthiness of `value?.trim()`: a non-null/undefined string is truthy iff,
// after trimming, it is non-empty. Used for `!title?.trim()` checks.
bool isNonBlankString(const Json::Value& v) {
  return v.isString() && !jsTrim(v.asString()).empty();
}

// Coerce a Json scalar to an int the way Mongoose casts a Number-typed field on
// save: numeric stays numeric; a numeric string is parsed; anything else 0.
int toIntCast(const Json::Value& v, int dflt) {
  if (v.isInt() || v.isInt64() || v.isUInt() || v.isUInt64())
    return static_cast<int>(v.asInt64());
  if (v.isDouble()) return static_cast<int>(v.asDouble());
  if (v.isBool()) return v.asBool() ? 1 : 0;
  if (v.isString()) {
    try {
      return static_cast<int>(std::stol(v.asString()));
    } catch (...) {
      return dflt;
    }
  }
  return dflt;
}

// JS parseInt(str, 10): optional leading sign + digits, NaN (nullopt) on none.
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

// JS Number(str) for the page coercion in (page - 1) * limit. An all-numeric
// (optionally signed/decimal) string converts; anything else is NaN. We only
// need integer-ish pagination values, so fall back to 0 on NaN (the multiply
// then yields a NaN-safe skip of 0, which mongocxx clamps).
double jsNumber(const std::string& s) {
  if (s.empty()) return 0.0;  // Number('') === 0
  errno = 0;
  char* end = nullptr;
  double v = std::strtod(s.c_str(), &end);
  // Number requires the WHOLE string to be a valid number (ignoring surrounding
  // whitespace); trailing garbage => NaN. Approximate with full-consume check.
  while (end && *end != '\0' && std::isspace(static_cast<unsigned char>(*end))) ++end;
  if (end == s.c_str() || (end && *end != '\0')) return 0.0;
  return v;
}

// The Express try/catch fallback for every handler:
//   res.status(500).json({ success:false, message: error.message })
pulse::http::HttpResponsePtr serverErrorWithMessage(const std::string& message) {
  Json::Value body(Json::objectValue);
  body["success"] = false;
  body["message"] = message;
  return pulse::http::json(drogon::k500InternalServerError, std::move(body));
}

// A { success:false, message } reply with an explicit status (validation/404).
pulse::http::HttpResponsePtr failWith(drogon::HttpStatusCode code,
                                      const std::string& message) {
  Json::Value body(Json::objectValue);
  body["success"] = false;
  body["message"] = message;
  return pulse::http::json(code, std::move(body));
}

// A { success:true, data } reply with an explicit status.
pulse::http::HttpResponsePtr okData(Json::Value data,
                                    drogon::HttpStatusCode code = drogon::k200OK) {
  return pulse::http::ok(std::move(data), code);
}

// ── Controller-layer .populate('<ref>', 'username profile.avatar') ────────────
// Resolve a set of user-ObjectId hex strings to the projected subdocument
// { _id, username, profile:{ avatar } } that Mongoose's populate select
// 'username profile.avatar' returns (Mongoose always includes _id). Returns a
// hex -> user-json map; unresolved ids are simply absent (populate leaves a
// dangling ref unchanged).
Json::Value fetchUsersById(const std::vector<std::string>& ids) {
  Json::Value byId(Json::objectValue);
  if (ids.empty()) return byId;
  try {
    bld::array in;
    for (const auto& hex : ids)
      if (auto o = pulse::bsonjson::tryOid(hex)) in.append(*o);
    auto filter = make_document(kvp("_id", make_document(kvp("$in", in.extract()))));
    // select: 'username profile.avatar' (Mongoose auto-includes _id).
    auto projection =
        make_document(kvp("username", 1), kvp("profile.avatar", 1));
    mongocxx::options::find opts{};
    opts.projection(projection.view());

    auto col = pulse::db::collection("users");
    auto cursor = col.find(filter.view(), opts);
    for (const auto& doc : cursor) {
      Json::Value u = pulse::bsonjson::toJson(doc);
      if (u.isMember("_id") && u["_id"].isString())
        byId[u["_id"].asString()] = u;
    }
  } catch (const std::exception& e) {
    pulse::log::error("[chain] user populate failed: {}", e.what());
  }
  return byId;
}

// Extract the hex id from a Json ref that is either a hex string or an already
// hydrated { _id } subdocument.
std::string refHex(const Json::Value& ref) {
  if (ref.isString()) return ref.asString();
  if (ref.isObject() && ref.isMember("_id") && ref["_id"].isString())
    return ref["_id"].asString();
  return std::string();
}

// Replace a single ref field with its hydrated user doc where available.
void populateField(Json::Value& parent, const char* field, const Json::Value& byId) {
  if (!parent.isMember(field)) return;
  const std::string hex = refHex(parent[field]);
  if (!hex.empty() && byId.isMember(hex)) parent[field] = byId[hex];
}

// Collect a ref's hex id into `ids` (deduped) for a batched populate query.
void collectRef(const Json::Value& ref, std::vector<std::string>& ids) {
  const std::string hex = refHex(ref);
  if (!hex.empty() && std::find(ids.begin(), ids.end(), hex) == ids.end())
    ids.push_back(hex);
}

}  // namespace

// ── GET /api/v1/chains — getChains ────────────────────────────────────────────
// const { genre, page = 1, limit = 20, status = 'active' } = req.query;
// const query = {};
// if (status !== 'all') query.status = status;
// if (genre) query.genre = genre;
// ChainStory.find(query).sort({ likes:-1, contributorCount:-1 })
//   .skip((page-1)*limit).limit(parseInt(limit))
//   .populate('starterAuthor', 'username profile.avatar')
//   .select('-pendingSegments -segments.voters');
// withLast = chains.map(c => ({ ...c.toObject(), lastSegment: <virtual> }));
// res.json({ success:true, data: withLast });
void ChainController::getChains(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    // req.query with destructuring defaults (applied only when undefined). The
    // params map lets us distinguish "absent" (-> default) from "?x=" (empty
    // string), which matters for the `status` default and the page/limit ones.
    const auto& params = req->getParameters();
    const std::string genre = req->getParameter("genre");
    const std::string status =
        params.count("status") > 0 ? req->getParameter("status") : "active";
    const std::string pageRaw =
        params.count("page") > 0 ? req->getParameter("page") : "1";
    const std::string limitRaw =
        params.count("limit") > 0 ? req->getParameter("limit") : "20";

    // const query = {}; if (status !== 'all') query.status = status;
    // if (genre) query.genre = genre;   (JS truthiness: empty string is falsy)
    bld::document filter{};
    if (status != "all") filter.append(kvp("status", status));
    if (!genre.empty()) filter.append(kvp("genre", genre));

    // .skip((page - 1) * limit) — JS numeric coercion of the raw query strings.
    const double pageNum = jsNumber(pageRaw);
    const double limitNum = jsNumber(limitRaw);
    long long skip = static_cast<long long>((pageNum - 1.0) * limitNum);
    if (skip < 0) skip = 0;
    // .limit(parseInt(limit))
    long limit = jsParseInt(limitRaw).value_or(0);

    mongocxx::options::find opts{};
    opts.sort(make_document(kvp("likes", -1), kvp("contributorCount", -1)));
    opts.skip(static_cast<std::int64_t>(skip));
    opts.limit(static_cast<std::int64_t>(limit));
    // .select('-pendingSegments -segments.voters')
    opts.projection(
        make_document(kvp("pendingSegments", 0), kvp("segments.voters", 0)));

    Json::Value chains(Json::arrayValue);
    {
      auto col = pulse::db::collection(chainstory::kCollection);
      auto cursor = col.find(filter.extract(), opts);
      for (const auto& view : cursor)
        chains.append(pulse::bsonjson::toJson(view));
    }

    // .populate('starterAuthor', 'username profile.avatar') — batched join.
    std::vector<std::string> ids;
    for (const auto& c : chains)
      if (c.isMember("starterAuthor")) collectRef(c["starterAuthor"], ids);
    const Json::Value byId = fetchUsersById(ids);

    // withLast: ...c.toObject() + lastSegment virtual.
    Json::Value withLast(Json::arrayValue);
    for (auto& c : chains) {
      populateField(c, "starterAuthor", byId);
      // lastSegment: segments.length ? last.content : starterContent.
      c["lastSegment"] = chainstory::lastSegment(c);
      withLast.append(c);
    }

    callback(okData(std::move(withLast)));
  } catch (const std::exception& e) {
    pulse::log::error("Get chains error: {}", e.what());
    callback(serverErrorWithMessage(e.what()));
  }
}

// ── GET /api/v1/chains/:chainId — getById ─────────────────────────────────────
// const chain = await ChainStory.findById(chainId)
//   .populate('starterAuthor', 'username profile.avatar')
//   .populate('segments.author', 'username profile.avatar')
//   .populate('contributors', 'username profile.avatar');
// if (!chain) return res.status(404).json({ success:false, message:'Chain not found' });
// res.json({ success:true, data: chain });
void ChainController::getById(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string chainId) {
  (void)req;
  try {
    // findById: an invalid ObjectId makes Mongoose throw a CastError, which the
    // try/catch turns into res.status(500).json({ message: error.message }).
    bsoncxx::oid oid;
    try {
      oid = pulse::bsonjson::oid(chainId);
    } catch (const std::exception&) {
      callback(serverErrorWithMessage(
          "Cast to ObjectId failed for value \"" + chainId +
          "\" (type string) at path \"_id\" for model \"ChainStory\""));
      return;
    }

    Json::Value chain;
    {
      auto col = pulse::db::collection(chainstory::kCollection);
      auto found = col.find_one(make_document(kvp("_id", oid)));
      if (!found) {
        callback(failWith(drogon::k404NotFound, "Chain not found"));
        return;
      }
      chain = pulse::bsonjson::toJson(found->view());
    }

    // Batched populate over starterAuthor + segments.author + contributors.
    std::vector<std::string> ids;
    if (chain.isMember("starterAuthor")) collectRef(chain["starterAuthor"], ids);
    if (chain.isMember("segments") && chain["segments"].isArray())
      for (const auto& seg : chain["segments"])
        if (seg.isMember("author")) collectRef(seg["author"], ids);
    if (chain.isMember("contributors") && chain["contributors"].isArray())
      for (const auto& c : chain["contributors"]) collectRef(c, ids);
    const Json::Value byId = fetchUsersById(ids);

    populateField(chain, "starterAuthor", byId);
    if (chain.isMember("segments") && chain["segments"].isArray())
      for (auto& seg : chain["segments"]) populateField(seg, "author", byId);
    if (chain.isMember("contributors") && chain["contributors"].isArray()) {
      Json::Value& contributors = chain["contributors"];
      for (Json::ArrayIndex i = 0; i < contributors.size(); ++i) {
        const std::string hex = refHex(contributors[i]);
        if (!hex.empty() && byId.isMember(hex)) contributors[i] = byId[hex];
      }
    }

    callback(okData(std::move(chain)));
  } catch (const std::exception& e) {
    pulse::log::error("Get chain error: {}", e.what());
    callback(serverErrorWithMessage(e.what()));
  }
}

// ── POST /api/v1/chains — create ──────────────────────────────────────────────
// const { title, starterContent, genre, previewImage, maxSegments, requireVotes } = req.body;
// const userId = req.user.userId;
// if (!title?.trim() || !starterContent?.trim())
//   return res.status(400).json({ success:false, message:'Title and starter content required' });
// const chain = await ChainStory.create({ title:title.trim(), starterContent:starterContent.trim(),
//   starterAuthor:userId, genre:genre||'other', previewImage, maxSegments:maxSegments||50,
//   requireVotes:requireVotes||3, contributors:[userId], contributorCount:1 });
// res.status(201).json({ success:true, data: chain });
void ChainController::create(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    Json::Value body(Json::objectValue);
    if (auto json = req->getJsonObject()) body = *json;

    const Json::Value& title = body["title"];
    const Json::Value& starterContent = body["starterContent"];

    // if (!title?.trim() || !starterContent?.trim())
    if (!isNonBlankString(title) || !isNonBlankString(starterContent)) {
      callback(failWith(drogon::k400BadRequest,
                        "Title and starter content required"));
      return;
    }

    const std::string userId = authedUserId(req);

    // Assemble the document ChainStory.create({...}) inserts, with the same
    // explicit fields the JS passed; applyDefaults fills the remaining schema
    // defaults (segmentCount, totalVotes, likes, status, isPublic, …) + timestamps.
    Json::Value doc(Json::objectValue);
    doc["title"] = jsTrim(title.asString());
    doc["starterContent"] = jsTrim(starterContent.asString());
    doc["starterAuthor"] = userId;
    // genre: genre || 'other'  (JS truthiness: empty string -> 'other').
    doc["genre"] = (body.isMember("genre") && body["genre"].isString() &&
                    !body["genre"].asString().empty())
                       ? body["genre"].asString()
                       : std::string("other");
    // previewImage: passed straight through (may be undefined -> absent).
    if (body.isMember("previewImage")) doc["previewImage"] = body["previewImage"];
    // maxSegments: maxSegments || 50.
    {
      const Json::Value& m = body["maxSegments"];
      const bool truthy = (m.isNumeric() && m.asDouble() != 0.0) ||
                          (m.isString() && !m.asString().empty()) ||
                          (m.isBool() && m.asBool());
      doc["maxSegments"] = truthy ? m : Json::Value(50);
    }
    // requireVotes: requireVotes || 3.
    {
      const Json::Value& r = body["requireVotes"];
      const bool truthy = (r.isNumeric() && r.asDouble() != 0.0) ||
                          (r.isString() && !r.asString().empty()) ||
                          (r.isBool() && r.asBool());
      doc["requireVotes"] = truthy ? r : Json::Value(3);
    }
    // contributors:[userId], contributorCount:1.
    Json::Value contributors(Json::arrayValue);
    contributors.append(userId);
    doc["contributors"] = contributors;
    doc["contributorCount"] = 1;

    doc = chainstory::applyDefaults(std::move(doc));

    // Build the insert with correct BSON types (ObjectId refs, Date timestamps,
    // int32 counters) so the stored shape matches Mongoose — fromJson would
    // store starterAuthor/contributors as plain strings and timestamps as
    // strings, which would break the { 'segments.author':1 } index and later
    // ObjectId comparisons in voteOnSegment. On create both segment arrays are
    // empty and contributors holds exactly the author, so we build it directly.
    bsoncxx::oid authorOid = pulse::bsonjson::oid(userId);
    bld::document insert{};
    insert.append(kvp("title", doc["title"].asString()));
    if (doc.isMember("previewImage")) {
      if (doc["previewImage"].isString())
        insert.append(kvp("previewImage", doc["previewImage"].asString()));
      else if (doc["previewImage"].isNull())
        insert.append(kvp("previewImage", bsoncxx::types::b_null{}));
    }
    insert.append(kvp("starterContent", doc["starterContent"].asString()));
    insert.append(kvp("starterAuthor", authorOid));
    insert.append(kvp("segments", bld::array{}));
    insert.append(kvp("pendingSegments", bld::array{}));
    insert.append(kvp("segmentCount", toIntCast(doc["segmentCount"], 0)));
    insert.append(kvp("contributorCount", toIntCast(doc["contributorCount"], 0)));
    insert.append(kvp("contributors", [&](bld::sub_array a) { a.append(authorOid); }));
    insert.append(kvp("totalVotes", toIntCast(doc["totalVotes"], 0)));
    insert.append(kvp("likes", toIntCast(doc["likes"], 0)));
    insert.append(kvp("status", doc["status"].asString()));
    insert.append(kvp("maxSegments", toIntCast(doc["maxSegments"], 50)));
    insert.append(kvp("isPublic", doc["isPublic"].asBool()));
    insert.append(kvp("allowAnyone", doc["allowAnyone"].asBool()));
    insert.append(kvp("requireVotes", toIntCast(doc["requireVotes"], 3)));
    insert.append(kvp("genre", doc["genre"].asString()));
    insert.append(kvp("tags", bld::array{}));
    insert.append(kvp("createdAt", bsoncxx::types::b_date{
                                       std::chrono::system_clock::now()}));
    insert.append(kvp("updatedAt", bsoncxx::types::b_date{
                                       std::chrono::system_clock::now()}));
    insert.append(kvp("__v", 0));

    // Insert and read the persisted document back (Mongoose returns the created
    // doc, which res.json serializes — includes _id and __v, no virtuals).
    Json::Value created;
    {
      auto col = pulse::db::collection(chainstory::kCollection);
      auto result = col.insert_one(insert.extract());
      bsoncxx::oid newId;
      if (result && result->inserted_id().type() == bsoncxx::type::k_oid)
        newId = result->inserted_id().get_oid().value;
      auto found = col.find_one(make_document(kvp("_id", newId)));
      if (found) created = pulse::bsonjson::toJson(found->view());
    }

    callback(okData(std::move(created), drogon::k201Created));
  } catch (const std::exception& e) {
    pulse::log::error("Create chain error: {}", e.what());
    callback(serverErrorWithMessage(e.what()));
  }
}

// ── POST /api/v1/chains/:chainId/segment — submitSegment ──────────────────────
// const { content, media } = req.body; const userId = req.user.userId;
// if (!content?.trim()) return res.status(400).json({ success:false, message:'Content required' });
// const chain = await ChainStory.findById(chainId);
// if (!chain) return res.status(404).json({ success:false, message:'Chain not found' });
// const segment = await chain.submitSegment(content.trim(), userId, media);
// res.status(201).json({ success:true, data: segment });
void ChainController::submitSegment(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string chainId) {
  try {
    Json::Value body(Json::objectValue);
    if (auto json = req->getJsonObject()) body = *json;

    const Json::Value& content = body["content"];
    // if (!content?.trim()) -> 400 'Content required'
    if (!isNonBlankString(content)) {
      callback(failWith(drogon::k400BadRequest, "Content required"));
      return;
    }

    const std::string userId = authedUserId(req);

    // findById: CastError on invalid id -> 500 with the message.
    bsoncxx::oid oid;
    try {
      oid = pulse::bsonjson::oid(chainId);
    } catch (const std::exception&) {
      callback(serverErrorWithMessage(
          "Cast to ObjectId failed for value \"" + chainId +
          "\" (type string) at path \"_id\" for model \"ChainStory\""));
      return;
    }

    // if (!chain) -> 404 'Chain not found' (checked before the instance method).
    {
      auto col = pulse::db::collection(chainstory::kCollection);
      if (!col.find_one(make_document(kvp("_id", oid)))) {
        callback(failWith(drogon::k404NotFound, "Chain not found"));
        return;
      }
    }

    // chain.submitSegment(content.trim(), userId, media) — model owns the
    // status/maxSegments guards (their throws land in the catch -> 500).
    std::optional<std::string> media;
    if (body.isMember("media") && body["media"].isString())
      media = body["media"].asString();
    bsoncxx::oid authorOid = pulse::bsonjson::oid(userId);

    Json::Value segment = chainstory::submitSegment(
        oid, jsTrim(content.asString()), authorOid, media);

    callback(okData(std::move(segment), drogon::k201Created));
  } catch (const std::exception& e) {
    pulse::log::error("Submit segment error: {}", e.what());
    callback(serverErrorWithMessage(e.what()));
  }
}

// ── GET /api/v1/chains/:chainId/pending — getPending ──────────────────────────
// const chain = await ChainStory.findById(chainId).select('pendingSegments title')
//   .populate('pendingSegments.author', 'username profile.avatar');
// if (!chain) return res.status(404).json({ success:false, message:'Chain not found' });
// res.json({ success:true, data: chain.pendingSegments });
void ChainController::getPending(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string chainId) {
  (void)req;
  try {
    bsoncxx::oid oid;
    try {
      oid = pulse::bsonjson::oid(chainId);
    } catch (const std::exception&) {
      callback(serverErrorWithMessage(
          "Cast to ObjectId failed for value \"" + chainId +
          "\" (type string) at path \"_id\" for model \"ChainStory\""));
      return;
    }

    Json::Value chain;
    {
      auto col = pulse::db::collection(chainstory::kCollection);
      mongocxx::options::find opts{};
      // .select('pendingSegments title')  (Mongoose adds _id by default).
      opts.projection(make_document(
          kvp("pendingSegments", 1), kvp("title", 1)));
      auto found = col.find_one(make_document(kvp("_id", oid)), opts);
      if (!found) {
        callback(failWith(drogon::k404NotFound, "Chain not found"));
        return;
      }
      chain = pulse::bsonjson::toJson(found->view());
    }

    Json::Value pending = chain.isMember("pendingSegments") &&
                                  chain["pendingSegments"].isArray()
                              ? chain["pendingSegments"]
                              : Json::Value(Json::arrayValue);

    // .populate('pendingSegments.author', 'username profile.avatar')
    std::vector<std::string> ids;
    for (const auto& seg : pending)
      if (seg.isMember("author")) collectRef(seg["author"], ids);
    const Json::Value byId = fetchUsersById(ids);
    for (auto& seg : pending) populateField(seg, "author", byId);

    // res.json({ success:true, data: chain.pendingSegments });
    callback(okData(std::move(pending)));
  } catch (const std::exception& e) {
    pulse::log::error("Get pending error: {}", e.what());
    callback(serverErrorWithMessage(e.what()));
  }
}

// ── POST /api/v1/chains/:chainId/segment/:segmentId/vote — voteSegment ─────────
// const { value } = req.body; const userId = req.user.userId;
// if (![1, -1].includes(value)) return res.status(400).json({ success:false, message:'Vote must be 1 or -1' });
// const chain = await ChainStory.findById(chainId);
// if (!chain) return res.status(404).json({ success:false, message:'Chain not found' });
// const result = await chain.voteOnSegment(segmentId, userId, value);
// res.json({ success:true, data: result });
void ChainController::voteSegment(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string chainId, std::string segmentId) {
  try {
    Json::Value body(Json::objectValue);
    if (auto json = req->getJsonObject()) body = *json;

    // if (![1, -1].includes(value)) — strict membership: value must be the
    // number 1 or -1 (JS [1,-1].includes uses SameValueZero, so the string "1"
    // does NOT match). Anything else -> 400.
    const Json::Value& valueJson = body["value"];
    int value = 0;
    bool valid = false;
    if (valueJson.isInt() || valueJson.isInt64() || valueJson.isUInt() ||
        valueJson.isUInt64()) {
      long long v = valueJson.asInt64();
      if (v == 1 || v == -1) { value = static_cast<int>(v); valid = true; }
    } else if (valueJson.isDouble()) {
      double v = valueJson.asDouble();
      if (v == 1.0 || v == -1.0) { value = static_cast<int>(v); valid = true; }
    }
    if (!valid) {
      callback(failWith(drogon::k400BadRequest, "Vote must be 1 or -1"));
      return;
    }

    const std::string userId = authedUserId(req);

    bsoncxx::oid chainOid;
    try {
      chainOid = pulse::bsonjson::oid(chainId);
    } catch (const std::exception&) {
      callback(serverErrorWithMessage(
          "Cast to ObjectId failed for value \"" + chainId +
          "\" (type string) at path \"_id\" for model \"ChainStory\""));
      return;
    }

    // if (!chain) -> 404 'Chain not found' (before the instance method).
    {
      auto col = pulse::db::collection(chainstory::kCollection);
      if (!col.find_one(make_document(kvp("_id", chainOid)))) {
        callback(failWith(drogon::k404NotFound, "Chain not found"));
        return;
      }
    }

    // chain.voteOnSegment(segmentId, userId, value) — model owns the
    // 'Segment not found' throw (-> catch -> 500 with the message).
    bsoncxx::oid segmentOid = pulse::bsonjson::oid(segmentId);
    bsoncxx::oid userOid = pulse::bsonjson::oid(userId);

    chainstory::VoteResult result =
        chainstory::voteOnSegment(chainOid, segmentOid, userOid, value);

    // res.json({ success:true, data: { votes, approved } });
    Json::Value data(Json::objectValue);
    data["votes"] = result.votes;
    data["approved"] = result.approved;
    callback(okData(std::move(data)));
  } catch (const std::exception& e) {
    pulse::log::error("Vote segment error: {}", e.what());
    callback(serverErrorWithMessage(e.what()));
  }
}

// ── POST /api/v1/chains/:chainId/like — likeChain ─────────────────────────────
// const userId = req.user.userId;
// const chain = await ChainStory.findById(chainId);
// if (!chain) return res.status(404).json({ success:false, message:'Chain not found' });
// const likes = await chain.toggleLike(userId);
// res.json({ success:true, data: { likes } });
void ChainController::likeChain(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string chainId) {
  try {
    const std::string userId = authedUserId(req);

    bsoncxx::oid chainOid;
    try {
      chainOid = pulse::bsonjson::oid(chainId);
    } catch (const std::exception&) {
      callback(serverErrorWithMessage(
          "Cast to ObjectId failed for value \"" + chainId +
          "\" (type string) at path \"_id\" for model \"ChainStory\""));
      return;
    }

    // if (!chain) -> 404 'Chain not found' (before the instance method).
    {
      auto col = pulse::db::collection(chainstory::kCollection);
      if (!col.find_one(make_document(kvp("_id", chainOid)))) {
        callback(failWith(drogon::k404NotFound, "Chain not found"));
        return;
      }
    }

    // chain.toggleLike(userId) -> new like count.
    bsoncxx::oid userOid = pulse::bsonjson::oid(userId);
    long long likes = chainstory::toggleLike(chainOid, userOid);

    // res.json({ success:true, data: { likes } });
    Json::Value data(Json::objectValue);
    data["likes"] = static_cast<Json::Int64>(likes);
    callback(okData(std::move(data)));
  } catch (const std::exception& e) {
    pulse::log::error("Like chain error: {}", e.what());
    callback(serverErrorWithMessage(e.what()));
  }
}
