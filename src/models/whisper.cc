// whisper.cc — implementation of the Whisper model port (see whisper.hpp).
//
// Ground truth: src/models/Whisper.js. Ports the schema indexes, the field
// defaults/enums Mongoose applies on insert, the anonymizing toJSON-style
// output transform, and the statics/instance helpers (getNearby, vote,
// addReply) preserving the exact filters, projections, sorts, limits, and
// update logic.
#include "pulse/models/whisper.hpp"

#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/oid.hpp>

#include <mongocxx/collection.hpp>
#include <mongocxx/cursor.hpp>
#include <mongocxx/options/index.hpp>
#include <mongocxx/options/find.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <string>

namespace pulse::models::whisper {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;
using bld::make_array;
using pulse::bsonjson::tryOid;
using pulse::bsonjson::oidToHex;

namespace {

// "now" as a BSON date (matches Mongoose `new Date()` / Date.now()).
bsoncxx::types::b_date nowDate() {
  return bsoncxx::types::b_date{std::chrono::system_clock::now()};
}

// Render an absolute time point as an ISO-8601 UTC string (YYYY-MM-DDTHH:MM:SS.000Z),
// matching pulse::bsonjson::nowIso8601 formatting.
std::string toIso8601(std::chrono::system_clock::time_point when) {
  std::time_t tt = std::chrono::system_clock::to_time_t(when);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &tt);
#else
  gmtime_r(&tt, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &tm);
  return std::string(buf);
}

// Mongoose `trim: true` — strip leading/trailing ASCII whitespace.
std::string trim(const std::string& s) {
  std::size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

} // namespace

// ---------------------------------------------------------------------------
// Indexes
// ---------------------------------------------------------------------------
void ensureIndexes() {
  try {
    auto col = pulse::db::collection(kCollection);

    // whisperSchema.index({ location: '2dsphere' });
    col.create_index(make_document(kvp("location", "2dsphere")));

    // whisperSchema.index({ score: -1, createdAt: -1 });
    col.create_index(make_document(kvp("score", -1), kvp("createdAt", -1)));

    // expiresAt field: index: { expires: 0 } -> TTL index, expireAfterSeconds: 0.
    {
      mongocxx::options::index opts{};
      opts.expire_after(std::chrono::seconds(0));
      col.create_index(make_document(kvp("expiresAt", 1)), opts);
    }

    pulse::log::info("Whisper indexes ensured on collection '{}'", kCollection);
  } catch (const std::exception& e) {
    pulse::log::error("Whisper ensureIndexes failed: {}", e.what());
    throw;
  }
}

// ---------------------------------------------------------------------------
// Defaults / serialization
// ---------------------------------------------------------------------------
std::string defaultExpiry() {
  return toIso8601(std::chrono::system_clock::now() +
                   std::chrono::milliseconds(kExpiryTtlMs));
}

Json::Value applyDefaults(Json::Value doc) {
  if (!doc.isObject()) doc = Json::Value(Json::objectValue);

  // content: trim (Mongoose `trim: true`).
  if (doc.isMember("content") && doc["content"].isString()) {
    doc["content"] = trim(doc["content"].asString());
  }

  // location.type: enum ['Point'], default 'Point'.
  if (!doc.isMember("location") || !doc["location"].isObject()) {
    doc["location"] = Json::Value(Json::objectValue);
  }
  Json::Value& location = doc["location"];
  if (!location.isMember("type") || !location["type"].isString() ||
      location["type"].asString().empty()) {
    location["type"] = "Point";
  }

  // upvotes / downvotes / score: default 0.
  if (!doc.isMember("upvotes"))   doc["upvotes"] = 0;
  if (!doc.isMember("downvotes")) doc["downvotes"] = 0;
  if (!doc.isMember("score"))     doc["score"] = 0;

  // voters: [] default. Each voter is { user, vote } (no defaulted subfields).
  if (!doc.isMember("voters") || !doc["voters"].isArray()) {
    doc["voters"] = Json::Value(Json::arrayValue);
  }

  // replies: [] default, with per-reply defaults (upvotes:0, createdAt:now).
  if (!doc.isMember("replies") || !doc["replies"].isArray()) {
    doc["replies"] = Json::Value(Json::arrayValue);
  } else {
    // Per-reply defaults: upvotes: 0, createdAt: now (reply.content has only a
    // maxlength 200 constraint, no trim/default).
    const std::string now = pulse::bsonjson::nowIso8601();
    for (Json::Value& reply : doc["replies"]) {
      if (!reply.isObject()) continue;
      if (!reply.isMember("upvotes"))   reply["upvotes"] = 0;
      if (!reply.isMember("createdAt")) reply["createdAt"] = now;
    }
  }

  // reports: default 0.
  if (!doc.isMember("reports")) doc["reports"] = 0;

  // isHidden: default false.
  if (!doc.isMember("isHidden")) doc["isHidden"] = false;

  // expiresAt: default now + 24h.
  if (!doc.isMember("expiresAt") || !doc["expiresAt"].isString() ||
      doc["expiresAt"].asString().empty()) {
    doc["expiresAt"] = defaultExpiry();
  }

  // timestamps: true -> createdAt / updatedAt on insert.
  const std::string now = pulse::bsonjson::nowIso8601();
  if (!doc.isMember("createdAt")) doc["createdAt"] = now;
  if (!doc.isMember("updatedAt")) doc["updatedAt"] = now;

  return doc;
}

Json::Value sanitizeForOutput(Json::Value doc) {
  if (!doc.isObject()) return doc;

  // Anonymous model: strip the select:false / sensitive fields the schema hides.
  // author (top-level, never exposed).
  doc.removeMember("author");

  // voters[].user (select:false on the voter subdoc's user).
  if (doc.isMember("voters") && doc["voters"].isArray()) {
    for (Json::Value& v : doc["voters"]) {
      if (v.isObject()) v.removeMember("user");
    }
  }

  // replies[].author (select:false on the reply subdoc's author).
  if (doc.isMember("replies") && doc["replies"].isArray()) {
    for (Json::Value& r : doc["replies"]) {
      if (r.isObject()) r.removeMember("author");
    }
  }

  // Mongoose internal version key.
  doc.removeMember("__v");

  return doc;
}

// ---------------------------------------------------------------------------
// Statics
// ---------------------------------------------------------------------------
Json::Value getNearby(double lng, double lat, double radiusKm, int limit) {
  auto col = pulse::db::collection(kCollection);

  // {
  //   location: { $near: { $geometry: { type:'Point', coordinates:[lng,lat] },
  //                        $maxDistance: radiusKm * 1000 } },
  //   isHidden: false
  // }
  auto filter = make_document(
      kvp("location", make_document(
          kvp("$near", make_document(
              kvp("$geometry", make_document(
                  kvp("type", "Point"),
                  kvp("coordinates", make_array(lng, lat)))),
              kvp("$maxDistance", radiusKm * 1000))))),
      kvp("isHidden", false));

  // .sort({ score: -1, createdAt: -1 }).limit(limit)
  //   .select('-author -voters.user -replies.author')
  mongocxx::options::find opts{};
  opts.sort(make_document(kvp("score", -1), kvp("createdAt", -1)));
  opts.limit(limit);
  opts.projection(make_document(
      kvp("author", 0),
      kvp("voters.user", 0),
      kvp("replies.author", 0)));

  Json::Value out(Json::arrayValue);
  auto cursor = col.find(filter.view(), opts);
  for (auto&& view : cursor) {
    out.append(pulse::bsonjson::toJson(view));
  }
  return out;
}

std::optional<VoteResult> vote(const std::string& whisperId,
                               const std::string& userId,
                               const std::string& voteType) {
  auto col = pulse::db::collection(kCollection);

  auto whisperOid = tryOid(whisperId);
  if (!whisperOid) return std::nullopt;  // invalid id -> no such whisper

  // findById(whisperId).select('+voters.user') — voters.user is select:false so
  // it must be explicitly included; the rest of the doc loads by default.
  auto found = col.find_one(make_document(kvp("_id", *whisperOid)));
  if (!found) return std::nullopt;       // JS: throw new Error('Whisper not found')

  Json::Value whisper = pulse::bsonjson::toJson(found->view());

  long long upvotes   = whisper.isMember("upvotes")   ? whisper["upvotes"].asInt64()   : 0;
  long long downvotes = whisper.isMember("downvotes") ? whisper["downvotes"].asInt64() : 0;

  // voters array of { user, vote }. Find an existing vote by this user.
  Json::Value voters = whisper.isMember("voters") && whisper["voters"].isArray()
                           ? whisper["voters"]
                           : Json::Value(Json::arrayValue);

  auto voterUserId = [](const Json::Value& v) -> std::string {
    if (!v.isObject() || !v.isMember("user")) return "";
    const Json::Value& u = v["user"];
    if (u.isString()) return u.asString();
    if (u.isObject() && u.isMember("$oid")) return u["$oid"].asString();
    return "";
  };

  int existingIdx = -1;
  for (Json::ArrayIndex i = 0; i < voters.size(); ++i) {
    if (voterUserId(voters[i]) == userId) { existingIdx = static_cast<int>(i); break; }
  }

  if (existingIdx > -1) {
    const std::string existingVote = voters[static_cast<Json::ArrayIndex>(existingIdx)]
                                         .get("vote", "")
                                         .asString();
    if (existingVote == voteType) {
      // Remove vote (toggle off).
      Json::Value newVoters(Json::arrayValue);
      for (Json::ArrayIndex i = 0; i < voters.size(); ++i) {
        if (static_cast<int>(i) != existingIdx) newVoters.append(voters[i]);
      }
      voters = newVoters;
      if (voteType == "up") --upvotes; else --downvotes;
    } else {
      // Change vote.
      if (existingVote == "up") --upvotes; else --downvotes;
      voters[static_cast<Json::ArrayIndex>(existingIdx)]["vote"] = voteType;
      if (voteType == "up") ++upvotes; else ++downvotes;
    }
  } else {
    // New vote.
    Json::Value newVoter(Json::objectValue);
    newVoter["user"] = userId;
    newVoter["vote"] = voteType;
    voters.append(newVoter);
    if (voteType == "up") ++upvotes; else ++downvotes;
  }

  const long long score = upvotes - downvotes;

  // Rebuild the voters BSON array, preserving each voter's user ObjectId.
  bld::array votersArr;
  for (const Json::Value& v : voters) {
    if (!v.isObject()) continue;
    const std::string uId = voterUserId(v);
    const std::string vt = v.get("vote", "").asString();
    if (auto uOid = tryOid(uId)) {
      votersArr.append(make_document(kvp("user", *uOid), kvp("vote", vt)));
    } else {
      votersArr.append(make_document(kvp("user", uId), kvp("vote", vt)));
    }
  }

  // whisper.save() — persist updated voters, tallies, score, and updatedAt.
  auto update = make_document(kvp("$set", make_document(
      kvp("voters", votersArr.extract()),
      kvp("upvotes", upvotes),
      kvp("downvotes", downvotes),
      kvp("score", score),
      kvp("updatedAt", nowDate()))));

  col.update_one(make_document(kvp("_id", *whisperOid)), update.view());

  VoteResult result;
  result.upvotes = upvotes;
  result.downvotes = downvotes;
  result.score = score;
  return result;
}

// ---------------------------------------------------------------------------
// Instance methods
// ---------------------------------------------------------------------------
std::optional<Json::Value> addReply(const std::string& whisperId,
                                    const std::string& content,
                                    const std::string& authorId) {
  auto col = pulse::db::collection(kCollection);

  auto whisperOid = tryOid(whisperId);
  if (!whisperOid) return std::nullopt;

  // this.replies.push({ content, author: authorId }); await this.save();
  // Reply subfield defaults: upvotes: 0, createdAt: now.
  const bsoncxx::types::b_date createdAt = nowDate();

  bld::document reply;
  reply.append(kvp("content", content));
  if (auto aOid = tryOid(authorId)) {
    reply.append(kvp("author", *aOid));
  } else {
    reply.append(kvp("author", authorId));
  }
  reply.append(kvp("upvotes", 0));
  reply.append(kvp("createdAt", createdAt));
  // Mongoose assigns each subdocument its own _id.
  const bsoncxx::oid replyId;
  reply.append(kvp("_id", replyId));

  auto update = make_document(
      kvp("$push", make_document(kvp("replies", reply.extract()))),
      kvp("$set", make_document(kvp("updatedAt", nowDate()))));

  auto result = col.update_one(make_document(kvp("_id", *whisperOid)), update.view());
  if (!result || result->matched_count() == 0) return std::nullopt;

  // Return the freshly appended reply (this.replies[this.replies.length - 1]),
  // shaped like the persisted subdocument.
  Json::Value out(Json::objectValue);
  out["_id"]       = oidToHex(replyId);
  out["content"]   = content;
  out["author"]    = authorId;
  out["upvotes"]   = 0;
  out["createdAt"] = toIso8601(std::chrono::system_clock::now());
  return out;
}

} // namespace pulse::models::whisper
