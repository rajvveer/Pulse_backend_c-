// snap.hpp — C++ port of src/models/Snap.js (Mongoose "Snap" model).
//
// Snap — ephemeral media that disappears. Two audiences, one model:
//   - audience: "story"  -> story rail; visible to followers 24h, then TTL-expires.
//   - audience: "direct" -> sent to specific recipients (disappearing snap).
//
// Mirrors the JS schema field names, defaults, enums, indexes and statics 1:1.
// The Mongoose model name is "Snap"; Mongoose pluralizes/lowercases it to the
// collection "snaps".
#pragma once
#include <string>
#include <vector>
#include <optional>
#include <json/json.h>

namespace pulse::models::snap {

// Collection name (Mongoose default pluralization of model "Snap").
inline constexpr const char* kCollection = "snaps";

// Story/direct TTL: auto-deletes ~24h after creation (DEFAULT_TTL_MS in JS).
inline constexpr long long kDefaultTtlMs = 24LL * 60 * 60 * 1000;

// Create EVERY index the schema declares (field-level user index, TTL on
// expiresAt, and the two compound rail/inbox indexes).
void ensureIndexes();

// ===== Defaults / serialization =====

// Fill in schema defaults + enums on insert (audience, mediaType, durationMs,
// viewCount, reactions, viewers, recipients, timestamps). Does NOT set expiresAt
// when absent beyond leaving it for the caller — but applies defaultExpiry() if
// missing, matching how controllers always set an expiry.
Json::Value applyDefaults(Json::Value doc);

// Strip internal/sensitive fields for API output (matches a toJSON transform).
// This schema has no select:false or sensitive fields, so this only removes the
// version key "__v" (and "id" duplication is left intact, like Mongoose).
Json::Value sanitizeForOutput(Json::Value doc);

// ===== Statics (query logic) =====

// defaultExpiry() — new Date(Date.now() + DEFAULT_TTL_MS) as an ISO-8601 string.
std::string defaultExpiry();

// getStoryRail(viewerId, followingIds) — the viewer's own active story + active
// stories from people they follow, grouped by author into "rings". Returns a
// JSON array of ring objects: { authorId, user, snaps:[...], hasUnseen }.
Json::Value getStoryRail(const std::string& viewerId,
                         const std::vector<std::string>& followingIds = {});

// getDirectInbox(userId) — direct snaps addressed to a user, still unexpired,
// newest first. Returns a JSON array of lean snap docs.
Json::Value getDirectInbox(const std::string& userId);

} // namespace pulse::models::snap
