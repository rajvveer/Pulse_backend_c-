// chainstory.cc — implementation of the ChainStory model port (chainstory.hpp).
//
// Ground truth: src/models/ChainStory.js.
//
// The schema's instance methods (submitSegment / voteOnSegment / toggleLike)
// mutate embedded subdocument arrays (`segments` / `pendingSegments`) with logic
// that does not map onto a single atomic update operator (re-voting splices the
// voters array; approval moves a subdocument between two arrays and conditionally
// flips counters/status). For 1:1 parity we therefore read the chain document,
// reproduce the exact JS mutation in Json space, and write the whole document
// back with replace_one — re-coercing the ObjectId/Date fields so the BSON types
// (and the { 'segments.author':1 } index) stay correct.
#include "pulse/models/chainstory.hpp"

#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/oid.hpp>

#include <mongocxx/collection.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/find_one_and_update.hpp>
#include <mongocxx/exception/exception.hpp>

#include <chrono>
#include <ctime>
#include <cstdio>
#include <stdexcept>

namespace pulse::models::chainstory {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;
using pulse::bsonjson::oid;
using pulse::bsonjson::tryOid;
using pulse::bsonjson::oidToHex;
using pulse::bsonjson::nowIso8601;

namespace {

bsoncxx::types::b_date nowDate() {
  return bsoncxx::types::b_date{std::chrono::system_clock::now()};
}

// Parse an ISO-8601 UTC timestamp (YYYY-MM-DDTHH:MM:SS.mmmZ — the shape
// bsonjson::toJson emits for BSON dates) back into a BSON b_date. Unparseable /
// empty input falls back to "now". Used to preserve original createdAt values
// when a fetched document is round-tripped through Json and replaced.
bsoncxx::types::b_date parseIsoDate(const Json::Value& v) {
  if (v.isString()) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0;
    if (std::sscanf(v.asString().c_str(), "%d-%d-%dT%d:%d:%d.%dZ",
                    &y, &mo, &d, &h, &mi, &s, &ms) >= 6) {
      std::tm tm{};
      tm.tm_year = y - 1900;
      tm.tm_mon  = mo - 1;
      tm.tm_mday = d;
      tm.tm_hour = h;
      tm.tm_min  = mi;
      tm.tm_sec  = s;
#if defined(_WIN32)
      std::time_t t = _mkgmtime(&tm);
#else
      std::time_t t = timegm(&tm);
#endif
      long long millis = static_cast<long long>(t) * 1000 + ms;
      return bsoncxx::types::b_date{std::chrono::milliseconds{millis}};
    }
  }
  return nowDate();
}

// Append a Json string field that is logically an ObjectId reference as a real
// BSON ObjectId when it is a valid 24-hex string; otherwise append as-is
// (null/absent stays absent). Keeps stored types matching how Mongoose persists
// ObjectId-ref fields so indexes/queries behave identically.
void appendOidOrNull(bld::document& out, const std::string& key,
                     const Json::Value& v) {
  if (v.isString()) {
    auto maybe = tryOid(v.asString());
    if (maybe) { out.append(kvp(key, *maybe)); return; }
    out.append(kvp(key, v.asString()));
    return;
  }
  if (v.isNull()) { out.append(kvp(key, bsoncxx::types::b_null{})); return; }
}

int asInt(const Json::Value& v, int dflt = 0) {
  if (v.isInt() || v.isInt64()) return static_cast<int>(v.asInt64());
  if (v.isUInt() || v.isUInt64()) return static_cast<int>(v.asUInt64());
  if (v.isDouble()) return static_cast<int>(v.asDouble());
  if (v.isBool()) return v.asBool() ? 1 : 0;
  return dflt;
}

// Build a BSON voter subdocument { user: ObjectId, value: Number } from Json.
bsoncxx::document::value buildVoter(const Json::Value& voter) {
  bld::document doc{};
  if (voter.isMember("user")) appendOidOrNull(doc, "user", voter["user"]);
  if (voter.isMember("value")) doc.append(kvp("value", asInt(voter["value"])));
  return doc.extract();
}

// Build a BSON segment subdocument from a (defaults-applied) Json segment.
// Mirrors segmentSchema field types: content(String), media(String),
// mediaType(String), author(ObjectId), votes(Number), voters([{user,value}]),
// isApproved(Boolean), createdAt(Date). _id is preserved/created as ObjectId.
bsoncxx::document::value buildSegment(const Json::Value& seg) {
  bld::document doc{};

  // _id: keep existing ObjectId, else mint a new one (Mongoose auto-_id on
  // subdocuments — needed so voteOnSegment can address it by segmentId).
  if (seg.isMember("_id") && seg["_id"].isString() &&
      tryOid(seg["_id"].asString())) {
    doc.append(kvp("_id", oid(seg["_id"].asString())));
  } else {
    doc.append(kvp("_id", bsoncxx::oid{}));
  }

  if (seg.isMember("content") && seg["content"].isString())
    doc.append(kvp("content", seg["content"].asString()));

  if (seg.isMember("media") && seg["media"].isString())
    doc.append(kvp("media", seg["media"].asString()));
  else if (seg.isMember("media") && seg["media"].isNull())
    doc.append(kvp("media", bsoncxx::types::b_null{}));

  doc.append(kvp("mediaType",
                 seg.isMember("mediaType") && seg["mediaType"].isString()
                     ? seg["mediaType"].asString()
                     : std::string("none")));

  if (seg.isMember("author")) appendOidOrNull(doc, "author", seg["author"]);

  doc.append(kvp("votes",
                 seg.isMember("votes") ? asInt(seg["votes"]) : 0));

  bld::array voters{};
  if (seg.isMember("voters") && seg["voters"].isArray()) {
    for (const auto& v : seg["voters"]) voters.append(buildVoter(v));
  }
  doc.append(kvp("voters", voters));

  doc.append(kvp("isApproved",
                 seg.isMember("isApproved") ? seg["isApproved"].asBool() : false));

  // createdAt: { default: Date.now }. Preserve an existing value (parsed from
  // its ISO string) so a save() does not reset it; default to now otherwise.
  doc.append(kvp("createdAt",
                 seg.isMember("createdAt") ? parseIsoDate(seg["createdAt"]) : nowDate()));

  return doc.extract();
}

void appendSegmentArray(bld::document& out, const std::string& key,
                        const Json::Value& arr) {
  bld::array a{};
  if (arr.isArray()) {
    for (const auto& seg : arr) a.append(buildSegment(seg));
  }
  out.append(kvp(key, a));
}

void appendOidArray(bld::document& out, const std::string& key,
                    const Json::Value& arr) {
  bld::array a{};
  if (arr.isArray()) {
    for (const auto& el : arr) {
      if (el.isString() && tryOid(el.asString()))
        a.append(oid(el.asString()));
      else if (el.isString())
        a.append(el.asString());
    }
  }
  out.append(kvp(key, a));
}

void appendStringArray(bld::document& out, const std::string& key,
                       const Json::Value& arr) {
  bld::array a{};
  if (arr.isArray()) {
    for (const auto& el : arr)
      if (el.isString()) a.append(el.asString());
  }
  out.append(kvp(key, a));
}

// Load a chain by id, returning its document as Json (toJson view).
std::optional<Json::Value> loadChain(mongocxx::collection& col,
                                     const bsoncxx::oid& chainId) {
  auto found = col.find_one(make_document(kvp("_id", chainId)));
  if (!found) return std::nullopt;
  return pulse::bsonjson::toJson(found->view());
}

// Persist a Json chain document back to Mongo with replace_one, refreshing
// updatedAt (timestamps: true on save()).
void saveChain(mongocxx::collection& col, const bsoncxx::oid& chainId,
               Json::Value doc) {
  // Build the typed BSON and overwrite createdAt with the existing value when
  // present, plus a fresh updatedAt — mirroring Mongoose save() with timestamps.
  bld::document out{};

  // _id always the addressed chain.
  out.append(kvp("_id", chainId));

  if (doc.isMember("title") && doc["title"].isString())
    out.append(kvp("title", doc["title"].asString()));
  if (doc.isMember("previewImage") && doc["previewImage"].isString())
    out.append(kvp("previewImage", doc["previewImage"].asString()));

  if (doc.isMember("starterContent") && doc["starterContent"].isString())
    out.append(kvp("starterContent", doc["starterContent"].asString()));
  if (doc.isMember("starterAuthor"))
    appendOidOrNull(out, "starterAuthor", doc["starterAuthor"]);

  appendSegmentArray(out, "segments",
                     doc.isMember("segments") ? doc["segments"] : Json::Value(Json::arrayValue));
  appendSegmentArray(out, "pendingSegments",
                     doc.isMember("pendingSegments") ? doc["pendingSegments"] : Json::Value(Json::arrayValue));

  out.append(kvp("segmentCount", doc.isMember("segmentCount") ? asInt(doc["segmentCount"]) : 0));
  out.append(kvp("contributorCount", doc.isMember("contributorCount") ? asInt(doc["contributorCount"]) : 0));
  appendOidArray(out, "contributors",
                 doc.isMember("contributors") ? doc["contributors"] : Json::Value(Json::arrayValue));
  out.append(kvp("totalVotes", doc.isMember("totalVotes") ? asInt(doc["totalVotes"]) : 0));
  out.append(kvp("likes", doc.isMember("likes") ? asInt(doc["likes"]) : 0));

  out.append(kvp("status",
                 doc.isMember("status") && doc["status"].isString()
                     ? doc["status"].asString()
                     : std::string("active")));
  out.append(kvp("maxSegments", doc.isMember("maxSegments") ? asInt(doc["maxSegments"]) : 50));

  out.append(kvp("isPublic", doc.isMember("isPublic") ? doc["isPublic"].asBool() : true));
  out.append(kvp("allowAnyone", doc.isMember("allowAnyone") ? doc["allowAnyone"].asBool() : true));
  out.append(kvp("requireVotes", doc.isMember("requireVotes") ? asInt(doc["requireVotes"]) : 3));

  out.append(kvp("genre",
                 doc.isMember("genre") && doc["genre"].isString()
                     ? doc["genre"].asString()
                     : std::string("other")));
  appendStringArray(out, "tags",
                    doc.isMember("tags") ? doc["tags"] : Json::Value(Json::arrayValue));

  // timestamps: true on save() — preserve the original createdAt (parsed from
  // its ISO string), refresh updatedAt to now.
  out.append(kvp("createdAt",
                 doc.isMember("createdAt") ? parseIsoDate(doc["createdAt"]) : nowDate()));
  out.append(kvp("updatedAt", nowDate()));

  if (doc.isMember("__v") && doc["__v"].isNumeric())
    out.append(kvp("__v", asInt(doc["__v"])));

  col.replace_one(make_document(kvp("_id", chainId)), out.extract());
}

} // namespace

