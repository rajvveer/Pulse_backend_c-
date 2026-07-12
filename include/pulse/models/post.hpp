// post.hpp — C++ port of src/models/Post.js (Mongoose "Post" model).
//
// Mongoose model name "Post" -> MongoDB collection "posts" (default pluralized,
// lowercased; the schema declares no explicit collection name).
//
// This header exposes:
//   * kCollection                  — the exact collection name.
//   * ensureIndexes()              — creates EVERY index the schema declares.
//   * the schema's statics          — getHomeFeed / getGlobalFeed /
//                                     getTrendingPosts / getNearbyPosts, ported
//                                     as free functions that run the SAME
//                                     filter / sort / limit (populate is a
//                                     controller-layer join, not part of the
//                                     query that hits Mongo, so the projection
//                                     of authors is left to the caller).
//   * the schema's instance method  — isLikedBy, ported as a free function.
//   * applyDefaults()              — fills schema defaults + enums on insert.
//   * sanitizeForOutput()          — strips select:false fields (embedding,
//                                     embeddingVersion) + __v, matching the
//                                     toJSON transform.
#pragma once
#include <string>
#include <vector>
#include <optional>
#include <json/json.h>

namespace pulse::models::post {

// Mongoose: mongoose.model('Post', postSchema) -> collection "posts".
inline constexpr const char* kCollection = "posts";

// ── Index management ──────────────────────────────────────────────────────
// Creates every index declared in Post.js: the field-level `index: true` on
// author and vibe, the 2dsphere on location/location.coordinates, and every
// explicit postSchema.index(...) call (including the weighted text index named
// "post_text_search"). Idempotent — mongocxx create_index is a no-op when the
// index already exists with the same spec.
void ensureIndexes();

// ── Options structs (mirror the JS `options = {}` arg) ────────────────────
struct FeedOptions {
  int limit = 20;
  // ISO-8601 / parseable date string; empty == not provided (JS: undefined).
  std::string lastPostDate;
};

struct TrendingOptions {
  int limit = 20;
  int timeRangeHours = 24;  // JS: timeRange (hours)
};

struct NearbyOptions {
  int limit = 20;
};

// ── Statics (ported query logic) ──────────────────────────────────────────
// Each returns a JSON array of post documents (Json::arrayValue). The author
// `.populate('author', 'username name avatar profile isVerified')` is a join the
// controller performs after the fetch; these run the EXACT find/sort/limit the
// JS issued against Mongo.

// statics.getHomeFeed(userId, followingIds, options)
//   isActive:true, author:{$in:[...followingIds, userId]},
//   visibility:{$in:['public','followers']},
//   createdAt: lastPostDate ? {$lt:Date} : {$exists:true}
//   sort createdAt:-1, limit
Json::Value getHomeFeed(const std::string& userId,
                        const std::vector<std::string>& followingIds,
                        const FeedOptions& options = {});

// statics.getGlobalFeed(options)
//   isActive:true, visibility:'public',
//   createdAt: lastPostDate ? {$lt:Date} : {$exists:true}
//   sort createdAt:-1, limit
Json::Value getGlobalFeed(const FeedOptions& options = {});

// statics.getTrendingPosts(options)
//   isActive:true, visibility:'public', createdAt:{$gte: now - timeRange h}
//   sort {'stats.likes':-1,'stats.comments':-1}, limit
Json::Value getTrendingPosts(const TrendingOptions& options = {});

// statics.getNearbyPosts(coordinates, maxDistance, options)
//   isActive:true, visibility:'public',
//   location:{$near:{$geometry:{type:'Point',coordinates},$maxDistance}}
//   limit
Json::Value getNearbyPosts(const std::vector<double>& coordinates,
                           double maxDistance = 1000,
                           const NearbyOptions& options = {});

// ── Instance methods (ported) ─────────────────────────────────────────────
// methods.isLikedBy(userId) — this.likes.some(id => id == userId).
// `likes` is the array of like ObjectId hex strings from the post document.
bool isLikedBy(const Json::Value& likes, const std::string& userId);

// ── Insert defaults + output sanitization ─────────────────────────────────
// applyDefaults: fills every schema default + enum default on insert
// (location.type, stats.*, vibe, vibeScore.*, all boolean flags, reportCount,
// embeddingVersion) plus timestamps (createdAt/updatedAt) and __v. Also derives
// content.hashtags from content.text and stats.likes from likes, mirroring the
// pre('save') hook for the insert path.
Json::Value applyDefaults(Json::Value doc);

// sanitizeForOutput: strips select:false fields (embedding, embeddingVersion)
// and the version key (__v) — the response shape clients see (toJSON transform).
Json::Value sanitizeForOutput(Json::Value doc);

} // namespace pulse::models::post
