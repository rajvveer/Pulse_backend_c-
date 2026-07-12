// roulette.hpp — C++ port of src/models/Roulette.js (Mongoose model).
//
// Roulette: random 1-on-1 content-based matching. A user joins a queue, the
// system pairs them with another waiting user, they get a timed (3-minute) chat,
// then each side chooses Connect or Pass. Mutual Connects form a connection.
//
// Schema (roulettes collection):
//   users         : [{ user: ObjectId ref User (required),
//                       joinedAt: Date default Date.now,
//                       decision: String enum['pending','connect','pass'] default 'pending' }]
//   status        : String enum['waiting','matched','chatting','deciding',
//                                'completed','expired'] default 'waiting'
//   matchScore    : Number default 0          (0-100 compatibility)
//   matchReason   : String default ''
//   messages      : [{ sender: ObjectId ref User,
//                       text: String maxlength 500,
//                       timestamp: Date default Date.now }]
//   matchedAt     : Date
//   chatStartedAt : Date
//   chatDuration  : Number default 180        (seconds; 3 min)
//   expiresAt     : Date                       (TTL: expireAfterSeconds 0)
//   outcome       : String enum['mutual_connect','one_sided','mutual_pass',
//                                'expired'] default null
//   icebreaker    : String default ''
//   (timestamps: true) : createdAt / updatedAt
//
// The schema declares no toJSON transform and no select:false / sensitive
// fields, so sanitizeForOutput only strips the Mongoose version key (__v).
//
// Field names and the collection name match the JS schema EXACTLY.
#pragma once
#include <json/json.h>
#include <optional>
#include <string>
#include <vector>
#include <bsoncxx/oid.hpp>

namespace pulse::models::roulette {

// Mongoose pluralizes + lowercases the model name "Roulette".
inline constexpr const char* kCollection = "roulettes";

// Allowed enum values (Mongoose enum validators).
inline constexpr const char* kUserDecisions[] = {"pending", "connect", "pass"};
inline constexpr const char* kStatuses[] = {"waiting", "matched", "chatting",
                                            "deciding", "completed", "expired"};
inline constexpr const char* kOutcomes[] = {"mutual_connect", "one_sided",
                                            "mutual_pass", "expired"};

// Field defaults that other code may need to reference.
inline constexpr int kDefaultChatDuration = 180;   // seconds (3 min)
inline constexpr int kDefaultMatchScore = 0;

// The icebreaker prompt pool (matches the ICEBREAKERS array in the JS module).
const std::vector<std::string>& icebreakers();

// Creates EVERY index the schema declares:
//   { status: 1 }
//   { 'users.user': 1 }
//   { expiresAt: 1 }   with expireAfterSeconds: 0   (TTL auto-cleanup)
void ensureIndexes();

// ---- Insert defaults / output transform --------------------------------------

// Fills in schema defaults + enum-bearing fields on insert: status, matchScore,
// matchReason, chatDuration, outcome(null), icebreaker, per-element users[]
// (joinedAt/decision) and messages[] (timestamp) defaults, plus createdAt/
// updatedAt from the timestamps option and the version key __v. Returns a copy.
Json::Value applyDefaults(Json::Value doc);

// Mirrors Mongoose's default res.json() serialization: strips the internal
// version key (__v). The schema declares no select:false / sensitive fields.
Json::Value sanitizeForOutput(Json::Value doc);

// ---- Statics (ported query logic) --------------------------------------------

// rouletteSchema.statics.joinQueue(userId):
//   findOne({ 'users.user': userId,
//             status: { $in: ['waiting','matched','chatting','deciding'] } })
//   -> if found, return it; otherwise create a new 'waiting' session
//      { users:[{user:userId, joinedAt:now}], status:'waiting' } and return it.
// Returns the session document as Json::Value.
Json::Value joinQueue(const std::string& userId);

// rouletteSchema.statics.findMatch(userId, userVibes):
//   find({ status:'waiting', 'users.user': { $ne: userId },
//          'users.0.joinedAt': { $gte: now-5min } })
//     .sort({ 'users.0.joinedAt': 1 }).limit(10)
//   -> null if none; otherwise take waiting[0], push {user:userId, joinedAt:now},
//      set status='matched', matchedAt=now, a random icebreaker, and
//      expiresAt = now + 15min, save, and return the updated session.
// userVibes is accepted for parity (the JS ignores it in the current match
// heuristic — longest-waiting wins).
std::optional<Json::Value> findMatch(const std::string& userId,
                                     const Json::Value& userVibes = Json::Value(Json::objectValue));

// rouletteSchema.statics.getUserHistory(userId, limit=20):
//   find({ 'users.user': userId, status:'completed' })
//     .sort({ createdAt:-1 }).limit(limit)
//     .populate('users.user', 'username profile.displayName profile.avatar')
// Returns an array of completed sessions (newest first).
std::vector<Json::Value> getUserHistory(const std::string& userId, int limit = 20);

// ---- Instance helpers (ported as free functions over an oid) -----------------

// rouletteSchema.methods.startChat(): status='chatting'; chatStartedAt=now; save().
// Returns the updated document, or std::nullopt if not found.
std::optional<Json::Value> startChat(const bsoncxx::oid& sessionId);

// rouletteSchema.methods.addMessage(senderId, text):
//   push { sender, text: text.substring(0,500), timestamp: now }; cap at the last
//   100 messages; save(). Returns the appended message subdocument, or
//   std::nullopt if the session was not found.
std::optional<Json::Value> addMessage(const bsoncxx::oid& sessionId,
                                      const std::string& senderId,
                                      const std::string& text);

// rouletteSchema.methods.recordDecision(userId, decision):
//   set the matching users[].decision; if all users decided -> status='completed'
//   and outcome in {mutual_connect, mutual_pass, one_sided}; else status='deciding'.
//   save(). Returns { outcome, status }.
// Throws std::runtime_error("User not in this session") if userId is not a
// participant (mirrors the JS throw). Returns std::nullopt if the session is not
// found.
std::optional<Json::Value> recordDecision(const bsoncxx::oid& sessionId,
                                          const std::string& userId,
                                          const std::string& decision);

} // namespace pulse::models::roulette