// ---------------------------------------------------------------------------
// Indexes
// ---------------------------------------------------------------------------
void ensureIndexes() {
  try {
    auto col = pulse::db::collection(kCollection);

    // chainStorySchema.index({ status: 1, likes: -1 });
    col.create_index(make_document(kvp("status", 1), kvp("likes", -1)));

    // chainStorySchema.index({ genre: 1 });
    col.create_index(make_document(kvp("genre", 1)));

    // chainStorySchema.index({ 'segments.author': 1 });
    col.create_index(make_document(kvp("segments.author", 1)));

    pulse::log::info("ChainStory indexes ensured on collection '{}'", kCollection);
  } catch (const std::exception& e) {
    pulse::log::error("ChainStory ensureIndexes failed: {}", e.what());
    throw;
  }
}

// ---------------------------------------------------------------------------
// Defaults / serialization
// ---------------------------------------------------------------------------
Json::Value applySegmentDefaults(Json::Value segment) {
  if (!segment.isObject()) segment = Json::Value(Json::objectValue);

  // mediaType: { enum:['image','video','none'], default:'none' }
  if (!segment.isMember("mediaType")) segment["mediaType"] = "none";

  // votes: { default: 0 }
  if (!segment.isMember("votes")) segment["votes"] = 0;

  // voters: [{ user, value }] — defaults to empty array.
  if (!segment.isMember("voters") || !segment["voters"].isArray())
    segment["voters"] = Json::Value(Json::arrayValue);

  // isApproved: { default: false }
  if (!segment.isMember("isApproved")) segment["isApproved"] = false;

  // createdAt: { default: Date.now }
  if (!segment.isMember("createdAt")) segment["createdAt"] = nowIso8601();

  return segment;
}

