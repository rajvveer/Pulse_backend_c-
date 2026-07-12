// userbehavior.hpp — C++ port of src/models/UserBehavior.js (Mongoose
// "UserBehavior" model).
//
// Mongoose model name "UserBehavior" -> MongoDB collection "userbehaviors"
// (default pluralized + lowercased; the schema declares no explicit collection
// name). UserBehavior is a deep per-user behavior profile that powers feed
// ranking / addiction mechanics (content affinities, session patterns, reward
// sensitivity, recently-seen tracking).
//
// This header exposes:
//   * kCollection                  — the exact collection name ("userbehaviors").
//   * ensureIndexes()              — creates EVERY index the schema declares
//                                    (field-level unique index on `user`, plus
//                                    the explicit index({ user:1 }) and
//                                    index({ 'currentSession.lastActivityAt':1 })).
//   * the schema statics carrying query logic, ported as free functions:
//       getOrCreate / recordView / recordLike / getSeenPostIds / getPreferences.
//   * the schema instance methods that were pure document logic, ported as free
//     functions: updateAffinitiesFromPost / getMediaType.
//   * applyDefaults()              — fills the (deeply nested) schema defaults on
//                                    insert + timestamps + __v.
//   * sanitizeForOutput()          — drops the Mongoose version key (__v); the
//                                    schema declares no select:false / sensitive
//                                    fields, so nothing else is stripped.
#pragma once
#include <string>
#include <vector>
#include <set>
#include <optional>
#include <json/json.h>

namespace pulse::models::userbehavior {

// Mongoose: mongoose.model('UserBehavior', userBehaviorSchema) -> "userbehaviors".
inline constexpr const char* kCollection = "userbehaviors";

// ── Schema constants (top of UserBehavior.js) ───────────────────────────────
inline constexpr double      kAffinityDecay     = 0.95;            // unused decay factor (parity)
inline constexpr long long   kSessionTimeoutMs  = 30LL * 60 * 1000;  // 30 min = new session
inline constexpr int         kMaxRecentPosts    = 500;            // track last 500 seen posts

// ── Index management ────────────────────────────────────────────────────────
// Creates every index UserBehavior.js declares:
//   * field-level `user: { unique: true, index: true }` -> unique index { user: 1 }
//   * userBehaviorSchema.index({ user: 1 })
//   * userBehaviorSchema.index({ 'currentSession.lastActivityAt': 1 })
// Idempotent — mongocxx create_index is a no-op when the index already exists.
void ensureIndexes();

// ── Insert defaults + output sanitization ───────────────────────────────────
// applyDefaults: fills every schema default on insert — the full
// contentAffinities / sessionPatterns / engagementVelocity / rewardSensitivity
// subtrees (mediaTypes/postLengths = 0.5, dwellTimeToLikeMs = 2000, all the
// *Preference/*Sensitivity/*Influence = 0.5, the numeric counters = 0), the
// empty Map fields (topics / authorCategories -> {}), the empty arrays
// (peakHours / recentlySeenPosts), currentSession defaults, lastProfileUpdate =
// now, plus timestamps (createdAt/updatedAt) and __v.
Json::Value applyDefaults(Json::Value doc);

// sanitizeForOutput: the schema has no select:false / sensitive fields; only the
// version key (__v) is dropped from the response shape (default toJSON).
Json::Value sanitizeForOutput(Json::Value doc);

// ── Instance methods (pure document logic, ported) ──────────────────────────

// methods.getMediaType(post): inspect post.content.media[].type.
//   [] -> 'text'; contains 'video' -> 'video'; 'gif' -> 'gif'; 'image' ->
//   'image'; else 'text'. `post` is a JSON view of the post document.
std::string getMediaType(const Json::Value& post);

// methods.updateAffinitiesFromPost(post, dwellTimeMs=0, multiplier=1.0):
// mutates the behavior document `behavior` (passed by reference as JSON) in
// place — exactly mirroring the EMA/weight math in the JS: media-type affinity,
// post-length affinity (short/medium/long by content.text.length), and topic
// affinities from content.hashtags (lowercased). Also stamps lastProfileUpdate.
// A no-op when `post` is null/empty (JS: `if (!post) return;`).
void updateAffinitiesFromPost(Json::Value& behavior,
                              const Json::Value& post,
                              double dwellTimeMs = 0.0,
                              double multiplier = 1.0);

// ── Statics (ported query logic) ────────────────────────────────────────────

// statics.getOrCreate(userId):
//   findOne({ user: userId }); if missing, create({ user: userId }).
// Returns the behavior document as JSON (sanitized), creating it on first call.
Json::Value getOrCreate(const std::string& userId);

// statics.recordView(userId, post, dwellTimeMs=0):
//   getOrCreate, advance session state (new session after SESSION_TIMEOUT_MS,
//   rolling avg session duration / posts-per-session), bump postsViewed, EMA the
//   dwell time, append { postId, seenAt } to recentlySeenPosts (capped to
//   MAX_RECENT_POSTS), then updateAffinitiesFromPost(post, dwellTimeMs) and save.
// `post` is the JSON view of the post doc (must carry _id / content.*). Returns
// the updated, sanitized behavior document.
Json::Value recordView(const std::string& userId,
                       const Json::Value& post,
                       double dwellTimeMs = 0.0);

// statics.recordLike(userId, post, timeSinceViewMs=0):
//   getOrCreate, bump currentSession.likesGiven, EMA avgTimeToLikeMs, recompute
//   likeRate, then updateAffinitiesFromPost(post, 0, 2.0) and save.
// Returns the updated, sanitized behavior document.
Json::Value recordLike(const std::string& userId,
                       const Json::Value& post,
                       double timeSinceViewMs = 0.0);

// statics.getSeenPostIds(userId, withinHours=24):
//   findOne({ user: userId }).select('recentlySeenPosts'); keep entries whose
//   seenAt >= now - withinHours; return the set of postId hex strings.
//   Empty set when there is no behavior doc.
std::set<std::string> getSeenPostIds(const std::string& userId,
                                     double withinHours = 24.0);

// statics.getPreferences(userId):
//   findOne({ user: userId }). When absent, returns the default-preferences
//   object; otherwise the projection { mediaTypes, topics, noveltyPreference,
//   viralPreference, socialProofInfluence, sessionDepth } exactly as the JS
//   assembled it. Returned as a JSON object.
Json::Value getPreferences(const std::string& userId);

} // namespace pulse::models::userbehavior
