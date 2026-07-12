// pulsescore.hpp — C++ port of src/models/PulseScore.js (Mongoose model).
//
// PulseScore — Gamified Social Reputation System. A dynamic score (0-1000)
// visible on profiles, calculated from engagement quality, consistency, and
// community contribution. Powers leaderboards, tier badges, and social proof.
//
// Schema (ground truth — src/models/PulseScore.js):
//   user           ObjectId ref User, required, unique, index
//   score          Number  default 0   (min 0, max 1000)
//   tier           String  enum[newcomer,rising,established,influencer,icon]
//                          default 'newcomer'
//   components     { engagement, consistency, community, reach, creativity }
//                          each Number default 0
//   metrics        { totalPosts, totalLikesGiven, totalLikesReceived,
//                    totalCommentsGiven, totalCommentsReceived, totalFollowers,
//                    totalFollowing, totalShares, totalViews, uniqueVibes,
//                    mediaPostsCount, daysActive, currentStreak, longestStreak }
//                          each Number default 0,
//                    lastActiveDate String default ''
//   history        [{ date Date default now, score Number, tier String,
//                     delta Number }]
//   achievements   [{ id String, name String, description String, emoji String,
//                     unlockedAt Date default now }]
//   lastComputedAt Date default now
//   { timestamps: true } -> createdAt / updatedAt
//
// Indexes the schema declares:
//   user: { index: true, unique: true }    (field-level)
//   { score: -1 }                          (leaderboards)
//   { tier: 1 }
//
// The Mongoose model name is "PulseScore"; Mongoose pluralizes/lowercases it to
// the collection "pulsescores". Field names and collection name match the JS
// schema EXACTLY.
#pragma once
#include <string>
#include <vector>
#include <optional>
#include <json/json.h>

namespace pulse::models::pulsescore {

// Mongoose model('PulseScore') -> lowercased + pluralized collection name.
inline constexpr const char* kCollection = "pulsescores";

// tier enum values (order matters — matches Object.entries(TIERS) iteration).
inline constexpr const char* kTierNewcomer    = "newcomer";
inline constexpr const char* kTierRising       = "rising";
inline constexpr const char* kTierEstablished  = "established";
inline constexpr const char* kTierInfluencer   = "influencer";
inline constexpr const char* kTierIcon         = "icon";

// =========================================================
//  TIER CONFIGURATION (ported from the JS TIERS table)
// =========================================================
struct TierConfig {
  const char* key;
  int min;
  int max;
  const char* emoji;
  const char* label;
  const char* color;
};

// The 5 tiers in declaration order (Object.entries(TIERS) order).
const std::vector<TierConfig>& tiers();

// Look up the config for a tier key; returns nullptr if unknown.
const TierConfig* tierConfig(const std::string& tier);

// =========================================================
//  INDEXES
// =========================================================
// Creates EVERY index the schema declares, with identical keys/options.
void ensureIndexes();

// =========================================================
//  DEFAULTS / OUTPUT SHAPING
// =========================================================
// Fill in schema defaults + enums on insert (score, tier, all components,
// all metrics, history [], achievements [], lastComputedAt, timestamps).
Json::Value applyDefaults(Json::Value doc);

// Strip internal/non-API fields for output (matches the default Mongoose
// toJSON: drops __v). The schema declares no select:false fields.
Json::Value sanitizeForOutput(Json::Value doc);

// =========================================================
//  STATIC METHODS — query logic (ported 1:1)
// =========================================================

// getOrCreate(userId) — find the user's PulseScore, creating a fresh defaulted
// document if none exists. Returns the (sanitized) document JSON.
Json::Value getOrCreate(const std::string& userId);

// getLeaderboard(limit=50) — top users by score:
//   find({ score: { $gt: 0 } }).sort({ score: -1 }).limit(limit)
//     .populate('user', 'username profile.displayName profile.avatar isVerified')
//     .lean()
// Returns a JSON array of lean docs with `user` populated.
Json::Value getLeaderboard(long long limit = 50);

// getUserRank(userId) — { rank, total, percentile } or null when the user has
// no PulseScore document.
//   rank  = countDocuments({ score: { $gt: userScore.score } }) + 1
//   total = countDocuments({ score: { $gt: 0 } })
//   percentile = Math.round(((total - rank) / total) * 100)
std::optional<Json::Value> getUserRank(const std::string& userId);

// =========================================================
//  INSTANCE METHODS — ported as free functions over the doc
// =========================================================

// recordAction(doc, action, value=1) — update raw metrics for the action,
// advance the activity streak, then recalculate the full score (recalculate
// updates components, tier, achievements, history, lastComputedAt). Returns the
// mutated document JSON. Mirrors pulseScoreSchema.methods.recordAction.
Json::Value recordAction(Json::Value doc, const std::string& action,
                         double value = 1.0);

// recalculate(doc) — recompute all 5 components, total score, tier,
// achievements, and append today's history entry. Mirrors
// pulseScoreSchema.methods._recalculate.
Json::Value recalculate(Json::Value doc);

// updateTier(doc) — set doc.tier from doc.score using the TIERS table. Mirrors
// pulseScoreSchema.methods._updateTier.
Json::Value updateTier(Json::Value doc);

// checkAchievements(doc) — award any newly-earned achievements (idempotent;
// only adds ones whose id is not already present). Mirrors
// pulseScoreSchema.methods._checkAchievements.
Json::Value checkAchievements(Json::Value doc);

// getDisplayData(doc) — public display payload:
//   { score, tier, tierLabel, tierEmoji, tierColor, components, streak,
//     achievements (count), nextTierAt, progressToNext }
// Mirrors pulseScoreSchema.methods.getDisplayData.
Json::Value getDisplayData(const Json::Value& doc);

} // namespace pulse::models::pulsescore
