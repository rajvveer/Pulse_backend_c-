// follow.hpp — C++ port of src/models/Follow.js (Mongoose model).
//
// Dedicated collection for follow relationships. Each document links a
// `follower` (the user who follows) to a `following` (the user being followed),
// with Mongoose `timestamps` (createdAt / updatedAt). This replaces storing
// follower/following arrays inside the User document so follow/unfollow stays
// O(1) and follower counts use indexed countDocuments.
//
// The JS model declared:
//   follower  : ObjectId ref User, required, index
//   following : ObjectId ref User, required, index
//   { timestamps: true }
//   index { follower:1, following:1 } unique  — prevents duplicate follows
//   index { following:1, createdAt:-1 }        — "get followers of X"
//   index { follower:1, createdAt:-1 }         — "get who X follows"
#pragma once
#include <string>
#include <vector>
#include <json/json.h>

namespace pulse::models::follow {

// Mongoose pluralizes the model name 'Follow' -> collection "follows".
inline constexpr const char* kCollection = "follows";

// Create every index the schema declares (idempotent).
void ensureIndexes();

// ---- Defaults / output shaping (parity with Mongoose insert + toJSON) ----

// Fill schema defaults on insert: applies timestamps (createdAt/updatedAt) when
// absent. The Follow schema has no enums or other defaulted fields.
Json::Value applyDefaults(Json::Value doc);

// Strip internal fields from a document for API output (matches the default
// Mongoose toJSON: drops __v). The Follow schema has no select:false fields.
Json::Value sanitizeForOutput(Json::Value doc);

// ---- Static methods (query logic) ported as free functions ----

// Result of toggleFollow.
struct ToggleResult {
  bool followed = false;
  long long followerCount = 0;
  long long followingCount = 0;
};

// Toggle follow relationship between followerId and followingId.
// Delete-first toggle: an atomic deleteOne; if a doc was removed we are now
// unfollowed, otherwise we create the follow (ignoring 11000 duplicate races).
// Returns { followed, followerCount, followingCount }.
ToggleResult toggleFollow(const std::string& followerId, const std::string& followingId);

// Check if followerId follows followingId.
bool isFollowing(const std::string& followerId, const std::string& followingId);

// Follower count for a user: countDocuments({ following: userId }).
long long getFollowerCount(const std::string& userId);

// Following count for a user: countDocuments({ follower: userId }).
long long getFollowingCount(const std::string& userId);

// Follower ObjectId hex strings for a user: find({ following: userId })
// .select('follower'). Order matches the underlying find (no explicit sort).
std::vector<std::string> getFollowerIds(const std::string& userId);

// Following ObjectId hex strings for a user: find({ follower: userId })
// .select('following').
std::vector<std::string> getFollowingIds(const std::string& userId);

} // namespace pulse::models::follow