Json::Value applyDefaults(Json::Value doc) {
  if (!doc.isObject()) doc = Json::Value(Json::objectValue);

  // segments / pendingSegments: arrays of segmentSchema. Default to [] and
  // apply per-segment defaults to any provided entries.
  for (const char* key : {"segments", "pendingSegments"}) {
    if (!doc.isMember(key) || !doc[key].isArray())
      doc[key] = Json::Value(Json::arrayValue);
    Json::Value& arr = doc[key];
    for (Json::ArrayIndex i = 0; i < arr.size(); ++i)
      arr[i] = applySegmentDefaults(arr[i]);
  }

  // segmentCount: { default: 0 }
  if (!doc.isMember("segmentCount")) doc["segmentCount"] = 0;
  // contributorCount: { default: 0 }
  if (!doc.isMember("contributorCount")) doc["contributorCount"] = 0;
  // contributors: [ObjectId] — defaults to empty array.
  if (!doc.isMember("contributors") || !doc["contributors"].isArray())
    doc["contributors"] = Json::Value(Json::arrayValue);
  // totalVotes: { default: 0 }
  if (!doc.isMember("totalVotes")) doc["totalVotes"] = 0;
  // likes: { default: 0 }
  if (!doc.isMember("likes")) doc["likes"] = 0;

  // status: { enum:['active','complete','archived'], default:'active' }
  if (!doc.isMember("status")) doc["status"] = "active";
  // maxSegments: { default: 50 }
  if (!doc.isMember("maxSegments")) doc["maxSegments"] = 50;

  // isPublic: { default: true }
  if (!doc.isMember("isPublic")) doc["isPublic"] = true;
  // allowAnyone: { default: true }
  if (!doc.isMember("allowAnyone")) doc["allowAnyone"] = true;
  // requireVotes: { default: 3 }
  if (!doc.isMember("requireVotes")) doc["requireVotes"] = 3;

  // genre: { enum:[...], default:'other' }
  if (!doc.isMember("genre")) doc["genre"] = "other";
  // tags: [String] — defaults to empty array.
  if (!doc.isMember("tags") || !doc["tags"].isArray())
    doc["tags"] = Json::Value(Json::arrayValue);

  // timestamps: true -> createdAt / updatedAt
  const std::string now = nowIso8601();
  if (!doc.isMember("createdAt")) doc["createdAt"] = now;
  if (!doc.isMember("updatedAt")) doc["updatedAt"] = now;

  // Mongoose version key.
  if (!doc.isMember("__v")) doc["__v"] = 0;

  return doc;
}

