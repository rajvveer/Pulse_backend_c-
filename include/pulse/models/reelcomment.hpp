// reelcomment.hpp — C++ port of src/models/ReelComment.js (Mongoose model).
//
// Schema (ground truth):
//   reel          : ObjectId  ref 'Reel'        required
//   author        : ObjectId  ref 'User'        required
//   content       : String                      required, trim
//   parentComment : ObjectId  ref 'ReelComment' default null   (nesting/replies)
//   likes         : [ObjectId ref 'User']                      default []
//   { timestamps: true }  -> createdAt / updatedAt
//   toJSON/toObject: { virtuals: true }
//
// Virtual (NOT stored): replies = ReelComment where parentComment == this._id.
//   Populated by the controller; never persisted, so applyDefaults omits it.
//
// The schema declares NO explicit indexes (only the implicit _id index that
// Mongo creates automatically), so ensureIndexes() creates nothing extra.
#pragma once
#include <string>
#include <json/json.h>

namespace pulse::models::reelcomment {

// Mongoose pluralizes the model name 'ReelComment' -> 'reelcomments'.
inline constexpr const char* kCollection = "reelcomments";

// Creates every index the schema declares. ReelComment declares none beyond the
// implicit _id index, so this is intentionally a no-op (kept for parity with the
// per-model ensureIndexes() registration in src/db/indexes.cc).
void ensureIndexes();

// Fills in schema defaults on insert: parentComment -> null, likes -> [],
// createdAt/updatedAt -> now, __v -> 0. Required fields (reel, author, content)
// are caller-supplied and left untouched; content is trimmed if present.
Json::Value applyDefaults(Json::Value doc);

// Mirrors the toJSON transform. The schema marks no fields select:false and has
// no sensitive fields, so this only strips the Mongoose version key __v.
Json::Value sanitizeForOutput(Json::Value doc);

} // namespace pulse::models::reelcomment
