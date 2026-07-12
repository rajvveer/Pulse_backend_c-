// alterego.hpp — C++ port of src/models/AlterEgo.js (Mongoose AlterEgo model).
//
// An AlterEgo is a per-user AI persona (one document per user, enforced by the
// unique index on `user`). It stores persona settings, training data, learned
// response patterns, conversation history, settings, stats, a capped activity
// log, and guess-who game stats.
//
// Collection: "alteregos" (Mongoose pluralizes + lowercases "AlterEgo").
//
// Schema (alteregos collection):
//   user                : ObjectId ref User, required, unique
//   name                : String, required, maxlength 20, trim
//   personality          : String enum ['friendly','funny','professional',
//                                        'mysterious','chill'], default 'friendly'
//   training             : { howAreYou:String, favoriteTopics:[String],
//                            humorStyle:String, complimentResponse:String,
//                            hotTakes:[String], phrases:[String], emojis:[String] }
//   trainingLevel        : Number, default 0, min 0, max 5
//   responsePatterns     : [{ trigger:String, response:String, frequency:Number }]
//   vocabulary           : { commonWords:[String], slang:[String],
//                            greetings:[String], farewells:[String] }
//   conversations        : [{ withUser:ObjectId ref User,
//                             messages:[{ role:String enum ['user','ego'],
//                                         content:String,
//                                         timestamp:Date default Date.now }],
//                             lastActive:Date }]
//   isActive             : Boolean, default false
//   autoReplyDM          : Boolean, default true
//   autoReplyComments    : Boolean, default false
//   activeHours          : { start:Number default 0, end:Number default 24 }
//   totalReplies         : Number, default 0
//   satisfactionScore    : Number, default 0
//   lastActive           : Date
//   activityLog          : [{ action:String enum ['dm_reply','comment_reply',
//                                                  'guess_game'],
//                             targetUser:ObjectId ref User,
//                             originalMessage:String, egoResponse:String,
//                             wasRevealed:Boolean default false,
//                             guessedCorrectly:Boolean default null,
//                             timestamp:Date default Date.now }]
//   guessWhoStats        : { totalGames:Number default 0,
//                            correctGuesses:Number default 0,
//                            fooledCount:Number default 0 }
//   (timestamps: true)   : createdAt / updatedAt
//
// This namespace exposes the collection name, an ensureIndexes() recreating every
// declared index, applyDefaults()/sanitizeForOutput() mirroring schema defaults
// and the default Mongoose res.json() serialization, and the statics/instance
// methods that carried real query/mutation logic ported as free functions.
//
// NOTE: generateResponse() / buildSystemPrompt() in the JS model delegate to the
// external AI service (alterEgoAIService) and contain no DB query logic beyond a
// $set/$push of the same fields handled by the helpers below; the prompt-building
// string assembly is part of the AI service layer, not the data model, and is not
// ported here.
#pragma once
#include <string>
#include <optional>
#include <json/json.h>
#include <bsoncxx/oid.hpp>

