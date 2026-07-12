// session.hpp — C++ port of src/models/Session.js (Mongoose "Session" model).
//
// Mirrors the Mongoose schema 1:1: same collection ("sessions"), same field
// names, same indexes (incl. the TTL index on expiresAt with expireAfterSeconds
// = 0), and the same query/update logic from the schema statics and methods.
//
// Statics carrying query logic -> free functions returning Json::Value /
// std::optional / count results. Instance methods that touched only the document
// (updateActivity / deactivate / isExpired) -> free functions taking an oid or
// the relevant fields. Schema defaults + enums are applied by applyDefaults() on
// insert; select:false fields are stripped by sanitizeForOutput() to match the
// implicit toJSON output (accessToken/refreshToken/firebaseToken were select:false).
#pragma once
#include <string>
#include <optional>
#include <cstdint>
#include <json/json.h>
#include <bsoncxx/oid.hpp>

namespace pulse::models::session {

// Exact Mongoose collection name (schema option `collection: 'sessions'`).
inline constexpr const char* kCollection = "sessions";

// deviceInfo.platform enum (Mongoose `enum: ['ios','android','web','desktop']`).
bool isValidPlatform(const std::string& platform);

// Create EVERY index the schema declares (called from src/db/indexes.cc).
//   - expiresAt: TTL, expireAfterSeconds = 0   (schema `expires: 0`)
//   - { userId: 1, isActive: 1 }
//   - { deviceId: 1 }
//   - { refreshToken: 1 }
//   - { lastActivity: -1 }
void ensureIndexes();

// Fill in schema defaults + enums on insert (timestamps, deviceInfo.*,
// firebaseToken=null, userAgent='', location.coordinates=null, isActive=true,
// lastActivity=now). Does NOT fabricate required fields (userId, deviceId,
// accessToken, refreshToken, ipAddress, expiresAt, deviceInfo.platform).
Json::Value applyDefaults(Json::Value doc);

// Strip select:false / sensitive fields to match the document serialization the
// API exposes: accessToken, refreshToken, firebaseToken (all select:false) and __v.
Json::Value sanitizeForOutput(Json::Value doc);

// ---- statics (query logic) ---------------------------------------------------

// Session.findActiveSession(userId, deviceId):
//   findOne({ userId, deviceId, isActive: true, expiresAt: { $gt: now } })
// Returns the matched doc (sanitized) or std::nullopt.
std::optional<Json::Value> findActiveSession(const bsoncxx::oid& userId,
                                             const std::string& deviceId);

// Session.deactivateUserSessions(userId, excludeDeviceId = null):
//   updateMany({ userId, isActive: true[, deviceId: { $ne: excludeDeviceId }] },
//              { $set: { isActive: false } })
// Returns number of documents modified.
long long deactivateUserSessions(const bsoncxx::oid& userId,
                                 const std::optional<std::string>& excludeDeviceId = std::nullopt);

// Session.cleanupExpired():
//   deleteMany({ $or: [
//     { expiresAt: { $lt: now } },
//     { isActive: false, updatedAt: { $lt: now - 7 days } } ] })
// Returns number of documents deleted.
long long cleanupExpired();

// ---- instance helpers (document logic) --------------------------------------

// session.updateActivity(): set lastActivity = now for the given session.
// Returns number of documents modified (1 on success, 0 if not found).
long long updateActivity(const bsoncxx::oid& sessionId);

// session.deactivate(): set isActive = false for the given session.
// Returns number of documents modified (1 on success, 0 if not found).
long long deactivate(const bsoncxx::oid& sessionId);

// session.isExpired(): expiresAt < now. Takes the millisecond epoch of expiresAt.
bool isExpired(long long expiresAtMillis);

} // namespace pulse::models::session
