// whisper.hpp — C++ port of src/models/Whisper.js (Mongoose "Whisper" model).
//
// Whisper — anonymous, location-based, ephemeral micro-posts. A whisper carries
// short text (<=280 chars), a GeoJSON Point location, an upvote/downvote score,
// nested anonymous replies, and an `author` that is stored but NEVER exposed
// (schema `select: false`). Each whisper auto-expires ~24h after creation via a
// TTL index on `expiresAt`.
//
// The JS schema declared:
//   content   : String, required, maxlength 280, trim
//   location  : { type: 'Point' (enum/default), coordinates: [Number] required } // [lng, lat]
//   city      : String
//   region    : String
//   author    : ObjectId ref User, required, select:false
//   upvotes   : Number default 0
//   downvotes : Number default 0
//   score     : Number default 0            // upvotes - downvotes
//   voters    : [{ user: ObjectId select:false, vote: enum['up','down'] }]
//   replies   : [{ content: String maxlength 200, author: ObjectId select:false,
//                  upvotes: Number default 0, createdAt: Date default now }]
//   reports   : Number default 0
//   isHidden  : Boolean default false
//   expiresAt : Date default now+24h, index { expires: 0 }   // TTL
//   { timestamps: true }
//   index { location: '2dsphere' }
//   index { score: -1, createdAt: -1 }
//
// Statics:  getNearby(lng, lat, radiusKm=5, limit=50), vote(whisperId, userId, voteType)
// Methods:  addReply(content, authorId)
//
// Mongoose pluralizes/lowercases the model name "Whisper" -> collection "whispers".
#pragma once
#include <string>
#include <optional>
#include <json/json.h>

namespace pulse::models::whisper {

// Mongoose default pluralization of model "Whisper" -> collection "whispers".
inline constexpr const char* kCollection = "whispers";

// Auto-expire window: new Date(Date.now() + 24 * 60 * 60 * 1000).
inline constexpr long long kExpiryTtlMs = 24LL * 60 * 60 * 1000;

// Create EVERY index the schema declares (idempotent — safe to call on boot):
//   - 2dsphere on `location`
//   - compound { score: -1, createdAt: -1 }
//   - TTL on `expiresAt` (expireAfterSeconds: 0)
void ensureIndexes();

// ===== Defaults / serialization (parity with Mongoose insert + toJSON) =====

// Fill schema defaults + enums on insert (location.type='Point', upvotes/
// downvotes/score/reports = 0, isHidden=false, voters/replies = [], reply
// defaults, expiresAt = now+24h, timestamps). Does NOT overwrite fields already
// present. Returns the augmented document.
Json::Value applyDefaults(Json::Value doc);

// defaultExpiry() — new Date(Date.now() + 24h) as an ISO-8601 UTC string.
std::string defaultExpiry();

// Shape a stored/fetched document for API output. Whispers are anonymous: this
// strips the select:false / sensitive fields the schema hides — `author`,
// `voters.user`, `replies.author` — plus the Mongoose version key __v.
Json::Value sanitizeForOutput(Json::Value doc);

// ===== Statics (query logic) ported as free functions =====

// getNearby(lng, lat, radiusKm=5, limit=50)
//   find({ location: { $near: { $geometry: Point[lng,lat],
//                               $maxDistance: radiusKm*1000 } },
//          isHidden: false })
//     .sort({ score: -1, createdAt: -1 })
//     .limit(limit)
//     .select('-author -voters.user -replies.author')
// Returns a JSON array of whisper docs with the anonymous fields projected out.
Json::Value getNearby(double lng, double lat, double radiusKm = 5, int limit = 50);

// vote(whisperId, userId, voteType) — toggle/switch a user's vote.
//   findById(whisperId).select('+voters.user'); throws if not found.
//   - existing same vote  -> remove it (toggle off), decrement that tally
//   - existing other vote -> switch it, dec old tally, inc new tally
//   - no existing vote    -> push {user,vote}, inc that tally
//   score = upvotes - downvotes; save.
// Returns { upvotes, downvotes, score }. std::nullopt when the whisper is absent
// (the JS threw new Error('Whisper not found')).
struct VoteResult {
  long long upvotes = 0;
  long long downvotes = 0;
  long long score = 0;
};
std::optional<VoteResult> vote(const std::string& whisperId,
                               const std::string& userId,
                               const std::string& voteType);

// ===== Instance methods (operate on a single document) =====

// addReply(content, authorId) — push { content, author } onto replies (with the
// reply-level defaults upvotes:0, createdAt:now) and save. Returns the newly
// appended reply as JSON, or std::nullopt if the whisper does not exist.
std::optional<Json::Value> addReply(const std::string& whisperId,
                                    const std::string& content,
                                    const std::string& authorId);

} // namespace pulse::models::whisper
