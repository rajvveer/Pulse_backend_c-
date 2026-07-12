// reelcomment.cc — implementation of the ReelComment model port.
// See include/pulse/models/reelcomment.hpp and src/models/ReelComment.js.
#include "pulse/models/reelcomment.hpp"

#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <mongocxx/collection.hpp>

#include <algorithm>
#include <cctype>
#include <string>

namespace pulse::models::reelcomment {

namespace {

// Mongoose `trim: true` — strip leading/trailing ASCII whitespace.
std::string trim(const std::string& s) {
  std::size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

} // namespace

void ensureIndexes() {
  // The ReelComment schema declares no explicit indexes; Mongo creates the
  // implicit _id index on its own. Reference the collection so this stays
  // consistent with sibling models and to fail loudly if the name drifts.
  (void)pulse::db::collection(kCollection);
  pulse::log::debug("reelcomment.ensureIndexes: no explicit indexes to create "
                    "(collection '{}')", kCollection);
}

Json::Value applyDefaults(Json::Value doc) {
  // content: trim (Mongoose `trim: true`).
  if (doc.isMember("content") && doc["content"].isString()) {
    doc["content"] = trim(doc["content"].asString());
  }

  // parentComment: default null (top-level comment unless a reply target given).
  if (!doc.isMember("parentComment")) {
    doc["parentComment"] = Json::Value::nullSingleton();
  }

  // likes: default [] (empty array of User ObjectIds).
  if (!doc.isMember("likes") || !doc["likes"].isArray()) {
    doc["likes"] = Json::Value(Json::arrayValue);
  }

  // timestamps: createdAt / updatedAt set to now on insert.
  const std::string now = pulse::bsonjson::nowIso8601();
  if (!doc.isMember("createdAt")) doc["createdAt"] = now;
  if (!doc.isMember("updatedAt")) doc["updatedAt"] = now;

  // Mongoose version key.
  if (!doc.isMember("__v")) doc["__v"] = 0;

  return doc;
}

Json::Value sanitizeForOutput(Json::Value doc) {
  // toJSON has no select:false or sensitive fields to strip; only drop __v.
  doc.removeMember("__v");
  return doc;
}

} // namespace pulse::models::reelcomment
