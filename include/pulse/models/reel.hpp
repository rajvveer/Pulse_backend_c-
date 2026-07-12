// reel.hpp — C++ port of src/models/Reel.js (Mongoose "Reel" model).
//
// The Reel schema is a pure data model: it carries field defaults, timestamps,
// a `toJSON { virtuals: true }` transform (which surfaces the Mongoose `id`
// virtual), and a set of indexes used by the reel-feed ranking algorithm. It
// declares no statics and no instance methods, so this namespace exposes only
// the collection name, index creation, default application, and the JSON
// output shaping that mirrors Mongoose serialization.
//
// Collection name "reels" is Mongoose's default pluralization of model "Reel".
#pragma once
#include <json/json.h>

namespace pulse::models::reel {

// Mongoose pluralizes the model name "Reel" -> collection "reels".
inline constexpr const char* kCollection = "reels";

// Creates every index the schema declares (field-level `user` index plus the
// five compound/single-field index() calls). Idempotent — safe to call on boot.
void ensureIndexes();

// Fills in the schema defaults (and array initializers) for an insert document,
// matching Mongoose's behavior when a document is created. Does not overwrite
// fields already present in `doc`. Returns the augmented document.
Json::Value applyDefaults(Json::Value doc);

// Shapes a stored/fetched document for API output, mirroring the schema's
// toJSON({ virtuals: true }) transform: exposes the `id` virtual (hex of _id)
// and strips the Mongoose bookkeeping field __v. The Reel schema has no
// select:false / sensitive fields, so no other fields are removed.
Json::Value sanitizeForOutput(Json::Value doc);

} // namespace pulse::models::reel
