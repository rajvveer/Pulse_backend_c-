// socialdna.hpp — C++ port of src/models/SocialDNA.js (Mongoose "SocialDNA" model).
//
// "Your Unique Content Personality Fingerprint": tracks a per-user vibe
// breakdown (five strands — chill/hype/sad/funny/creative — as percentages that
// sum to 100), raw signal counters used to derive those percentages, weekly
// evolution snapshots, DNA "twins" (similar users), latest insights, and viral
// share-card stats.
//
// Collection: "socialdnas" (Mongoose pluralizes model "SocialDNA").
//
// This namespace exposes the collection name, an ensureIndexes() that recreates
// every index the schema declared, applyDefaults()/sanitizeForOutput() mirroring
// the schema defaults + serialization, the statics that carried query logic
// (getOrCreate, recordSignal), and the instance methods that carried real logic
// (_recalcStrands, takeSnapshot, _generateInsights, matchWith, getShareCardData)
// ported as free functions operating on Json::Value documents / fields / an oid.
#pragma once
#include <string>
#include <vector>
#include <optional>
#include <json/json.h>
#include <bsoncxx/oid.hpp>

namespace pulse::models::socialdna {

// Mongoose pluralizes the model name "SocialDNA" -> collection "socialdnas".
inline constexpr const char* kCollection = "socialdnas";

// The five DNA strands / vibe enum values (used by enums and percentage math).
inline constexpr const char* kVibes[] = {"chill", "hype", "sad", "funny", "creative"};

// Recreate every index declared by socialDNASchema:
//   field `user`        -> unique index { user: 1 }
//   socialDNASchema.index({ dominantVibe: 1 })
//   socialDNASchema.index({ 'twins.user': 1 })
//   socialDNASchema.index({ totalSignals: 1 })
void ensureIndexes();

// --- Defaults / serialization -------------------------------------------------

// Fill in schema defaults + enum-bearing fields on insert, matching Mongoose's
// behavior when a SocialDNA document is created:
//   strands.{chill,hype,sad,funny,creative} default 20
//   dominantVibe default 'chill'
//   rawSignals.{...} default 0, totalSignals default 0
//   snapshots [], twins [], latestInsights []
//   streak 0, totalWeeksTracked 0, cardShareCount 0
//   lastComputedAt = now (Date.now), timestamps createdAt/updatedAt = now, __v 0
// Does NOT supply the required `user` field — the caller must provide it.
Json::Value applyDefaults(Json::Value doc);

// Shape a stored/fetched document for API output. The schema declares no custom
// toJSON transform and no select:false / sensitive fields, so this only removes
// the Mongoose internal version key (__v), matching default Mongoose JSON.
Json::Value sanitizeForOutput(Json::Value doc);

// --- Statics (ported query logic) --------------------------------------------

// socialDNASchema.statics.getOrCreate(userId):
//   let dna = findOne({ user: userId }); if (!dna) { dna = new this({user}); save() }
// Returns the (possibly newly created) DNA document as Json::Value.
Json::Value getOrCreate(const std::string& userId);

// socialDNASchema.statics.recordSignal(userId, vibe, weight = 1):
//   validates vibe; getOrCreate; rawSignals[vibe]+=weight; totalSignals+=weight;
//   _recalcStrands(); lastComputedAt = now; save().
// Returns the updated document, or std::nullopt when `vibe` is not a valid vibe.
std::optional<Json::Value> recordSignal(const std::string& userId,
                                        const std::string& vibe,
                                        int weight = 1);

// --- Instance methods (ported as free functions over a Json document) ---------

// socialDNASchema.methods._recalcStrands(): recompute strand percentages from
// rawSignals (rounded to sum to 100, surplus added to the dominant strand) and
// update dominantVibe. Mutates `doc` in place and returns it.
Json::Value& recalcStrands(Json::Value& doc);

// socialDNASchema.methods.takeSnapshot(): append a weekly snapshot (weekStart =
// now-7d, weekEnd = now, current strands/dominantVibe, generated insights,
// totalInteractions = totalSignals), trim snapshots to the last 52, and
// increment totalWeeksTracked + streak. Mutates `doc` and returns the snapshot
// that was appended.
Json::Value takeSnapshot(Json::Value& doc);

// socialDNASchema.methods._generateInsights(): build the weekly insight array by
// comparing the current strands/dominantVibe against the most recent snapshot
// (welcome / spike / shift / dominant-change / milestone / stable). Returns the
// insights array (does not mutate `doc`).
Json::Value generateInsights(const Json::Value& doc);

// socialDNASchema.methods.matchWith(otherDNA): cosine similarity of the two
// strand vectors, rounded to an integer percentage (0 when either magnitude is
// zero).
int matchWith(const Json::Value& doc, const Json::Value& otherDNA);

// socialDNASchema.methods.getShareCardData(): strands sorted descending by
// percentage with per-vibe emoji/color, plus dominant vibe emoji/color, streak,
// and weeksTracked. Returns the share-card payload.
Json::Value getShareCardData(const Json::Value& doc);

// --- Weekly batch job (ports DNAMatchAlgo.runWeeklyComputation) ---------------
// Iterates every DNA doc with totalSignals >= 1 using an _id keyset cursor
// (batches of WEEKLY_BATCH_SIZE, default 500, sorted by _id asc), calls
// takeSnapshot() on each, sets latestInsights to that snapshot's insights, and
// persists. Returns the number of users processed. Invoked by the
// weekly-dna-computation background job (src/jobs/scheduler.cc).
long long runWeeklyComputation();

} // namespace pulse::models::socialdna