Json::Value sanitizeForOutput(Json::Value doc) {
  if (!doc.isObject()) return doc;

  // Surface the Mongoose `id` virtual (hex of _id) when available.
  if (!doc.isMember("id") && doc.isMember("_id")) {
    const Json::Value& id = doc["_id"];
    if (id.isString()) doc["id"] = id.asString();
    else if (id.isObject() && id.isMember("$oid")) doc["id"] = id["$oid"];
  }

  // The ChainStory schema declares no select:false / sensitive fields; the
  // default Mongoose toJSON keeps every field aside from the version key.
  if (doc.isMember("__v")) doc.removeMember("__v");
  return doc;
}

// ---------------------------------------------------------------------------
// Virtuals
// ---------------------------------------------------------------------------
std::string lastSegment(const Json::Value& doc) {
  // if (this.segments.length === 0) return this.starterContent;
  // return this.segments[this.segments.length - 1].content;
  const Json::Value& segs = doc["segments"];
  if (!segs.isArray() || segs.empty()) {
    return doc.isMember("starterContent") && doc["starterContent"].isString()
               ? doc["starterContent"].asString()
               : std::string();
  }
  const Json::Value& last = segs[segs.size() - 1];
  return last.isMember("content") && last["content"].isString()
             ? last["content"].asString()
             : std::string();
}

// ---------------------------------------------------------------------------
// Statics
// ---------------------------------------------------------------------------
Json::Value getActiveChains(const ActiveChainsOptions& options) {
  try {
    auto col = pulse::db::collection(kCollection);

    // const query = { status: 'active' }; if (genre) query.genre = genre;
    bld::document filter{};
    filter.append(kvp("status", "active"));
    if (!options.genre.empty()) filter.append(kvp("genre", options.genre));

    mongocxx::options::find opts{};
    // .sort({ likes: -1, contributorCount: -1 })
    opts.sort(make_document(kvp("likes", -1), kvp("contributorCount", -1)));
    // .skip(skip).limit(limit)
    opts.skip(static_cast<std::int64_t>(options.skip));
    opts.limit(static_cast<std::int64_t>(options.limit));
    // .select('-pendingSegments -segments.voters')
    opts.projection(make_document(
        kvp("pendingSegments", 0),
        kvp("segments.voters", 0)));

    Json::Value out(Json::arrayValue);
    auto cursor = col.find(filter.extract(), opts);
    for (const auto& view : cursor) {
      out.append(sanitizeForOutput(pulse::bsonjson::toJson(view)));
    }
    return out;
  } catch (const std::exception& e) {
    pulse::log::error("ChainStory getActiveChains failed: {}", e.what());
    throw;
  }
}

