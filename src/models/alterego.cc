// alterego.cc — C++ port of src/models/AlterEgo.js. See pulse/models/alterego.hpp.
#include "pulse/models/alterego.hpp"

#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/oid.hpp>

#include <mongocxx/collection.hpp>
#include <mongocxx/options/index.hpp>
#include <mongocxx/options/find_one_and_update.hpp>
#include <mongocxx/exception/operation_exception.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <string>
#include <vector>

namespace pulse::models::alterego {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;
using bld::make_array;
using pulse::bsonjson::oid;
using pulse::bsonjson::tryOid;
using pulse::bsonjson::nowIso8601;
using pulse::bsonjson::nowMillis;

namespace {

// A bsoncxx b_date for "now", so Date fields persist as real BSON Dates (as
// Mongoose does) rather than strings.
bsoncxx::types::b_date nowDate() {
  return bsoncxx::types::b_date{std::chrono::milliseconds{nowMillis()}};
}

// Lower-cases an ASCII string (used to mirror JS String.prototype.toLowerCase
// for the substring match in learnFromUser).
std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// JS `String.prototype.trim()` test: non-empty after trimming whitespace.
bool isNonBlankString(const Json::Value& v) {
  if (!v.isString()) return false;
  const std::string s = v.asString();
  auto notSpace = [](unsigned char c) { return !std::isspace(c); };
  return std::any_of(s.begin(), s.end(), notSpace);
}

} // namespace

// -----------------------------------------------------------------------------
// Indexes
// -----------------------------------------------------------------------------
void ensureIndexes() {
  try {
    auto col = pulse::db::collection(kCollection);

    // Field-level `user: { unique: true }` -> unique index { user: 1 }.
    {
      mongocxx::options::index opts{};
      opts.unique(true);
      col.create_index(make_document(kvp("user", 1)), opts);
    }

    // alterEgoSchema.index({ user: 1 }) — declared explicitly too; redundant with
    // the unique index above but kept for 1:1 parity with the schema. Mongo
    // treats this as the same key spec, so the unique index already covers it.

    // alterEgoSchema.index({ isActive: 1 })
    col.create_index(make_document(kvp("isActive", 1)));

    pulse::log::info("[alterego] ensured indexes on '{}'", kCollection);
  } catch (const std::exception& e) {
    pulse::log::error("[alterego] ensureIndexes failed: {}", e.what());
    throw;
  }
}

// -----------------------------------------------------------------------------
// Defaults / serialization
// -----------------------------------------------------------------------------
int computeTrainingLevel(const Json::Value& training) {
  // const levelFields = ['howAreYou','favoriteTopics','humorStyle',
  //                      'complimentResponse','hotTakes'];
  // filled = levelFields.filter(f => non-empty string / non-empty array)
  static const char* kLevelFields[] = {"howAreYou", "favoriteTopics",
                                       "humorStyle", "complimentResponse",
                                       "hotTakes"};
  int filled = 0;
  for (const char* f : kLevelFields) {
    if (!training.isObject() || !training.isMember(f)) continue;
    const Json::Value& val = training[f];
    if (val.isNull()) continue;
    if (val.isArray()) {
      if (val.size() > 0) ++filled;
    } else if (val.isString()) {
      if (isNonBlankString(val)) ++filled;
    }
  }
  return filled;
}

Json::Value applyDefaults(Json::Value doc) {
  if (!doc.isObject()) doc = Json::Value(Json::objectValue);

  // personality: enum, default 'friendly'.
  if (!doc.isMember("personality") || doc["personality"].isNull() ||
      (doc["personality"].isString() && doc["personality"].asString().empty())) {
    doc["personality"] = kDefaultPersonality;
  }

  // training: sub-document with array fields (no scalar defaults in Mongoose,
  // but the container exists). Provide an empty object so callers can index in.
  if (!doc.isMember("training") || doc["training"].isNull()) {
    doc["training"] = Json::Value(Json::objectValue);
  }

  // trainingLevel: Number, default 0 (min 0, max 5).
  if (!doc.isMember("trainingLevel") || doc["trainingLevel"].isNull()) {
    doc["trainingLevel"] = 0;
  }

  // responsePatterns: [] (array default empty).
  if (!doc.isMember("responsePatterns") || doc["responsePatterns"].isNull()) {
    doc["responsePatterns"] = Json::Value(Json::arrayValue);
  }

  // vocabulary: sub-document of array fields.
  if (!doc.isMember("vocabulary") || doc["vocabulary"].isNull()) {
    doc["vocabulary"] = Json::Value(Json::objectValue);
  }

  // conversations: [] (array default empty).
  if (!doc.isMember("conversations") || doc["conversations"].isNull()) {
    doc["conversations"] = Json::Value(Json::arrayValue);
  }

  // isActive: Boolean, default false.
  if (!doc.isMember("isActive") || doc["isActive"].isNull()) {
    doc["isActive"] = false;
  }

  // autoReplyDM: Boolean, default true.
  if (!doc.isMember("autoReplyDM") || doc["autoReplyDM"].isNull()) {
    doc["autoReplyDM"] = true;
  }

  // autoReplyComments: Boolean, default false.
  if (!doc.isMember("autoReplyComments") || doc["autoReplyComments"].isNull()) {
    doc["autoReplyComments"] = false;
  }

  // activeHours: { start: default 0, end: default 24 }.
  if (!doc.isMember("activeHours") || doc["activeHours"].isNull() ||
      !doc["activeHours"].isObject()) {
    doc["activeHours"] = Json::Value(Json::objectValue);
  }
  if (!doc["activeHours"].isMember("start") || doc["activeHours"]["start"].isNull()) {
    doc["activeHours"]["start"] = 0;
  }
  if (!doc["activeHours"].isMember("end") || doc["activeHours"]["end"].isNull()) {
    doc["activeHours"]["end"] = 24;
  }

  // totalReplies: Number, default 0.
  if (!doc.isMember("totalReplies") || doc["totalReplies"].isNull()) {
    doc["totalReplies"] = 0;
  }

  // satisfactionScore: Number, default 0.
  if (!doc.isMember("satisfactionScore") || doc["satisfactionScore"].isNull()) {
    doc["satisfactionScore"] = 0;
  }

  // activityLog: [] (array default empty).
  if (!doc.isMember("activityLog") || doc["activityLog"].isNull()) {
    doc["activityLog"] = Json::Value(Json::arrayValue);
  }

  // guessWhoStats: { totalGames:0, correctGuesses:0, fooledCount:0 }.
  if (!doc.isMember("guessWhoStats") || doc["guessWhoStats"].isNull() ||
      !doc["guessWhoStats"].isObject()) {
    doc["guessWhoStats"] = Json::Value(Json::objectValue);
  }
  if (!doc["guessWhoStats"].isMember("totalGames") ||
      doc["guessWhoStats"]["totalGames"].isNull()) {
    doc["guessWhoStats"]["totalGames"] = 0;
  }
  if (!doc["guessWhoStats"].isMember("correctGuesses") ||
      doc["guessWhoStats"]["correctGuesses"].isNull()) {
    doc["guessWhoStats"]["correctGuesses"] = 0;
  }
  if (!doc["guessWhoStats"].isMember("fooledCount") ||
      doc["guessWhoStats"]["fooledCount"].isNull()) {
    doc["guessWhoStats"]["fooledCount"] = 0;
  }

  // timestamps: true -> createdAt / updatedAt.
  const std::string now = nowIso8601();
  if (!doc.isMember("createdAt") || doc["createdAt"].isNull()) {
    doc["createdAt"] = now;
  }
  if (!doc.isMember("updatedAt") || doc["updatedAt"].isNull()) {
    doc["updatedAt"] = now;
  }

  // Mongoose version key.
  if (!doc.isMember("__v")) doc["__v"] = 0;

  return doc;
}

Json::Value sanitizeForOutput(Json::Value doc) {
  if (!doc.isObject()) return doc;
  // The AlterEgo schema declares no select:false fields and no custom toJSON
  // transform; default Mongoose JSON keeps every field except the version key.
  doc.removeMember("__v");
  return doc;
}

// -----------------------------------------------------------------------------
// Build the BSON insert document for a brand-new alter ego (getOrCreate path),
// matching `new this({ user, name:'My Alter Ego', trainingLevel:0 })` + the
// schema defaults Mongoose stamps on save. `user` is stored as a real BSON
// ObjectId and timestamps as real BSON Dates.
// -----------------------------------------------------------------------------
namespace {
bsoncxx::document::value buildNewEgoDoc(const bsoncxx::oid& userOid) {
  const bsoncxx::types::b_date now = nowDate();
  return make_document(
      kvp("user", userOid),
      kvp("name", kDefaultName),
      kvp("personality", kDefaultPersonality),
      kvp("training", make_document()),
      kvp("trainingLevel", 0),
      kvp("responsePatterns", make_array()),
      kvp("vocabulary", make_document()),
      kvp("conversations", make_array()),
      kvp("isActive", false),
      kvp("autoReplyDM", true),
      kvp("autoReplyComments", false),
      kvp("activeHours", make_document(kvp("start", 0), kvp("end", 24))),
      kvp("totalReplies", 0),
      kvp("satisfactionScore", 0),
      kvp("activityLog", make_array()),
      kvp("guessWhoStats", make_document(kvp("totalGames", 0),
                                         kvp("correctGuesses", 0),
                                         kvp("fooledCount", 0))),
      kvp("createdAt", now),
      kvp("updatedAt", now),
      kvp("__v", 0));
}
} // namespace

// -----------------------------------------------------------------------------
// Statics
// -----------------------------------------------------------------------------
Json::Value getOrCreate(const std::string& userId) {
  try {
    auto col = pulse::db::collection(kCollection);
    const bsoncxx::oid userOid = oid(userId);

    // let ego = await this.findOne({ user: userId });
    auto existing = col.find_one(make_document(kvp("user", userOid)));
    if (existing) {
      return pulse::bsonjson::toJson(existing->view());
    }

    // if (!ego) { ego = new this({...}); await ego.save(); }
    auto newDoc = buildNewEgoDoc(userOid);
    try {
      col.insert_one(newDoc.view());
    } catch (const mongocxx::operation_exception& err) {
      // Duplicate from a concurrent getOrCreate (E11000 on unique `user`) —
      // fall through and re-read the winner.
      if (err.code().value() != 11000) throw;
    }

    auto created = col.find_one(make_document(kvp("user", userOid)));
    if (created) {
      return pulse::bsonjson::toJson(created->view());
    }
    // Should not happen, but return the constructed doc as a fallback.
    return pulse::bsonjson::toJson(newDoc.view());
  } catch (const std::exception& e) {
    pulse::log::error("[alterego] getOrCreate failed: {}", e.what());
    throw;
  }
}

// -----------------------------------------------------------------------------
// Instance helpers
// -----------------------------------------------------------------------------
std::optional<Json::Value> updateTraining(const bsoncxx::oid& id,
                                          const Json::Value& trainingData) {
  try {
    auto col = pulse::db::collection(kCollection);

    // Load the current doc so we can merge training fields the way Mongoose does
    // (assign each valid field onto this.training, then recompute trainingLevel).
    auto current = col.find_one(make_document(kvp("_id", id)));
    if (!current) return std::nullopt;

    Json::Value doc = pulse::bsonjson::toJson(current->view());
    Json::Value training = doc.isMember("training") && doc["training"].isObject()
                               ? doc["training"]
                               : Json::Value(Json::objectValue);

    // const validFields = ['howAreYou','favoriteTopics','humorStyle',
    //                      'complimentResponse','hotTakes','phrases','emojis'];
    static const char* kValidFields[] = {"howAreYou", "favoriteTopics",
                                         "humorStyle", "complimentResponse",
                                         "hotTakes", "phrases", "emojis"};
    if (trainingData.isObject()) {
      for (const char* f : kValidFields) {
        if (trainingData.isMember(f)) {
          training[f] = trainingData[f];
        }
      }
    }

    // this.trainingLevel = filled.length;
    const int level = computeTrainingLevel(training);

    // Build the $set update for the merged training fields + trainingLevel.
    // Re-serialize the training sub-document from JSON so embedded arrays persist.
    bld::document setDoc;
    setDoc.append(kvp("training", pulse::bsonjson::fromJson(training)));
    setDoc.append(kvp("trainingLevel", level));
    setDoc.append(kvp("updatedAt", nowDate()));

    auto update = make_document(kvp("$set", setDoc.extract()));

    mongocxx::options::find_one_and_update opts{};
    opts.return_document(mongocxx::options::return_document::k_after);

    auto result = col.find_one_and_update(
        make_document(kvp("_id", id)), update.view(), opts);
    if (!result) return std::nullopt;
    return pulse::bsonjson::toJson(result->view());
  } catch (const std::exception& e) {
    pulse::log::error("[alterego] updateTraining failed: {}", e.what());
    throw;
  }
}

std::optional<Json::Value> recordGuessResult(const bsoncxx::oid& id,
                                            bool guessedCorrectly) {
  try {
    auto col = pulse::db::collection(kCollection);

    // guessWhoStats.totalGames++;
    // if (guessedCorrectly) correctGuesses++ else fooledCount++;
    bld::document inc;
    inc.append(kvp("guessWhoStats.totalGames", 1));
    if (guessedCorrectly) {
      inc.append(kvp("guessWhoStats.correctGuesses", 1));
    } else {
      inc.append(kvp("guessWhoStats.fooledCount", 1));
    }

    auto update = make_document(
        kvp("$inc", inc.extract()),
        kvp("$set", make_document(kvp("updatedAt", nowDate()))));

    mongocxx::options::find_one_and_update opts{};
    opts.return_document(mongocxx::options::return_document::k_after);

    auto result = col.find_one_and_update(
        make_document(kvp("_id", id)), update.view(), opts);
    if (!result) return std::nullopt;

    // return this.guessWhoStats;
    Json::Value doc = pulse::bsonjson::toJson(result->view());
    if (doc.isMember("guessWhoStats")) return doc["guessWhoStats"];
    return Json::Value(Json::objectValue);
  } catch (const std::exception& e) {
    pulse::log::error("[alterego] recordGuessResult failed: {}", e.what());
    throw;
  }
}

void learnFromUser(const bsoncxx::oid& id, const std::string& trigger,
                   const std::string& response) {
  try {
    auto col = pulse::db::collection(kCollection);

    auto current = col.find_one(make_document(kvp("_id", id)));
    if (!current) return;

    Json::Value doc = pulse::bsonjson::toJson(current->view());
    Json::Value patterns =
        doc.isMember("responsePatterns") && doc["responsePatterns"].isArray()
            ? doc["responsePatterns"]
            : Json::Value(Json::arrayValue);

    // const existing = this.responsePatterns.find(p =>
    //   p.trigger.toLowerCase().includes(trigger.toLowerCase()));
    const std::string triggerLower = toLower(trigger);
    int existingIdx = -1;
    for (Json::ArrayIndex i = 0; i < patterns.size(); ++i) {
      const Json::Value& p = patterns[i];
      if (p.isObject() && p.isMember("trigger") && p["trigger"].isString()) {
        const std::string pTriggerLower = toLower(p["trigger"].asString());
        if (pTriggerLower.find(triggerLower) != std::string::npos) {
          existingIdx = static_cast<int>(i);
          break;
        }
      }
    }

    if (existingIdx >= 0) {
      // existing.frequency++;
      Json::Value& p = patterns[static_cast<Json::ArrayIndex>(existingIdx)];
      long long freq = p.isMember("frequency") && p["frequency"].isNumeric()
                           ? p["frequency"].asLargestInt()
                           : 0;
      p["frequency"] = static_cast<Json::Int64>(freq + 1);
    } else {
      // this.responsePatterns.push({ trigger, response, frequency: 1 });
      Json::Value p(Json::objectValue);
      p["trigger"] = trigger;
      p["response"] = response;
      p["frequency"] = 1;
      patterns.append(p);
    }

    // this.responsePatterns.sort((a,b) => b.frequency - a.frequency);
    std::vector<Json::Value> sorted;
    sorted.reserve(patterns.size());
    for (const auto& p : patterns) sorted.push_back(p);
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const Json::Value& a, const Json::Value& b) {
                       long long fa = a.isMember("frequency") && a["frequency"].isNumeric()
                                          ? a["frequency"].asLargestInt() : 0;
                       long long fb = b.isMember("frequency") && b["frequency"].isNumeric()
                                          ? b["frequency"].asLargestInt() : 0;
                       return fb < fa;  // descending by frequency
                     });

