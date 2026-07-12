// userengagement.hpp — C++ port of src/models/UserEngagement.js (Mongoose model).
//
// Tracks user-to-user interaction signals used for personalization: "User A
// engages heavily with User B's content" powers the affinity score in feed
// ranking. Each document links a `user` (whose feed we personalize) to a
// `targetUser` (the content creator), accumulates raw engagement `signals`, and
// stores a computed `affinityScore` plus a `lastInteraction` timestamp for decay.
//
// Schema (ground truth — src/models/UserEngagement.js):
//   user           ObjectId ref User          required, indexed
//   targetUser     ObjectId ref User          required, indexed
//   signals        embedded object of counts (all Number, default 0):
//     views, likes, comments, shares,
//     totalWatchTimeSeconds, avgWatchPercentage,
//     profileVisits, dmsSent, hides, reports
//   affinityScore  Number   default 0          indexed
//   lastInteraction Date     default Date.now   indexed
//   { timestamps: true }  -> createdAt / updatedAt (Date)
//
//   index { user:1, targetUser:1 } unique  — primary lookup
//   index { user:1, affinityScore:-1 }     — top affinities for a user
//   index { lastInteraction:1 }            — cleanup stale engagements
//
// All field names and the collection name match the JS schema EXACTLY.
#pragma once
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <json/json.h>
#include <bsoncxx/oid.hpp>

namespace pulse::models::userengagement {

// Mongoose pluralizes the model name 'UserEngagement' -> "userengagements".
inline constexpr const char* kCollection = "userengagements";

// =========================================================
//  CONSTANTS — tunable weights (mirror the JS module constants)
// =========================================================
// SIGNAL_WEIGHTS — per-signal contribution to the raw affinity score.
inline constexpr double kWeightViews                 =  0.1;
inline constexpr double kWeightLikes                 =  1.0;
inline constexpr double kWeightComments              =  2.0;
inline constexpr double kWeightShares                =  3.0;
inline constexpr double kWeightTotalWatchTimeSeconds =  0.01;  // Per second
inline constexpr double kWeightProfileVisits         =  0.5;
inline constexpr double kWeightDmsSent               =  2.5;
inline constexpr double kWeightHides                 = -5.0;
inline constexpr double kWeightReports               = -10.0;

// DECAY_HALF_LIFE_DAYS — affinity halves every 2 weeks without interaction.
inline constexpr double kDecayHalfLifeDays = 14.0;

// =========================================================
//  INDEXES
// =========================================================
// Creates EVERY index the schema declares, with identical keys/options.
void ensureIndexes();

// =========================================================
//  DEFAULTS / OUTPUT SHAPING
// =========================================================
// Fill in schema defaults on insert: the full signals sub-object (all 0),
// affinityScore (0), lastInteraction (now), timestamps (createdAt/updatedAt),
// and the Mongoose version key (__v). The user/targetUser required fields must
// be supplied by the caller.
Json::Value applyDefaults(Json::Value doc);

// Strip internal/non-API fields for output (matches the default Mongoose toJSON:
// drops __v). The UserEngagement schema declares no select:false / sensitive
// fields.
Json::Value sanitizeForOutput(Json::Value doc);

// =========================================================
//  INSTANCE METHODS (ported as free functions over fields / an oid)
// =========================================================

// userEngagementSchema.methods.recalculateAffinity():
//   score = sum(signal * weight); decay = 0.5 ^ (daysSinceInteraction / halfLife);
//   affinityScore = max(0, score * decay).
// Computes the affinity score from a signals JSON object and the lastInteraction
// instant (epoch millis), using `nowMillis` for the decay clock. Pure function.
double recalculateAffinity(const Json::Value& signals,
                           long long lastInteractionMillis,
                           long long nowMillis);

// =========================================================
//  STATIC METHODS — query logic (ported 1:1)
// =========================================================

// userEngagementSchema.statics.recordSignal(userId, targetUser, signalType, value=1):
//   self-engagement (userId === targetUser) returns nullopt;
//   upsert { $inc: { signals.<type>: value }, $set: { lastInteraction: now } },
//   recalculate affinity, persist it, and return the updated document.
// Returns std::nullopt for self-engagement, otherwise the engagement document.
std::optional<Json::Value> recordSignal(const std::string& userId,
                                        const std::string& targetUserId,
                                        const std::string& signalType,
                                        double value = 1.0);

// userEngagementSchema.statics.getAffinity(userId, targetUser):
//   findOne({ user, targetUser }) -> affinityScore or 0.
double getAffinity(const std::string& userId, const std::string& targetUserId);

// userEngagementSchema.statics.getTopAffinities(userId, limit=50):
//   find({ user }).sort({ affinityScore: -1 }).limit(limit)
//     .select('targetUser affinityScore').lean()
// Returns targetUser(hex) -> affinityScore.
std::map<std::string, double> getTopAffinities(const std::string& userId,
                                               long long limit = 50);

// userEngagementSchema.statics.getBatchAffinities(userId, targetUserIds):
//   find({ user, targetUser: { $in: targetUserIds } })
//     .select('targetUser affinityScore').lean()
//   then fill 0 for any requested id not present.
// Returns targetUser(hex) -> affinityScore for EVERY requested id.
std::map<std::string, double> getBatchAffinities(
    const std::string& userId,
    const std::vector<std::string>& targetUserIds);

// userEngagementSchema.statics.applyGlobalDecay():
//   deleteMany({ lastInteraction: { $lt: now-30d }, affinityScore: { $lt: 1 } });
//   then stream the remaining docs, recalculate each affinity, and persist it.
// Returns the number of documents whose affinity was recalculated/saved.
long long applyGlobalDecay();

} // namespace pulse::models::userengagement