namespace pulse::models::alterego {

// Mongoose pluralizes + lowercases the model name "AlterEgo".
inline constexpr const char* kCollection = "alteregos";

// Allowed enum values (Mongoose enum validators).
inline constexpr const char* kPersonalities[] = {"friendly", "funny",
                                                 "professional", "mysterious",
                                                 "chill"};
inline constexpr const char* kMessageRoles[]  = {"user", "ego"};
inline constexpr const char* kActivityActions[] = {"dm_reply", "comment_reply",
                                                   "guess_game"};

// Default persona name applied by the getOrCreate static.
inline constexpr const char* kDefaultName        = "My Alter Ego";
inline constexpr const char* kDefaultPersonality = "friendly";

// Caps the JS model enforces on the embedded arrays.
inline constexpr int kMaxActivityLog     = 100;  // activityLog capped at 100
inline constexpr int kMaxResponsePatterns = 100; // responsePatterns top 100

// --- Indexes ------------------------------------------------------------------

// Recreate every index declared by alterEgoSchema:
//   alterEgoSchema.index({ user: 1 })       // plus the field-level unique:true
//   alterEgoSchema.index({ isActive: 1 })
void ensureIndexes();

// --- Defaults / serialization -------------------------------------------------

// Fill in schema defaults + enum-bearing fields on insert (personality,
// trainingLevel, isActive, autoReplyDM, autoReplyComments, activeHours,
// totalReplies, satisfactionScore, guessWhoStats, and the empty embedded arrays
// / sub-document containers, plus timestamps and __v). Does NOT supply required
// fields (user, name) — those must be provided by the caller.
Json::Value applyDefaults(Json::Value doc);

// Strip internal/non-serialized fields for API output. The AlterEgo schema has
// no select:false fields and no custom toJSON transform, so this only removes the
// Mongoose version key (__v), matching default Mongoose JSON behavior.
Json::Value sanitizeForOutput(Json::Value doc);

// --- Statics (ported query logic) --------------------------------------------

// alterEgoSchema.statics.getOrCreate(userId):
//   let ego = await this.findOne({ user: userId });
//   if (!ego) { ego = new this({ user, name:'My Alter Ego', trainingLevel:0 });
//               await ego.save(); }
//   return ego;
// Finds the user's alter ego, creating one (with schema defaults applied) if
// none exists. Returns the document as Json::Value.
Json::Value getOrCreate(const std::string& userId);

// --- Instance helpers (ported as free functions over an oid) ------------------

// alterEgoSchema.methods.updateTraining(trainingData):
//   Updates the valid training.* fields, recomputes trainingLevel as the count
//   of the five "level" fields (howAreYou, favoriteTopics, humorStyle,
//   complimentResponse, hotTakes) that are non-empty, then save().
// `trainingData` is a Json object of incoming training fields. Returns the
// updated document, or std::nullopt if not found.
std::optional<Json::Value> updateTraining(const bsoncxx::oid& id,
                                          const Json::Value& trainingData);

// alterEgoSchema.methods.recordGuessResult(guessedCorrectly):
//   guessWhoStats.totalGames++; if (correct) correctGuesses++ else fooledCount++;
//   save(); return this.guessWhoStats;
// Returns the updated guessWhoStats sub-document, or std::nullopt if not found.
std::optional<Json::Value> recordGuessResult(const bsoncxx::oid& id,
                                            bool guessedCorrectly);

// alterEgoSchema.methods.learnFromUser(trigger, response):
//   Finds an existing responsePatterns entry whose trigger (case-insensitively)
//   contains `trigger`; if found bump its frequency, else push a new entry with
//   frequency 1. Then sort by frequency desc and keep the top 100; save().
void learnFromUser(const bsoncxx::oid& id, const std::string& trigger,
                   const std::string& response);

// Records one generated reply, mirroring the persistence side-effects of
// alterEgoSchema.methods.generateResponse() (the AI call itself lives in the
// service layer):
//   totalReplies++; lastActive = now;
//   activityLog.unshift({ action, targetUser, originalMessage(<=200),
//                         egoResponse(<=200), timestamp:now });
//   if (activityLog.length > 100) activityLog = activityLog.slice(0,100);
//   save();
// `action` defaults to "dm_reply" when empty; `targetUserId` may be empty for
// a null targetUser. Returns false if the document was not found.
bool recordReply(const bsoncxx::oid& id, const std::string& action,
                 const std::string& targetUserId,
                 const std::string& originalMessage,
                 const std::string& egoResponse);

// Recomputes the trainingLevel value from a training sub-document, matching the
// logic in updateTraining(): count of the five level fields that are non-empty
// (string trimmed non-empty, or array non-empty). Exposed for reuse/testing.
int computeTrainingLevel(const Json::Value& training);

} // namespace pulse::models::alterego
