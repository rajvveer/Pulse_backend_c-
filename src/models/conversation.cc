// conversation.cc — C++ port of src/models/Conversation.js.
//
// See include/pulse/models/conversation.hpp for the schema summary. The JS model
// carries no statics / instance methods with query logic, so the porting surface
// is the collection name, the four declared indexes, and default/enum + output
// handling matching Mongoose's default casting and res.json() serialization.
#include "pulse/models/conversation.hpp"

#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/options/index.hpp>

namespace pulse::models::conversation {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;

void ensureIndexes() {
  auto col = pulse::db::collection(kCollection);

  // conversationSchema.index({ participants: 1 });
  col.create_index(make_document(kvp("participants", 1)));

  // conversationSchema.index({ participants: 1, lastMessageAt: -1 });
  // Compound index satisfies the conversation-list match + sort.
  col.create_index(make_document(kvp("participants", 1),
                                 kvp("lastMessageAt", -1)));

  // conversationSchema.index({ lastMessageAt: -1 });
  col.create_index(make_document(kvp("lastMessageAt", -1)));

  // conversationSchema.index({ type: 1 });
  col.create_index(make_document(kvp("type", 1)));

  pulse::log::info("[conversation] ensured indexes on '{}'", kCollection);
}

Json::Value applyDefaults(Json::Value doc) {
  // type: String, enum ['direct','group'], default 'direct'.
  if (!doc.isMember("type") || doc["type"].isNull() ||
      (doc["type"].isString() && doc["type"].asString().empty())) {
    doc["type"] = kTypeDirect;
  } else if (doc["type"].isString()) {
    const std::string t = doc["type"].asString();
    if (t != kTypeDirect && t != kTypeGroup) {
      // Mongoose would reject an out-of-enum value; normalize to the default to
      // keep the document insertable rather than silently storing junk.
      doc["type"] = kTypeDirect;
    }
  }

  // groupAvatar: String, default null.
  if (!doc.isMember("groupAvatar")) {
    doc["groupAvatar"] = Json::Value::nullSingleton();
  }

  // lastMessageContent: String, default 'Started a conversation'.
  if (!doc.isMember("lastMessageContent") || doc["lastMessageContent"].isNull()) {
    doc["lastMessageContent"] = kDefaultLastMessageContent;
  }

  // lastMessageAt: Date, default Date.now.
  const std::string now = pulse::bsonjson::nowIso8601();
  if (!doc.isMember("lastMessageAt") || doc["lastMessageAt"].isNull()) {
    doc["lastMessageAt"] = now;
  }

  // unreadCounts: Map<String,Number>, default {} (empty object).
  if (!doc.isMember("unreadCounts") || doc["unreadCounts"].isNull()) {
    doc["unreadCounts"] = Json::Value(Json::objectValue);
  }

  // timestamps: true -> createdAt / updatedAt on insert.
  if (!doc.isMember("createdAt") || doc["createdAt"].isNull()) {
    doc["createdAt"] = now;
  }
  doc["updatedAt"] = now;

  return doc;
}

Json::Value sanitizeForOutput(Json::Value doc) {
  // Matches the default Mongoose res.json() serialization: drop the version key.
  // The schema declares no select:false / sensitive fields to strip.
  doc.removeMember("__v");
  return doc;
}

} // namespace pulse::models::conversation
