// otp.hpp — C++ port of src/models/OTP.js (Mongoose OTP model).
//
// Schema: one-time passwords used for email/SMS verification flows
// (signup, login, password_reset, 2fa, verification). Documents auto-expire
// via two TTL indexes: one on `expiresAt` (expireAfterSeconds:0 — delete at the
// stored instant) and a hard 30-minute cap on `createdAt`.
//
// Collection: "otps".
//
// This namespace exposes the collection name, an ensureIndexes() that recreates
// every index the Mongoose schema declared, the statics/instance helpers that
// carried query logic, plus applyDefaults()/sanitizeForOutput() mirroring the
// schema defaults and JSON serialization.
#pragma once
#include <string>
#include <vector>
#include <optional>
#include <json/json.h>
#include <bsoncxx/oid.hpp>

namespace pulse::models::otp {

// Mongoose: collection: 'otps'
inline constexpr const char* kCollection = "otps";

// Allowed enum values (Mongoose enum validators).
inline constexpr const char* kTypes[]    = {"email", "sms"};
inline constexpr const char* kPurposes[] = {"signup", "login", "password_reset",
                                            "2fa", "verification"};

// Recreate every index declared by otpSchema:
//   otpSchema.index({ identifier: 1, purpose: 1, verified: 1 })
//   otpSchema.index({ userId: 1 })
//   otpSchema.index({ createdAt: 1 }, { expireAfterSeconds: 1800 })
//   field expiresAt: { expires: 0 }  -> TTL index { expiresAt: 1 } expireAfterSeconds:0
void ensureIndexes();

// --- Defaults / serialization -------------------------------------------------

// Fill in schema defaults + enum-bearing fields on insert (timestamps,
// userId/verifiedAt/deliveredAt nulls, attempts/maxAttempts, verified,
// superseded, userAgent, and __v).
// Does NOT supply required fields (identifier, type, purpose, hashedCode,
// ipAddress, expiresAt) — those must be provided by the caller.
Json::Value applyDefaults(Json::Value doc);

// Strip internal/non-serialized fields for API output. The OTP schema has no
// select:false fields and no custom toJSON transform, so this only removes the
// Mongoose version key (__v), matching default Mongoose JSON behavior.
Json::Value sanitizeForOutput(Json::Value doc);

// --- Statics (ported query logic) --------------------------------------------

// otpSchema.statics.findValidOTP(identifier, purpose):
//   findOne({ identifier, purpose, verified:false, superseded:{$ne:true},
//             deliveredAt:{$type:'date'}, expiresAt:{ $gt: now } })
//     .sort({ deliveredAt: -1, createdAt: -1 })
// Returns the raw document as Json::Value, or std::nullopt if none.
std::optional<Json::Value> findValidOTP(const std::string& identifier,
                                        const std::string& purpose);

// Atomically reserves one verification attempt on the newest eligible OTP.
// The query requires verified:false, expiresAt > now and attempts < maxAttempts,
// then increments attempts before returning the document. This prevents a burst
// of concurrent guesses from all passing the attempt-limit check.
std::optional<Json::Value> reserveVerificationAttempt(
    const std::string& identifier, const std::string& purpose);

// otpSchema.statics.cleanupExpired():
//   deleteMany({ expiresAt: { $lt: now } })
// Returns the number of deleted documents.
long long cleanupExpired();

// --- Instance helpers (ported as free functions over fields / an oid) ---------

// otpSchema.methods.incrementAttempts(): this.attempts += 1; save().
// Atomically $inc attempts by 1 for the given document; returns the new
// `attempts` value, or std::nullopt if the document was not found.
std::optional<int> incrementAttempts(const bsoncxx::oid& id);

// otpSchema.methods.markAsVerified(): verified = true; verifiedAt = now; save().
// Sets verified:true and verifiedAt:now only while the document is still
// unverified and unexpired. Returns true only for the request that consumed it.
bool markAsVerified(const bsoncxx::oid& id);

// otpSchema.methods.isExpired(): return this.expiresAt < new Date();
// expiresAtIso is the stored expiresAt as an ISO-8601 string (as produced by
// bson_json toJson). nowMillis defaults to the current time.
bool isExpired(const std::string& expiresAtIso);
bool isExpired(long long expiresAtMillis, long long nowMillis);

// otpSchema.methods.isMaxAttemptsReached(): return this.attempts >= this.maxAttempts;
bool isMaxAttemptsReached(int attempts, int maxAttempts);

} // namespace pulse::models::otp
