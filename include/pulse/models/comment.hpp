// comment.hpp — C++ port of src/models/Comment.js (Mongoose model 'Comment').
//
// The Mongoose schema is a plain document with no custom statics or instance
// methods carrying query logic, so this model exposes:
//   - the collection name (kCollection),
//   - ensureIndexes() creating every index the schema declares,
//   - applyDefaults() filling schema defaults on insert,
//   - sanitizeForOutput() stripping Mongoose-internal fields (__v) for responses.
//
// Field names match the schema EXACTLY:
//   post, author, content, mentions[], likes[], gif{id,url,preview,width,
//   height,description}, parentComment, replies[], isEdited, editedAt,
//   isActive, createdAt, updatedAt (timestamps:true), __v.
#pragma once

#include <json/json.h>

namespace pulse::models::comment {

// Mongoose model('Comment') -> collection "comments".
inline constexpr const char* kCollection = "comments";

// Create every index declared by the schema (idempotent):
//   { post: 1 }                    (field-level index: true on `post`)
//   { post: 1, createdAt: -1 }
//   { author: 1, createdAt: -1 }
//   { parentComment: 1 }
void ensureIndexes();

// Fill in schema defaults + enums on insert. Mutates a copy and returns it.
// Defaults applied: parentComment=null, isEdited=false, isActive=true,
// mentions=[], likes=[], replies=[]. timestamps createdAt/updatedAt and __v
// are stamped too, mirroring Mongoose's insert behavior.
Json::Value applyDefaults(Json::Value doc);

// Strip Mongoose-internal/sensitive fields for API output (matches the default
// toJSON transform). The Comment schema declares no select:false fields, so this
// removes __v only.
Json::Value sanitizeForOutput(Json::Value doc);

} // namespace pulse::models::comment
