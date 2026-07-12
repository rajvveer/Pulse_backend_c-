// bookmark.hpp — C++ port of src/models/Bookmark.js (Mongoose model "Bookmark").
//
// Schema (ground truth):
//   user:     ObjectId, ref "User", required, index: true
//   itemId:   ObjectId, required
//   itemType: String, enum ["post", "reel"], required
//   options:  { timestamps: true }  -> createdAt / updatedAt
//
// Indexes:
//   { user: 1 }                                  (from `index: true` on user)
//   { user: 1, itemId: 1, itemType: 1 } unique   (one bookmark per user/item)
//   { user: 1, itemType: 1, createdAt: -1 }
//
// The Mongoose model name "Bookmark" maps to the MongoDB collection "bookmarks"
// (Mongoose lowercases + pluralizes the model name).
#pragma once
#include <json/json.h>

namespace pulse::models::bookmark {

// MongoDB collection name (Mongoose: lowercased + pluralized model name).
inline constexpr const char* kCollection = "bookmarks";

// Allowed values for the itemType enum.
inline constexpr const char* kItemTypePost = "post";
inline constexpr const char* kItemTypeReel = "reel";

// Create EVERY index the schema declares (same keys + options) on `bookmarks`.
void ensureIndexes();

// Fill in schema-relevant fields on insert. The Bookmark schema declares no
// field-level defaults, but `timestamps: true` adds createdAt/updatedAt — these
// are applied here when absent so inserts match Mongoose behaviour.
Json::Value applyDefaults(Json::Value doc);

// Validate the itemType enum value (["post","reel"]). Returns true if valid.
bool isValidItemType(const std::string& itemType);

// Strip internal/select:false fields from a document before returning it to the
// API layer, matching the default Mongoose toJSON behaviour (drops __v). The
// Bookmark schema marks no field `select:false` and defines no toJSON transform,
// so only the Mongoose-internal version key is removed.
Json::Value sanitizeForOutput(Json::Value doc);

} // namespace pulse::models::bookmark
