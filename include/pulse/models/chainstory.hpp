// chainstory.hpp — C++ port of src/models/ChainStory.js (Mongoose "ChainStory").
//
// Mongoose model name "ChainStory" -> MongoDB collection "chainstories"
// (default pluralization, lowercased; the schema declares no explicit
// collection name).
//
// The schema embeds two arrays of `segmentSchema` subdocuments (`segments` and
// `pendingSegments`). Each segment carries its own defaults/enums (mediaType,
// votes, voters, isApproved, createdAt) which applyDefaults() fills in.
//
// This header exposes:
//   * kCollection         — the exact collection name.
//   * ensureIndexes()     — creates EVERY index the schema declares:
//                             { status:1, likes:-1 }, { genre:1 },
//                             { 'segments.author':1 }.
//   * the schema's static  — getActiveChains, ported as a free function that
//                            runs the SAME find / sort / skip / limit /
//                            projection (the `.populate('starterAuthor', ...)`
//                            is a controller-layer join performed after the
//                            fetch, not part of the Mongo query).
//   * the schema's instance methods — submitSegment / voteOnSegment / toggleLike,
//                            ported as free functions keyed by the chain's oid.
//   * lastSegment()       — the `lastSegment` virtual.
//   * applyDefaults()     — fills schema defaults + enums on insert (top-level
//                            and per embedded segment).
//   * sanitizeForOutput() — strips Mongoose's internal __v and surfaces `id`.
#pragma once
#include <string>
#include <vector>
#include <optional>
#include <json/json.h>
#include <bsoncxx/oid.hpp>

namespace pulse::models::chainstory {

// Mongoose: mongoose.model('ChainStory', chainStorySchema) -> "chainstories".
inline constexpr const char* kCollection = "chainstories";

// ── Index management ──────────────────────────────────────────────────────
// Creates every index declared in ChainStory.js:
//   chainStorySchema.index({ status: 1, likes: -1 });
//   chainStorySchema.index({ genre: 1 });
//   chainStorySchema.index({ 'segments.author': 1 });
// Idempotent — mongocxx create_index is a no-op when the index already exists.
void ensureIndexes();

// ── Insert defaults + output sanitization ─────────────────────────────────
// applyDefaults: fills every top-level schema default/enum on insert
// (segmentCount, contributorCount, contributors, totalVotes, likes, status,
// maxSegments, isPublic, allowAnyone, requireVotes, genre, tags) plus
// timestamps (createdAt/updatedAt) and __v. Also normalizes each embedded
// segment in `segments` / `pendingSegments` via the segment defaults
// (mediaType, votes, voters, isApproved, createdAt). Does not overwrite fields
// already present. Returns the augmented document.
Json::Value applyDefaults(Json::Value doc);

// applySegmentDefaults: fills a single embedded segment subdocument's defaults
// (mediaType='none', votes=0, voters=[], isApproved=false, createdAt=now).
// Exposed because submitSegment builds a fresh segment.
Json::Value applySegmentDefaults(Json::Value segment);

// sanitizeForOutput: mirrors Mongoose serialization — surfaces the `id` (hex of
// _id) when present and strips the version key (__v). The ChainStory schema
// declares no select:false / sensitive fields.
Json::Value sanitizeForOutput(Json::Value doc);

// ── Virtuals ──────────────────────────────────────────────────────────────
// virtual lastSegment: if segments is empty return starterContent, else the
// content of the last segment. `doc` is a full chain document (Json object).
std::string lastSegment(const Json::Value& doc);

// ── Statics (ported query logic) ──────────────────────────────────────────
struct ActiveChainsOptions {
  std::string genre;   // empty == not provided (JS: undefined)
  int limit = 20;
  int skip = 0;
};

// statics.getActiveChains(options)
//   find({ status:'active', [genre] })
//     .sort({ likes:-1, contributorCount:-1 })
//     .skip(skip).limit(limit)
//     .select('-pendingSegments -segments.voters')
// Returns a JSON array of chain documents (Json::arrayValue). The
// `.populate('starterAuthor', 'username profile.avatar')` author join is left
// to the caller (controller-layer), matching the post.hpp convention.
Json::Value getActiveChains(const ActiveChainsOptions& options = {});

// ── Instance methods (ported) ─────────────────────────────────────────────

// methods.submitSegment(content, authorId, media=null)
//   Throws std::runtime_error("This chain is no longer accepting submissions")
//   when status != 'active', and ("This chain has reached maximum segments")
//   when segmentCount >= maxSegments. Otherwise pushes a new pending segment
//   (mediaType derived from the media URL: '.mp4' -> 'video', else 'image';
//   no media -> 'none') and persists. Returns the created segment as JSON.
Json::Value submitSegment(const bsoncxx::oid& chainId,
                          const std::string& content,
                          const bsoncxx::oid& authorId,
                          const std::optional<std::string>& media = std::nullopt);

struct VoteResult {
  int votes = 0;
  bool approved = false;
};

// methods.voteOnSegment(segmentId, userId, value)
//   Re-votes on a pending segment: removes any prior vote by userId (adjusting
//   the running tally), records the new {user,value}, and if votes >= requireVotes
//   and not yet approved, approves it — moving the segment into `segments`,
//   pulling it from `pendingSegments`, bumping segmentCount, tracking the
//   contributor (and contributorCount), and flipping status to 'complete' when
//   segmentCount >= maxSegments. Always increments totalVotes. Persists.
//   Throws std::runtime_error("Segment not found") when the pending segment is
//   absent. Returns { votes, approved } for the voted segment.
VoteResult voteOnSegment(const bsoncxx::oid& chainId,
                         const bsoncxx::oid& segmentId,
                         const bsoncxx::oid& userId,
                         int value);

// methods.toggleLike(userId)
//   this.likes++; save(); return this.likes. (Simple increment; the JS does not
//   actually track per-user likes.) Returns the new like count.
long long toggleLike(const bsoncxx::oid& chainId, const bsoncxx::oid& userId);

} // namespace pulse::models::chainstory
