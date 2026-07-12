// user.hpp — port of src/models/User.js (Mongoose) to the Drogon/mongocxx stack.
//
// Mirrors the User schema 1:1: same collection ("users"), same field names,
// same indexes, and the statics/instance helpers that carried real query logic.
// Sensitive / select:false fields (passwordHash, loginAttempts, lockUntil,
// twoFactorSecret, followers, following, __v) are stripped on output by
// sanitizeForOutput, matching the schema's toJSON transform.
#pragma once
#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <json/json.h>

namespace pulse::models::user {

// Collection name from `collection: 'users'` in the schema options.
inline constexpr const char* kCollection = "users";

// Account lock threshold / duration, from incrementLoginAttempts().
inline constexpr int       kMaxLoginAttempts = 5;
inline constexpr long long kLockDurationMs   = 2LL * 60 * 60 * 1000; // 2 hours

// ===== INDEXES =====
// Creates EVERY index the schema declares (keys + unique/sparse/2dsphere/text).
void ensureIndexes();

// ===== STATIC METHODS (query logic) =====

// findByAuthMethod(type, identifier) — findOne with the type-specific $or query.
std::optional<Json::Value> findByAuthMethod(const std::string& type,
                                            const std::string& identifier);

// findByCredentials(identifier, password) — findOne by email/username/phone,
// selecting +passwordHash. (Password comparison itself happens in the caller.)
std::optional<Json::Value> findByCredentials(const std::string& identifier);

// searchUsers(searchTerm, limit=20) — case-insensitive regex on username /
// profile.displayName, projected to the public profile fields.
std::vector<Json::Value> searchUsers(const std::string& searchTerm, int limit = 20);

// getTrendingUsers(limit=10) — active users sorted by followers desc, lastActive desc.
std::vector<Json::Value> getTrendingUsers(int limit = 10);

// getSuggestedUsers(userId, limit=10) — active users excluding self + everyone
// userId already follows (resolved from the Follow collection), sorted by followers.
std::vector<Json::Value> getSuggestedUsers(const std::string& userId, int limit = 10);

// Follow.getFollowingIds(userId) — the IDs userId follows (hex strings).
// Thin wrapper over pulse::models::follow::getFollowingIds, kept here because
// getSuggestedUsers depends on it (mirrors the JS Follow.getFollowingIds call).
std::vector<std::string> getFollowingIds(const std::string& userId);

// ===== INSTANCE METHODS (per-document logic) =====

// isAccountLocked() — lockUntil set and in the future.
bool isAccountLocked(long long lockUntilMs, long long nowMs);

// incrementLoginAttempts() — applies the same $inc / $set / $unset transitions
// to the document identified by oid, given its current loginAttempts/lockUntil.
void incrementLoginAttempts(const std::string& oid,
                            int currentLoginAttempts,
                            std::optional<long long> lockUntilMs);

// resetLoginAttempts() — $unset loginAttempts + lockUntil.
void resetLoginAttempts(const std::string& oid);

// updateLastActive() — set lastActive = now.
void updateLastActive(const std::string& oid);

// setOnlineStatus(isOnline) — set isOnline (and lastActive when coming online).
void setOnlineStatus(const std::string& oid, bool isOnline);

// canViewProfile(viewer) — public profiles are viewable; private only by the
// owner or a follower present in the (legacy) followers array.
bool canViewProfile(const Json::Value& userDoc, const std::string& viewerId);

// isFollowing / isFollower / isBlocked — membership in the legacy arrays.
bool isFollowing(const Json::Value& userDoc, const std::string& userId);
bool isFollower(const Json::Value& userDoc, const std::string& userId);
bool isBlocked(const Json::Value& userDoc, const std::string& userId);

// ===== DEFAULTS / OUTPUT =====

// applyDefaults(doc) — fills schema defaults + enum defaults on insert, applies
// the pre('save') displayName-from-username hook, and stamps timestamps.
Json::Value applyDefaults(Json::Value doc);

// sanitizeForOutput(doc) — strips passwordHash / loginAttempts / lockUntil /
// twoFactorSecret / followers / following / __v (toJSON transform + select:false).
Json::Value sanitizeForOutput(Json::Value doc);

} // namespace pulse::models::user
