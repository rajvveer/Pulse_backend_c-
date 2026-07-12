// message.hpp — C++ port of src/models/Message.js (Mongoose "Message" model).
//
// Direct-message / conversation message document. Mongoose pluralizes the model
// name "Message" -> the "messages" collection. The schema declares a single
// compound index { conversation: 1, createdAt: -1 } that satisfies the chat
// history query find({conversation}).sort({createdAt:-1}).limit(N) from the
// index itself (avoiding the 32MB in-memory sort abort under load).
//
// This schema declares NO statics and NO instance methods, so the only ported
// logic is the index spec, default/enum application on insert, and the JSON
// output transform (drop __v). There are no select:false / sensitive fields.
#pragma once
#include <json/json.h>

namespace pulse::models::message {

// Mongo collection name (Mongoose: model("Message") -> "messages").
inline constexpr const char* kCollection = "messages";

// Allowed values for the `type` field (schema enum). Default is "text".
// ['text', 'image', 'video', 'gif', 'sticker', 'system']

// Create every index the schema declares:
//   messageSchema.index({ conversation: 1, createdAt: -1 });
void ensureIndexes();

// Fill in schema defaults + enum default on insert, matching Mongoose:
//   type      -> "text" (when missing/empty)
//   reactions -> {} (empty object/Map)
//   isDeleted -> false
//   createdAt -> now ISO-8601 (timestamps: true)
//   updatedAt -> now ISO-8601 (timestamps: true)
Json::Value applyDefaults(Json::Value doc);

// Output transform for API responses. This schema defines no toJSON transform
// and no select:false fields, but per porting conventions we strip the Mongoose
// version key `__v`. Returns a copy.
Json::Value sanitizeForOutput(Json::Value doc);

} // namespace pulse::models::message
