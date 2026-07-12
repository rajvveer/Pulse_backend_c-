// like.hpp — C++ port of src/models/Like.js (Mongoose "Like" model).
//
// Production-grade like tracking. Replaces embedded arrays with a dedicated
// collection for O(1) lookups and atomic toggle operations. Supports likes on
// posts, reels, and comments.
//
// Schema (ground truth — src/models/Like.js):
//   user            ObjectId  ref User                required, indexed
//   targetType      String    enum[post,reel,comment] required
//   targetId        ObjectId  refPath targetTypeModel required
//   targetTypeModel String    enum[Post,Reel,Comment,ReelComment] required
//   timestamps: true  -> createdAt / updatedAt (Date)
//
// All field names and the collection name match the JS schema EXACTLY.
#pragma once
#include <string>
#include <vector>
#include <map>
#include <set>
#include <json/json.h>

namespace pulse::models::like {

// Mongoose model('Like') -> lowercased + pluralized collection name.
inline constexpr const char* kCollection = "likes";

// targetType enum values.
inline constexpr const char* kTargetTypePost    = "post";
inline constexpr const char* kTargetTypeReel    = "reel";
inline constexpr const char* kTargetTypeComment = "comment";

// targetTypeModel enum values.
inline constexpr const char* kModelPost        = "Post";
inline constexpr const char* kModelReel        = "Reel";
inline constexpr const char* kModelComment     = "Comment";
inline constexpr const char* kModelReelComment = "ReelComment";

// =========================================================
//  INDEXES
// =========================================================
// Creates EVERY index the schema declares, with identical keys/options.
void ensureIndexes();

// =========================================================
//  DEFAULTS / OUTPUT SHAPING
// =========================================================
// Fill in schema defaults + derived fields on insert (timestamps, and the
// targetTypeModel derived from targetType when omitted). Validates enums.
Json::Value applyDefaults(Json::Value doc);

// Strip internal/non-API fields for output (matches a toJSON transform: drops
// __v). The Like schema declares no select:false fields, but __v is removed so
// output mirrors how the lean API documents are shaped elsewhere.
Json::Value sanitizeForOutput(Json::Value doc);

// =========================================================
//  STATIC METHODS — atomic operations (ported 1:1)
// =========================================================

// Result of toggleLike: { liked: boolean, likeCount: number }.
struct ToggleResult {
  bool liked = false;
  long long likeCount = 0;
};

// Toggle like — atomic like/unlike. Delete-first toggle: deleteOne both checks
// and removes; on the "like" path a duplicate-key (E11000) is swallowed as
// "already liked". Returns { liked, likeCount }.
ToggleResult toggleLike(const std::string& userId,
                        const std::string& targetType,
                        const std::string& targetId);

// O(1) "did user X like item Y?" -> countDocuments > 0.
bool isLikedBy(const std::string& userId,
               const std::string& targetType,
               const std::string& targetId);

// Bulk check likes for feed rendering. Returns the set of liked targetIds
// (as 24-char hex strings).
std::set<std::string> getLikedIds(const std::string& userId,
                                  const std::string& targetType,
                                  const std::vector<std::string>& targetIds);

// Like count for an item -> countDocuments({ targetType, targetId }).
long long getLikeCount(const std::string& targetType,
                       const std::string& targetId);

// Like velocity — likes per hour in the trailing window.
double getLikeVelocity(const std::string& targetType,
                       const std::string& targetId,
                       double hoursWindow = 1.0);

// Batch like velocities (one aggregation for a whole candidate set):
// targetId(hex string) -> likes per hour. Every input id is present (0 default).
std::map<std::string, double> getBatchLikeVelocities(
    const std::string& targetType,
    const std::vector<std::string>& targetIds,
    double hoursWindow = 1.0);

// Batch like counts (one aggregation): targetId(hex string) -> count.
// Every input id is present (0 default).
std::map<std::string, long long> getBatchLikeCounts(
    const std::string& targetType,
    const std::vector<std::string>& targetIds);

} // namespace pulse::models::like
