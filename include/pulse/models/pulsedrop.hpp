// pulsedrop.hpp — C++ port of src/models/PulseDrop.js (Mongoose "PulseDrop").
//
// A PulseDrop is a time-limited, viral-triggered participation event. The JS
// schema declares field defaults/enums, Mongoose timestamps, three indexes, two
// statics that carry query logic (getActiveDrops, expireOld) plus a static that
// builds a document from a viral post (createFromViral), and two instance
// methods (join, getTimeRemaining).
//
// Mongoose model name "PulseDrop" -> MongoDB collection "pulsedrops" (default
// lowercased + pluralized; the schema declares no explicit collection name).
//
// This header exposes:
//   * kCollection                  — the exact collection name.
//   * ensureIndexes()              — creates EVERY index the schema declares.
//   * getActiveDrops / expireOld /
//     createFromViral              — the schema statics, ported as free
//                                    functions running the SAME filter / sort /
//                                    limit / update. (populate() of triggerPost
//                                    and featuredResponses is a controller-layer
//                                    join, not part of the query that hits Mongo,
//                                    so it is left to the caller.)
//   * join / getTimeRemaining      — the instance methods, ported as free
//                                    functions that take an _id / the relevant
//                                    fields.
//   * applyDefaults()              — fills schema defaults + enum defaults on
//                                    insert.
//   * sanitizeForOutput()          — strips the Mongoose version key (__v). The
//                                    schema declares no select:false / sensitive
//                                    fields and no custom toJSON transform.
#pragma once
#include <string>
#include <vector>
#include <optional>
#include <json/json.h>

namespace pulse::models::pulsedrop {

// Mongoose: mongoose.model('PulseDrop', pulseDropSchema) -> collection
// "pulsedrops".
inline constexpr const char* kCollection = "pulsedrops";

// ── Enum values (mirror the schema enum arrays) ───────────────────────────
// triggerType enum: ['viral','trending_hashtag','event','manual'], default
// 'viral'.
inline constexpr const char* kTriggerTypeViral           = "viral";
inline constexpr const char* kTriggerTypeTrendingHashtag = "trending_hashtag";
inline constexpr const char* kTriggerTypeEvent           = "event";
inline constexpr const char* kTriggerTypeManual          = "manual";

// status enum: ['active','expired','featured'], default 'active'.
inline constexpr const char* kStatusActive   = "active";
inline constexpr const char* kStatusExpired  = "expired";
inline constexpr const char* kStatusFeatured = "featured";

// ── Index management ──────────────────────────────────────────────────────
// Creates every index declared in PulseDrop.js:
//   { status: 1, expiresAt: 1 }
//   { trendingScore: -1 }
//   { hashtags: 1 }
// Idempotent — mongocxx create_index is a no-op when the index already exists
// with the same spec.
void ensureIndexes();

// ── Statics (ported query logic) ──────────────────────────────────────────

// statics.getActiveDrops(limit = 20)
//   find({ status:'active', expiresAt:{ $gt: now } })
//   .sort({ trending:-1, trendingScore:-1, participantCount:-1 })
//   .limit(limit)
// Returns a JSON array (Json::arrayValue) of drop documents. The
// .populate('triggerPost', ...) / .populate('featuredResponses', ...) joins are
// performed by the controller after this fetch.
Json::Value getActiveDrops(int limit = 20);

// statics.createFromViral(postId, data = {})
//   Loads the trigger Post, extracts #hashtags from its content, and inserts a
//   new drop with:
//     title       : data.title       || "🔥 <firstHashtag||'Trending'> Moment"
//     description : data.description || "Join the viral wave!"
//     triggerPost : postId
//     triggerType : 'viral'
//     hashtags    : extracted hashtags, lowercased
//     trending    : true
//   plus all schema defaults (applyDefaults). Returns the inserted document as
//   JSON. Returns std::nullopt when the Post is not found (JS throws 'Post not
//   found'); the caller decides how to surface that.
struct CreateFromViralData {
  std::optional<std::string> title;
  std::optional<std::string> description;
};
std::optional<Json::Value> createFromViral(const std::string& postId,
                                           const CreateFromViralData& data = {});

// statics.expireOld()
//   updateMany({ status:'active', expiresAt:{ $lte: now } },
//              { $set: { status:'expired' } })
// Returns the number of documents modified.
long long expireOld();

// ── Instance methods (ported) ─────────────────────────────────────────────

// Result of join().
struct JoinResult {
  bool found = false;             // false when no drop matched the _id
  long long participantCount = 0;
  long long responseCount = 0;
  long long trendingScore = 0;
};

// methods.join(userId, responsePostId = null)
//   If userId already participates: when a responsePostId is given, set that
//   participant's response and responseCount++. Otherwise push a new participant
//   { user, response, joinedAt: now }, participantCount++, and (if responsePostId
//   given) responseCount++. Then trendingScore = participantCount*2 +
//   responseCount*5, and save. `responsePostId` empty == JS null (no response).
JoinResult join(const std::string& dropId,
                const std::string& userId,
                const std::string& responsePostId = "");

// methods.getTimeRemaining()
//   const remaining = expiresAt - now;
//   if (remaining <= 0) return "0h 0m";
//   return `${hours}h ${mins}m`  (floor(remaining / 1h), floor(remaining%1h /1m))
// `expiresAtMillis` / `nowMillis` are epoch milliseconds (UTC).
std::string getTimeRemaining(long long expiresAtMillis, long long nowMillis);
// Convenience overload taking the ISO-8601 expiresAt string; uses the current
// wall clock for "now".
std::string getTimeRemaining(const std::string& expiresAtIso);

// ── Insert defaults + output sanitization ─────────────────────────────────
// applyDefaults: fills every schema default/enum default on insert —
//   triggerType='viral', participantCount=0, responseCount=0, startsAt=now,
//   expiresAt=now+24h, status='active', totalEngagement=0, trending=false,
//   trendingScore=0, participants=[], hashtags=[], featuredResponses=[] — plus
//   timestamps (createdAt/updatedAt) and the version key (__v).
Json::Value applyDefaults(Json::Value doc);

// sanitizeForOutput: drops the Mongoose version key (__v). The schema has no
// select:false / sensitive fields and no custom toJSON transform.
Json::Value sanitizeForOutput(Json::Value doc);

} // namespace pulse::models::pulsedrop
