// reel.cc — implementation of the Reel model port (see reel.hpp).
//
// Ground truth: src/models/Reel.js. The schema declares no statics/instance
// methods, so this file implements only ensureIndexes(), applyDefaults(), and
// sanitizeForOutput().
#include "pulse/models/reel.hpp"

#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/exception/exception.hpp>

namespace pulse::models::reel {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;

void ensureIndexes() {
  try {
    auto col = pulse::db::collection(kCollection);

    // Field-level: `user: { index: true }`.
    col.create_index(make_document(kvp("user", 1)));

    // reelSchema.index({ user: 1, createdAt: -1 });
    col.create_index(make_document(kvp("user", 1), kvp("createdAt", -1)));

    // reelSchema.index({ createdAt: -1 });
    col.create_index(make_document(kvp("createdAt", -1)));

    // reelSchema.index({ 'stats.likes': -1, createdAt: -1 });
    col.create_index(make_document(kvp("stats.likes", -1), kvp("createdAt", -1)));

    // reelSchema.index({ 'stats.views': -1 });
    col.create_index(make_document(kvp("stats.views", -1)));

    // reelSchema.index({ hashtags: 1 });
    col.create_index(make_document(kvp("hashtags", 1)));

    pulse::log::info("Reel indexes ensured on collection '{}'", kCollection);
  } catch (const std::exception& e) {
    pulse::log::error("Reel ensureIndexes failed: {}", e.what());
    throw;
  }
}

Json::Value applyDefaults(Json::Value doc) {
  if (!doc.isObject()) doc = Json::Value(Json::objectValue);

  // Legacy `likes` array (kept for migration compatibility) defaults to [].
  if (!doc.isMember("likes")) doc["likes"] = Json::Value(Json::arrayValue);

  // commentsCount: { type: Number, default: 0 }
  if (!doc.isMember("commentsCount")) doc["commentsCount"] = 0;

  // stats subdocument — every counter defaults to 0.
  if (!doc.isMember("stats") || !doc["stats"].isObject())
    doc["stats"] = Json::Value(Json::objectValue);
  Json::Value& stats = doc["stats"];
  if (!stats.isMember("likes"))              stats["likes"] = 0;
  if (!stats.isMember("comments"))           stats["comments"] = 0;
  if (!stats.isMember("shares"))             stats["shares"] = 0;
  if (!stats.isMember("views"))              stats["views"] = 0;
  if (!stats.isMember("saves"))              stats["saves"] = 0;
  if (!stats.isMember("avgWatchPercentage")) stats["avgWatchPercentage"] = 0;

  // duration: { type: Number, default: 0 }
  if (!doc.isMember("duration")) doc["duration"] = 0;

  // hashtags: [String] — defaults to an empty array.
  if (!doc.isMember("hashtags")) doc["hashtags"] = Json::Value(Json::arrayValue);

  // music: { type: String, default: null }
  if (!doc.isMember("music")) doc["music"] = Json::Value(Json::nullValue);

  // timestamps: true — set createdAt/updatedAt on insert.
  std::string now = pulse::bsonjson::nowIso8601();
  if (!doc.isMember("createdAt")) doc["createdAt"] = now;
  if (!doc.isMember("updatedAt")) doc["updatedAt"] = now;

  return doc;
}

Json::Value sanitizeForOutput(Json::Value doc) {
  if (!doc.isObject()) return doc;

  // toJSON({ virtuals: true }) surfaces the Mongoose `id` virtual: the hex
  // string of _id. Provide it when _id is present and `id` is not already set.
  if (!doc.isMember("id") && doc.isMember("_id")) {
    const Json::Value& id = doc["_id"];
    if (id.isString()) {
      doc["id"] = id.asString();
    } else if (id.isObject() && id.isMember("$oid")) {
      doc["id"] = id["$oid"];
    }
  }

  // Strip Mongoose's internal version key from API output.
  if (doc.isMember("__v")) doc.removeMember("__v");

  return doc;
}

} // namespace pulse::models::reel
