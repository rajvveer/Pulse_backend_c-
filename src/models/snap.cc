// snap.cc — implementation of the Snap model port (see snap.hpp).
#include "pulse/models/snap.hpp"

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
#include <mongocxx/pipeline.hpp>

#include <chrono>
#include <set>
#include <unordered_map>
#include <algorithm>

namespace pulse::models::snap {

namespace bbuild = bsoncxx::builder::basic;
using bbuild::kvp;
using bbuild::make_document;
using bbuild::make_array;

namespace {

// "now" as a BSON date for $gt:expiresAt comparisons.
bsoncxx::types::b_date nowDate() {
  return bsoncxx::types::b_date{std::chrono::system_clock::now()};
}

// The fields the JS .populate('user', 'username name avatar profile.avatar
// isVerified') brings in. We replicate this with a $lookup + projection.
bsoncxx::document::value userProjection() {
  return make_document(
      kvp("username", 1),
      kvp("name", 1),
      kvp("avatar", 1),
      kvp("profile.avatar", 1),
      kvp("isVerified", 1));
}

} // namespace

// ---------------------------------------------------------------------------
// Indexes
// ---------------------------------------------------------------------------
void ensureIndexes() {
  auto col = pulse::db::collection(kCollection);

  // Field-level: user: { index: true }
  col.create_index(make_document(kvp("user", 1)));

  // TTL: expiresAt: { index: { expires: 0 } } -> expireAfterSeconds: 0
  {
    mongocxx::options::index opts;
    opts.expire_after(std::chrono::seconds(0));
    col.create_index(make_document(kvp("expiresAt", 1)), opts);
  }

  // Story rail query: newest active story snaps by a set of authors.
  col.create_index(make_document(
      kvp("audience", 1), kvp("user", 1), kvp("createdAt", -1)));

  // Direct inbox query: snaps addressed to a recipient, newest first.
  col.create_index(make_document(
      kvp("audience", 1), kvp("recipients", 1), kvp("createdAt", -1)));

  pulse::log::info("[Snap] ensured indexes on collection {}", kCollection);
}

// ---------------------------------------------------------------------------
// Defaults / serialization
// ---------------------------------------------------------------------------
Json::Value applyDefaults(Json::Value doc) {
  // audience: enum ['story','direct'], default 'story', required
  if (!doc.isMember("audience") || !doc["audience"].isString() ||
      doc["audience"].asString().empty()) {
    doc["audience"] = "story";
  }

  // recipients: [] for story snaps (default empty array)
  if (!doc.isMember("recipients") || !doc["recipients"].isArray()) {
    doc["recipients"] = Json::Value(Json::arrayValue);
  }

  // mediaType: enum ['image','video'], default 'image'
  if (!doc.isMember("mediaType") || !doc["mediaType"].isString() ||
      doc["mediaType"].asString().empty()) {
    doc["mediaType"] = "image";
  }

  // durationMs: default 5000 (how long an image snap shows)
  if (!doc.isMember("durationMs")) {
    doc["durationMs"] = 5000;
  }

  // caption: trim (no default value, but normalize whitespace if present)
  if (doc.isMember("caption") && doc["caption"].isString()) {
    std::string c = doc["caption"].asString();
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    c.erase(c.begin(), std::find_if(c.begin(), c.end(), notSpace));
    c.erase(std::find_if(c.rbegin(), c.rend(), notSpace).base(), c.end());
    doc["caption"] = c;
  }

  // viewers: [] default
  if (!doc.isMember("viewers") || !doc["viewers"].isArray()) {
    doc["viewers"] = Json::Value(Json::arrayValue);
  }

  // viewCount: default 0
  if (!doc.isMember("viewCount")) {
    doc["viewCount"] = 0;
  }

  // reactions: Map of String, default {}
  if (!doc.isMember("reactions") || !doc["reactions"].isObject()) {
    doc["reactions"] = Json::Value(Json::objectValue);
  }

  // expiresAt: required; controllers always set one, but default to 24h TTL.
  if (!doc.isMember("expiresAt") || !doc["expiresAt"].isString() ||
      doc["expiresAt"].asString().empty()) {
    doc["expiresAt"] = defaultExpiry();
  }

  // timestamps: { createdAt, updatedAt }
  std::string now = pulse::bsonjson::nowIso8601();
  if (!doc.isMember("createdAt")) doc["createdAt"] = now;
  doc["updatedAt"] = now;

  return doc;
}

Json::Value sanitizeForOutput(Json::Value doc) {
  // No select:false / sensitive fields on this schema; strip Mongoose version key.
  doc.removeMember("__v");
  return doc;
}

// ---------------------------------------------------------------------------
// Statics
// ---------------------------------------------------------------------------
std::string defaultExpiry() {
  auto when = std::chrono::system_clock::now() +
              std::chrono::milliseconds(kDefaultTtlMs);
  // Render ISO-8601 UTC to match nowIso8601 formatting.
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

Json::Value getStoryRail(const std::string& viewerId,
                         const std::vector<std::string>& followingIds) {
  // authorIds = [...new Set([viewerId, ...followingIds])]
  std::vector<std::string> authorOrder;
  std::set<std::string> seenIds;
  auto pushId = [&](const std::string& id) {
    if (id.empty()) return;
    if (seenIds.insert(id).second) authorOrder.push_back(id);
  };
  pushId(viewerId);
  for (const auto& f : followingIds) pushId(f);

  // Build the $in array of ObjectIds (skip any non-oid strings defensively).
  bbuild::array inArr;
  for (const auto& id : authorOrder) {
    if (auto o = pulse::bsonjson::tryOid(id)) inArr.append(*o);
  }

  auto col = pulse::db::collection(kCollection);

  // find({ audience:'story', user:{$in:authorIds}, expiresAt:{$gt:now} })
  //   .sort({ createdAt: 1 }).populate('user', ...).lean()
  // Implemented as an aggregation so we can replicate the populate via $lookup.
  mongocxx::pipeline p;
  p.match(make_document(
      kvp("audience", "story"),
      kvp("user", make_document(kvp("$in", inArr.extract()))),
      kvp("expiresAt", make_document(kvp("$gt", nowDate())))));
  p.sort(make_document(kvp("createdAt", 1)));
  p.lookup(make_document(
      kvp("from", "users"),
      kvp("localField", "user"),
      kvp("foreignField", "_id"),
      kvp("pipeline", make_array(
          make_document(kvp("$project", userProjection())))),
      kvp("as", "userDoc")));
  p.add_fields(make_document(
      kvp("user", make_document(kvp("$ifNull", make_array(
          make_document(kvp("$arrayElemAt", make_array("$userDoc", 0))),
          "$user"))))));
  p.project(make_document(kvp("userDoc", 0)));

  auto cursor = col.aggregate(p);

  // Group by author into rings, tracking whether the viewer has seen all snaps.
  struct Ring {
    Json::Value user;             // populated user doc (or raw oid)
    Json::Value snaps = Json::Value(Json::arrayValue);
    bool hasUnseen = false;
    int order = 0;                // insertion order (Map iteration order in JS)
  };
  std::unordered_map<std::string, Ring> byAuthor;
  int nextOrder = 0;

  for (auto&& view : cursor) {
    Json::Value snap = pulse::bsonjson::toJson(view);

    // authorId = (snap.user?._id || snap.user).toString()
    std::string authorId;
    const Json::Value& u = snap["user"];
    if (u.isObject() && u.isMember("_id")) authorId = u["_id"].asString();
    else if (u.isString()) authorId = u.asString();

    auto it = byAuthor.find(authorId);
    if (it == byAuthor.end()) {
      Ring r;
      r.user = u;
      r.order = nextOrder++;
      it = byAuthor.emplace(authorId, std::move(r)).first;
    }
    Ring& group = it->second;

    // seen = (snap.viewers || []).some(v => String(v.user) === String(viewerId))
    bool seen = false;
    if (snap.isMember("viewers") && snap["viewers"].isArray()) {
      for (const auto& v : snap["viewers"]) {
        std::string vu;
        if (v.isObject() && v.isMember("user")) vu = v["user"].asString();
        else if (v.isString()) vu = v.asString();
        if (vu == viewerId) { seen = true; break; }
      }
    }
    if (!seen) group.hasUnseen = true;

    Json::Value entry(Json::objectValue);
    entry["_id"]          = snap.get("_id", Json::Value::nullSingleton());
    entry["mediaType"]    = snap.get("mediaType", Json::Value::nullSingleton());
    entry["mediaUrl"]     = snap.get("mediaUrl", Json::Value::nullSingleton());
    entry["thumbnailUrl"] = snap.get("thumbnailUrl", Json::Value::nullSingleton());
    entry["durationMs"]   = snap.get("durationMs", Json::Value::nullSingleton());
    entry["caption"]      = snap.get("caption", Json::Value::nullSingleton());
    entry["createdAt"]    = snap.get("createdAt", Json::Value::nullSingleton());
    entry["viewCount"]    = snap.get("viewCount", Json::Value::nullSingleton());
    entry["seen"]         = seen;
    group.snaps.append(entry);
  }

  // rings = [...byAuthor.entries()].map(...); preserve insertion order first.
  std::vector<std::pair<std::string, Ring*>> rings;
  rings.reserve(byAuthor.size());
  for (auto& kv : byAuthor) rings.emplace_back(kv.first, &kv.second);
  std::sort(rings.begin(), rings.end(),
            [](const auto& a, const auto& b) {
              return a.second->order < b.second->order;
            });

  // Order: viewer's own ring first, then unseen rings, then seen rings.
  // Replicates the JS comparator (stable on insertion order otherwise).
  std::stable_sort(rings.begin(), rings.end(),
      [&](const auto& a, const auto& b) {
        if (a.first == viewerId) return true;   // a.authorId === viewerId -> -1
        if (b.first == viewerId) return false;  // b.authorId === viewerId -> 1
        if (a.second->hasUnseen != b.second->hasUnseen)
          return a.second->hasUnseen;           // hasUnseen first
        return false;                           // keep order
      });

  Json::Value out(Json::arrayValue);
  for (const auto& rp : rings) {
    Json::Value ring(Json::objectValue);
    ring["authorId"] = rp.first;
    ring["user"]     = rp.second->user;
    ring["snaps"]    = rp.second->snaps;
    ring["hasUnseen"] = rp.second->hasUnseen;
    out.append(ring);
  }
  return out;
}

Json::Value getDirectInbox(const std::string& userId) {
  auto col = pulse::db::collection(kCollection);

  bsoncxx::builder::basic::document filter;
  filter.append(kvp("audience", "direct"));
  if (auto o = pulse::bsonjson::tryOid(userId)) {
    filter.append(kvp("recipients", *o));   // matches array membership
  } else {
    filter.append(kvp("recipients", userId));
  }
  filter.append(kvp("expiresAt", make_document(kvp("$gt", nowDate()))));

  // .sort({ createdAt: -1 }).populate('user', ...).lean()
  mongocxx::pipeline p;
  p.match(filter.extract());
  p.sort(make_document(kvp("createdAt", -1)));
  p.lookup(make_document(
      kvp("from", "users"),
      kvp("localField", "user"),
      kvp("foreignField", "_id"),
      kvp("pipeline", make_array(
          make_document(kvp("$project", userProjection())))),
      kvp("as", "userDoc")));
  p.add_fields(make_document(
      kvp("user", make_document(kvp("$ifNull", make_array(
          make_document(kvp("$arrayElemAt", make_array("$userDoc", 0))),
          "$user"))))));
  p.project(make_document(kvp("userDoc", 0)));

  auto cursor = col.aggregate(p);
  Json::Value out(Json::arrayValue);
  for (auto&& view : cursor) {
    out.append(pulse::bsonjson::toJson(view));
  }
  return out;
}

} // namespace pulse::models::snap
