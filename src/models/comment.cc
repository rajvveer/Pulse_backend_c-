// comment.cc — implementation of the Comment model port (src/models/Comment.js).
#include "pulse/models/comment.hpp"

#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"

#include <mongocxx/collection.hpp>
#include <mongocxx/exception/exception.hpp>
#include <mongocxx/options/index.hpp>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>

namespace pulse::models::comment {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;

void ensureIndexes() {
  try {
    auto col = pulse::db::collection(kCollection);

    // Field-level `index: true` on `post`:  commentSchema.path post -> { post: 1 }
    col.create_index(make_document(kvp("post", 1)));

    // commentSchema.index({ post: 1, createdAt: -1 });
    col.create_index(make_document(kvp("post", 1), kvp("createdAt", -1)));

    // commentSchema.index({ author: 1, createdAt: -1 });
    col.create_index(make_document(kvp("author", 1), kvp("createdAt", -1)));

    // commentSchema.index({ parentComment: 1 });
    col.create_index(make_document(kvp("parentComment", 1)));

    pulse::log::info("\xE2\x9C\x85 Ensured indexes for collection '{}'", kCollection);
  } catch (const std::exception& e) {
    pulse::log::error("Failed to ensure indexes for '{}': {}", kCollection, e.what());
    throw;
  }
}

Json::Value applyDefaults(Json::Value doc) {
  // parentComment: { default: null }
  if (!doc.isMember("parentComment")) {
    doc["parentComment"] = Json::Value::nullSingleton();
  }

  // isEdited: { default: false }
  if (!doc.isMember("isEdited")) {
    doc["isEdited"] = false;
  }

  // isActive: { default: true }
  if (!doc.isMember("isActive")) {
    doc["isActive"] = true;
  }

  // Array fields default to [] in Mongoose when absent.
  if (!doc.isMember("mentions") || !doc["mentions"].isArray()) {
    doc["mentions"] = Json::Value(Json::arrayValue);
  }
  if (!doc.isMember("likes") || !doc["likes"].isArray()) {
    doc["likes"] = Json::Value(Json::arrayValue);
  }
  if (!doc.isMember("replies") || !doc["replies"].isArray()) {
    doc["replies"] = Json::Value(Json::arrayValue);
  }

  // timestamps: true -> createdAt / updatedAt stamped on insert.
  const std::string now = pulse::bsonjson::nowIso8601();
  if (!doc.isMember("createdAt")) {
    doc["createdAt"] = now;
  }
  doc["updatedAt"] = now;

  // Mongoose document version key.
  if (!doc.isMember("__v")) {
    doc["__v"] = 0;
  }

  return doc;
}

Json::Value sanitizeForOutput(Json::Value doc) {
  // The Comment schema declares no select:false fields and no sensitive fields
  // (passwordHash/loginAttempts/etc.). The default Mongoose toJSON exposes the
  // document as-is aside from the internal version key, so strip __v only.
  doc.removeMember("__v");
  return doc;
}

} // namespace pulse::models::comment
