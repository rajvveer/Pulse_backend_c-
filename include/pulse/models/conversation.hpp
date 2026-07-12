// conversation.hpp — C++ port of src/models/Conversation.js (Mongoose model).
//
// Schema (conversations collection):
//   type                : String enum ['direct','group'], default 'direct'
//   participants         : [ObjectId ref User] (required)
//   groupName            : String, trim, maxlength 100
//   groupAvatar          : String, default null
//   groupDescription     : String, maxlength 500
//   admins               : [ObjectId ref User]
//   createdBy            : ObjectId ref User
//   lastMessage          : ObjectId ref Message
//   lastMessageContent   : String, default 'Started a conversation'
//   lastMessageSender    : ObjectId ref User
//   lastMessageAt        : Date, default Date.now
//   unreadCounts         : Map<String,Number>, default {}
//   (timestamps: true)   : createdAt / updatedAt
//
// The JS schema declares no statics, no instance methods carrying query logic,
// and no toJSON transform / select:false fields. This port therefore exposes the
// collection name, the index spec, and default/enum application + an output
// sanitizer that strips the Mongoose version key (__v), matching the default
// res.json() serialization.
#pragma once
#include <string>
#include <json/json.h>

namespace pulse::models::conversation {

// Mongoose pluralizes + lowercases the model name "Conversation".
inline constexpr const char* kCollection = "conversations";

// Allowed values for the `type` discriminator field.
inline constexpr const char* kTypeDirect = "direct";
inline constexpr const char* kTypeGroup  = "group";

// Default applied to lastMessageContent on insert.
inline constexpr const char* kDefaultLastMessageContent = "Started a conversation";

// Creates EVERY index the schema declares:
//   { participants: 1 }
//   { participants: 1, lastMessageAt: -1 }
//   { lastMessageAt: -1 }
//   { type: 1 }
void ensureIndexes();

// Fills schema defaults + enum normalization on insert (mirrors Mongoose
// default casting): type, groupAvatar, lastMessageContent, lastMessageAt,
// unreadCounts, and createdAt/updatedAt from the timestamps option.
Json::Value applyDefaults(Json::Value doc);

// Strips fields not surfaced by the default Mongoose res.json() serialization
// (the version key __v). The schema declares no select:false / sensitive fields.
Json::Value sanitizeForOutput(Json::Value doc);

} // namespace pulse::models::conversation
