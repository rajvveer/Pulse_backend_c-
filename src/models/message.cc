// message.cc — implementation of the Message model port (see message.hpp).
//
// Source of truth: src/models/Message.js. The schema is intentionally small:
// a single compound index, a handful of field defaults, and no statics/methods.
#include "pulse/models/message.hpp"

#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/options/index.hpp>

#include <exception>

namespace pulse::models::message {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;

void ensureIndexes() {
  try {
    auto col = pulse::db::collection(kCollection);

    // messageSchema.index({ conversation: 1, createdAt: -1 });
    // Compound index serving the chat-history query
    //   find({conversation}).sort({createdAt:-1}).limit(N)
    // from the index (equality match + sort). Not unique, not sparse, no TTL.
    col.create_index(make_document(kvp("conversation", 1), kvp("createdAt", -1)));

    pulse::log::info("[indexes] ensured indexes for collection {}", kCollection);
  } catch (const std::exception& e) {
    pulse::log::error("[indexes] failed to ensure indexes for {}: {}",
                      kCollection, e.what());
    throw;
  }
}

Json::Value applyDefaults(Json::Value doc) {
  // type: enum ['text','image','video','gif','sticker','system'], default 'text'
  if (!doc.isMember("type") || !doc["type"].isString() ||
      doc["type"].asString().empty()) {
    doc["type"] = "text";
  }

  // reactions: Map<userId, emoji>, default {} (empty object)
  if (!doc.isMember("reactions") || doc["reactions"].isNull()) {
    doc["reactions"] = Json::Value(Json::objectValue);
  }

  // isDeleted: Boolean, default false
  if (!doc.isMember("isDeleted") || !doc["isDeleted"].isBool()) {
    doc["isDeleted"] = false;
  }

  // timestamps: true -> createdAt / updatedAt set on insert.
  const std::string now = pulse::bsonjson::nowIso8601();
  if (!doc.isMember("createdAt") || !doc["createdAt"].isString() ||
      doc["createdAt"].asString().empty()) {
    doc["createdAt"] = now;
  }
  if (!doc.isMember("updatedAt") || !doc["updatedAt"].isString() ||
      doc["updatedAt"].asString().empty()) {
    doc["updatedAt"] = now;
  }

  return doc;
}

Json::Value sanitizeForOutput(Json::Value doc) {
  // No toJSON transform and no select:false / sensitive fields in this schema.
  // Per porting conventions, strip the Mongoose version key.
  doc.removeMember("__v");
  return doc;
}

} // namespace pulse::models::message