// ---------------------------------------------------------------------------
// Instance methods
// ---------------------------------------------------------------------------
Json::Value submitSegment(const bsoncxx::oid& chainId,
                          const std::string& content,
                          const bsoncxx::oid& authorId,
                          const std::optional<std::string>& media) {
  auto col = pulse::db::collection(kCollection);

  auto loaded = loadChain(col, chainId);
  if (!loaded) throw std::runtime_error("Chain not found");
  Json::Value chain = *loaded;

  // if (this.status !== 'active') throw ...
  const std::string status =
      chain.isMember("status") && chain["status"].isString()
          ? chain["status"].asString() : "active";
  if (status != "active")
    throw std::runtime_error("This chain is no longer accepting submissions");

  // if (this.segmentCount >= this.maxSegments) throw ...
  const int segmentCount = chain.isMember("segmentCount") ? chain["segmentCount"].asInt() : 0;
  const int maxSegments = chain.isMember("maxSegments") ? chain["maxSegments"].asInt() : 50;
  if (segmentCount >= maxSegments)
    throw std::runtime_error("This chain has reached maximum segments");

  // mediaType = media ? (media.includes('.mp4') ? 'video' : 'image') : 'none'
  std::string mediaType = "none";
  if (media && !media->empty()) {
    mediaType = (media->find(".mp4") != std::string::npos) ? "video" : "image";
  }

  // Build the new segment (segmentSchema defaults applied).
  Json::Value segment(Json::objectValue);
  segment["content"] = content;
  segment["author"] = oidToHex(authorId);
  if (media && !media->empty()) segment["media"] = *media;
  else segment["media"] = Json::Value(Json::nullValue);
  segment["mediaType"] = mediaType;
  segment = applySegmentDefaults(segment);

  // this.pendingSegments.push(segment); await this.save();
  if (!chain.isMember("pendingSegments") || !chain["pendingSegments"].isArray())
    chain["pendingSegments"] = Json::Value(Json::arrayValue);
  chain["pendingSegments"].append(segment);

  saveChain(col, chainId, chain);

  // return segment;
  return segment;
}