    // this.responsePatterns = this.responsePatterns.slice(0, 100);
    // Build the BSON array directly (fromJson() only converts top-level objects,
    // not arrays). Each entry has only String/Number fields — no oids/dates — so
    // a plain rebuild matches Mongoose's stored shape.
    const size_t keep = std::min<size_t>(sorted.size(), kMaxResponsePatterns);
    bld::array patternsArr;
    for (size_t i = 0; i < keep; ++i) {
      const Json::Value& p = sorted[i];
      const std::string trig = p.get("trigger", "").asString();
      const std::string resp = p.get("response", "").asString();
      const long long freq = p.isMember("frequency") && p["frequency"].isNumeric()
                                 ? p["frequency"].asLargestInt() : 0;
      patternsArr.append(make_document(kvp("trigger", trig),
                                       kvp("response", resp),
                                       kvp("frequency", freq)));
    }

    auto update = make_document(
        kvp("$set", make_document(
            kvp("responsePatterns", patternsArr.extract()),
            kvp("updatedAt", nowDate()))));

    col.update_one(make_document(kvp("_id", id)), update.view());
  } catch (const std::exception& e) {
    pulse::log::error("[alterego] learnFromUser failed: {}", e.what());
    throw;
  }
}

bool recordReply(const bsoncxx::oid& id, const std::string& action,
                 const std::string& targetUserId,
                 const std::string& originalMessage,
                 const std::string& egoResponse) {
  try {
    auto col = pulse::db::collection(kCollection);

    // action: context.action || 'dm_reply'
    const std::string actionVal = action.empty() ? "dm_reply" : action;

    // targetUser: context.targetUserId || null
    std::optional<bsoncxx::oid> targetOid;
    if (!targetUserId.empty()) targetOid = tryOid(targetUserId);

    // originalMessage: message.substring(0, 200)
    const std::string origTrunc =
        originalMessage.size() > 200 ? originalMessage.substr(0, 200)
                                     : originalMessage;
    // egoResponse: response.substring(0, 200)
    const std::string respTrunc =
        egoResponse.size() > 200 ? egoResponse.substr(0, 200) : egoResponse;

    const bsoncxx::types::b_date now = nowDate();

    // The activity log entry (matches the unshift({...}) shape, defaults filled).
    bld::document entry;
    entry.append(kvp("action", actionVal));
    if (targetOid) {
      entry.append(kvp("targetUser", *targetOid));
    } else {
      entry.append(kvp("targetUser", bsoncxx::types::b_null{}));
    }
    entry.append(kvp("originalMessage", origTrunc));
    entry.append(kvp("egoResponse", respTrunc));
    entry.append(kvp("wasRevealed", false));            // schema default
    entry.append(kvp("guessedCorrectly", bsoncxx::types::b_null{})); // default null
    entry.append(kvp("timestamp", now));
    // Mongoose assigns each embedded subdocument its own _id on save.
    entry.append(kvp("_id", bsoncxx::oid{}));

    // $each takes an array of documents to push.
    bld::array each;
    each.append(entry.extract());

    // this.totalReplies++; this.lastActive = now;
    // activityLog.unshift(entry) + cap at 100 -> $push with $position:0 + $slice:100.
    auto update = make_document(
        kvp("$inc", make_document(kvp("totalReplies", 1))),
        kvp("$set", make_document(kvp("lastActive", now),
                                  kvp("updatedAt", now))),
        kvp("$push", make_document(kvp("activityLog", make_document(
            kvp("$each", each.extract()),
            kvp("$position", 0),
            kvp("$slice", kMaxActivityLog))))));

    auto result = col.update_one(make_document(kvp("_id", id)), update.view());
    if (!result) return false;
    return result->matched_count() > 0;
  } catch (const std::exception& e) {
    pulse::log::error("[alterego] recordReply failed: {}", e.what());
    throw;
  }
}

} // namespace pulse::models::alterego