VoteResult voteOnSegment(const bsoncxx::oid& chainId,
                         const bsoncxx::oid& segmentId,
                         const bsoncxx::oid& userId,
                         int value) {
  auto col = pulse::db::collection(kCollection);

  auto loaded = loadChain(col, chainId);
  if (!loaded) throw std::runtime_error("Chain not found");
  Json::Value chain = *loaded;

  const std::string segIdHex = oidToHex(segmentId);
  const std::string userIdHex = oidToHex(userId);

  if (!chain.isMember("pendingSegments") || !chain["pendingSegments"].isArray())
    throw std::runtime_error("Segment not found");
  Json::Value& pending = chain["pendingSegments"];

  // const segment = this.pendingSegments.id(segmentId);
  Json::ArrayIndex segIdx = pending.size();
  for (Json::ArrayIndex i = 0; i < pending.size(); ++i) {
    if (pending[i].isMember("_id") && pending[i]["_id"].isString() &&
        pending[i]["_id"].asString() == segIdHex) {
      segIdx = i;
      break;
    }
  }
  if (segIdx == pending.size()) throw std::runtime_error("Segment not found");

  Json::Value segment = pending[segIdx];
  if (!segment.isMember("voters") || !segment["voters"].isArray())
    segment["voters"] = Json::Value(Json::arrayValue);
  Json::Value& voters = segment["voters"];

  // const existingIdx = segment.voters.findIndex(v => v.user == userId);
  int existingIdx = -1;
  for (Json::ArrayIndex i = 0; i < voters.size(); ++i) {
    const Json::Value& vu = voters[i]["user"];
    std::string vuHex = vu.isString() ? vu.asString() : std::string();
    if (vuHex == userIdHex) { existingIdx = static_cast<int>(i); break; }
  }

  int votes = segment.isMember("votes") ? segment["votes"].asInt() : 0;

  if (existingIdx > -1) {
    // segment.votes -= segment.voters[existingIdx].value;
    votes -= voters[static_cast<Json::ArrayIndex>(existingIdx)]["value"].asInt();
    // segment.voters.splice(existingIdx, 1);
    Json::Value newVoters(Json::arrayValue);
    for (Json::ArrayIndex i = 0; i < voters.size(); ++i)
      if (static_cast<int>(i) != existingIdx) newVoters.append(voters[i]);
    voters = newVoters;
  }

  // segment.voters.push({ user: userId, value }); segment.votes += value;
  Json::Value newVote(Json::objectValue);
  newVote["user"] = userIdHex;
  newVote["value"] = value;
  voters.append(newVote);
  votes += value;
  segment["votes"] = votes;

  bool isApproved = segment.isMember("isApproved") && segment["isApproved"].asBool();
  const int requireVotes = chain.isMember("requireVotes") ? chain["requireVotes"].asInt() : 3;
  const int maxSegments = chain.isMember("maxSegments") ? chain["maxSegments"].asInt() : 50;

  // Write the mutated segment back into pendingSegments before any move.
  pending[segIdx] = segment;

  // if (segment.votes >= this.requireVotes && !segment.isApproved) { ... }
  if (votes >= requireVotes && !isApproved) {
    segment["isApproved"] = true;
    isApproved = true;

    // this.segments.push(segment);
    if (!chain.isMember("segments") || !chain["segments"].isArray())
      chain["segments"] = Json::Value(Json::arrayValue);
    chain["segments"].append(segment);

    // this.pendingSegments.pull(segmentId);
    Json::Value newPending(Json::arrayValue);
    for (Json::ArrayIndex i = 0; i < pending.size(); ++i)
      if (i != segIdx) newPending.append(pending[i]);
    chain["pendingSegments"] = newPending;

    // this.segmentCount++;
    int segmentCount = (chain.isMember("segmentCount") ? chain["segmentCount"].asInt() : 0) + 1;
    chain["segmentCount"] = segmentCount;

    // if (!this.contributors.includes(segment.author)) { push; contributorCount++; }
    const std::string authorHex =
        segment.isMember("author") && segment["author"].isString()
            ? segment["author"].asString() : std::string();
    if (!chain.isMember("contributors") || !chain["contributors"].isArray())
      chain["contributors"] = Json::Value(Json::arrayValue);
    Json::Value& contributors = chain["contributors"];
    bool present = false;
    for (const auto& c : contributors)
      if (c.isString() && c.asString() == authorHex) { present = true; break; }
    if (!present) {
      contributors.append(authorHex);
      chain["contributorCount"] =
          (chain.isMember("contributorCount") ? chain["contributorCount"].asInt() : 0) + 1;
    }

    // if (this.segmentCount >= this.maxSegments) this.status = 'complete';
    if (segmentCount >= maxSegments) chain["status"] = "complete";
  }

  // this.totalVotes++;
  chain["totalVotes"] =
      (chain.isMember("totalVotes") ? chain["totalVotes"].asInt() : 0) + 1;

  saveChain(col, chainId, chain);

  // return { votes: segment.votes, approved: segment.isApproved };
  VoteResult result;
  result.votes = votes;
  result.approved = isApproved;
  return result;
}

long long toggleLike(const bsoncxx::oid& chainId, const bsoncxx::oid& /*userId*/) {
  auto col = pulse::db::collection(kCollection);

  // this.likes++; await this.save(); return this.likes;
  // Faithful, race-safe equivalent: $inc likes by 1 and read back the new value.
  auto update = make_document(kvp("$inc", make_document(kvp("likes", 1))));

  mongocxx::options::find_one_and_update fo{};
  fo.return_document(mongocxx::options::return_document::k_after);

  auto result = col.find_one_and_update(
      make_document(kvp("_id", chainId)), update.view(), fo);
  if (!result) throw std::runtime_error("Chain not found");

  auto view = result->view();
  auto it = view.find("likes");
  if (it == view.end()) return 0;
  switch (it->type()) {
    case bsoncxx::type::k_int32:  return it->get_int32().value;
    case bsoncxx::type::k_int64:  return it->get_int64().value;
    case bsoncxx::type::k_double: return static_cast<long long>(it->get_double().value);
    default: return 0;
  }
}

} // namespace pulse::models::chainstory
