// roulette.cc — C++ port of src/models/Roulette.js.
//
// Source of truth: src/models/Roulette.js. This file recreates the three
// declared indexes (including the TTL index on expiresAt), the schema defaults,
// and the statics/instance methods that carried real query logic:
//   statics:  joinQueue, findMatch, getUserHistory
//   methods:  startChat, addMessage, recordDecision
// Filters, sorts, limits, enum transitions, and the 100-message cap are
// preserved 1:1. BSON is built with the bsoncxx basic builders; documents are
// converted to Json::Value via pulse::bsonjson::toJson.
#include "pulse/models/roulette.hpp"

#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/oid.hpp>
#include <openssl/rand.h>

#include <mongocxx/collection.hpp>
#include <mongocxx/cursor.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/index.hpp>
#include <mongocxx/options/find_one_and_update.hpp>
#include <mongocxx/exception/operation_exception.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace pulse::models::roulette {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;
using bld::make_array;
namespace bj = pulse::bsonjson;

namespace {

// Milliseconds since epoch (matches Date.now()).
long long nowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// A bsoncxx b_date for "now" (matches `new Date()`).
bsoncxx::types::b_date nowDate() {
  return bsoncxx::types::b_date{std::chrono::system_clock::now()};
}

// b_date from a millisecond epoch value.
bsoncxx::types::b_date dateFromMillis(long long ms) {
  return bsoncxx::types::b_date{std::chrono::milliseconds{ms}};
}

// A userId from the request: ObjectId if it is a 24-hex string, else the raw
// string (mirrors Mongoose casting the value into the query).
void appendUserId(bld::document& doc, const std::string& key, const std::string& userId) {
  if (auto id = bj::tryOid(userId)) doc.append(kvp(key, *id));
  else                              doc.append(kvp(key, userId));
}

std::uint32_t secureUniform(std::uint32_t upperExclusive) {
  if (upperExclusive == 0) throw std::invalid_argument("empty random range");
  const std::uint64_t range =
      static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1;
  const std::uint64_t limit = range - (range % upperExclusive);
  std::uint32_t value = 0;
  do {
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&value), sizeof(value)) != 1)
      throw std::runtime_error("secure roulette selection failed");
  } while (static_cast<std::uint64_t>(value) >= limit);
  return value % upperExclusive;
}

// Pick a cryptographically unpredictable, unbiased icebreaker index.
const std::string& randomIcebreaker() {
  const auto& pool = icebreakers();
  return pool[secureUniform(static_cast<std::uint32_t>(pool.size()))];
}

// Read a Json field's value as a comparable id string. toJson renders ObjectId
// as its 24-char hex; this matches the JS `u.user.toString()` comparison.
std::string idString(const Json::Value& v) {
  if (v.isString()) return v.asString();
  if (v.isObject() && v.isMember("_id")) return v["_id"].asString();
  return v.asString();
}

// The active-status predicate is shared by queue admission, race recovery, and
// the partial unique index that enforces one live session per user.
bsoncxx::document::value activeStatuses() {
  return make_document(kvp("$in", make_array(
      "waiting", "matched", "chatting", "deciding")));
}

std::optional<Json::Value> findActiveSession(mongocxx::collection& col,
                                             const std::string& userId) {
  bld::document filter;
  appendUserId(filter, "users.user", userId);
  filter.append(kvp("status", activeStatuses()));
  auto found = col.find_one(filter.view());
  if (!found) return std::nullopt;
  return sanitizeForOutput(bj::toJson(found->view()));
}

bool isDuplicateKey(const mongocxx::operation_exception& e) {
  return e.code().value() == 11000 ||
         std::string(e.what()).find("E11000") != std::string::npos;
}

} // namespace

// -----------------------------------------------------------------------------
// Icebreaker pool — mirrors the ICEBREAKERS array in Roulette.js verbatim.
// -----------------------------------------------------------------------------
const std::vector<std::string>& icebreakers() {
  static const std::vector<std::string> kIcebreakers = {
      "What's the most random thing you've learned recently? \xF0\x9F\xA4\x93",
      "If you could only listen to one song for a week, what would it be? \xF0\x9F\x8E\xB5",
      "Hot take: pineapple on pizza? \xF0\x9F\x8D\x95",
      "What's your go-to comfort show? \xF0\x9F\x93\xBA",
      "If you could teleport anywhere right now, where? \xE2\x9C\x88\xEF\xB8\x8F",
      "What's the best compliment you've ever received? \xF0\x9F\x92\xAB",
      "Describe yourself in 3 emojis. Go! \xF0\x9F\x8E\xAD",
      "What's something on your bucket list? \xF0\x9F\xAA\xA3",
      "Morning person or night owl? \xF0\x9F\x8C\x99",
      "What's the last thing that made you laugh? \xF0\x9F\x98\x82",
      "If you had a superpower, what would it be? \xE2\x9A\xA1",
      "What's your unpopular opinion? \xF0\x9F\x97\xA3\xEF\xB8\x8F",
      "Coffee or tea? This is important. \xE2\x98\x95",
      "What's a skill you wish you had? \xF0\x9F\x8E\xAF",
      "What would your alter ego's name be? \xF0\x9F\x8E\xAD"};
  return kIcebreakers;
}

// -----------------------------------------------------------------------------
// Indexes
// -----------------------------------------------------------------------------
void ensureIndexes() {
  try {
    auto col = pulse::db::collection(kCollection);

    // rouletteSchema.index({ status: 1 });
    col.create_index(make_document(kvp("status", 1)));

    // rouletteSchema.index({ 'users.user': 1 });
    col.create_index(make_document(kvp("users.user", 1)));

    // Enforce the invariant the model methods rely on: a user may occur in at
    // most one active roulette document. Completed/expired history remains
    // unrestricted and immediately frees both participants for another match.
    {
      auto partial = make_document(kvp("status", activeStatuses()));
      mongocxx::options::index opts{};
      opts.name("uniq_active_roulette_user");
      opts.unique(true);
      opts.partial_filter_expression(partial.view());
      col.create_index(make_document(kvp("users.user", 1)), opts);
    }

    // rouletteSchema.index({ expiresAt: 1 }, { expireAfterSeconds: 0 });
    {
      mongocxx::options::index opts{};
      opts.expire_after(std::chrono::seconds(0));
      col.create_index(make_document(kvp("expiresAt", 1)), opts);
    }

    pulse::log::info("[roulette] ensured indexes on '{}'", kCollection);
  } catch (const std::exception& e) {
    pulse::log::error("[roulette] ensureIndexes failed: {}", e.what());
    throw;
  }
}

// -----------------------------------------------------------------------------
// Defaults / serialization
// -----------------------------------------------------------------------------
Json::Value applyDefaults(Json::Value doc) {
  if (!doc.isObject()) doc = Json::Value(Json::objectValue);

  const std::string now = bj::nowIso8601();

  // users: [{ user (required), joinedAt default now, decision default 'pending' }]
  if (doc.isMember("users") && doc["users"].isArray()) {
    for (Json::Value& u : doc["users"]) {
      if (!u.isObject()) continue;
      if (!u.isMember("joinedAt") || u["joinedAt"].isNull())
        u["joinedAt"] = now;
      if (!u.isMember("decision") || u["decision"].isNull() ||
          (u["decision"].isString() && u["decision"].asString().empty()))
        u["decision"] = "pending";
    }
  } else if (!doc.isMember("users")) {
    doc["users"] = Json::Value(Json::arrayValue);
  }

  // status: enum, default 'waiting'.
  if (!doc.isMember("status") || doc["status"].isNull() ||
      (doc["status"].isString() && doc["status"].asString().empty())) {
    doc["status"] = "waiting";
  }

  // matchScore: Number, default 0.
  if (!doc.isMember("matchScore") || doc["matchScore"].isNull())
    doc["matchScore"] = kDefaultMatchScore;

  // matchReason: String, default ''.
  if (!doc.isMember("matchReason") || doc["matchReason"].isNull())
    doc["matchReason"] = "";

  // messages: [{ sender, text maxlength 500, timestamp default now }].
  if (doc.isMember("messages") && doc["messages"].isArray()) {
    for (Json::Value& m : doc["messages"]) {
      if (!m.isObject()) continue;
      if (!m.isMember("timestamp") || m["timestamp"].isNull())
        m["timestamp"] = now;
    }
  } else if (!doc.isMember("messages")) {
    doc["messages"] = Json::Value(Json::arrayValue);
  }

  // chatDuration: Number, default 180.
  if (!doc.isMember("chatDuration") || doc["chatDuration"].isNull())
    doc["chatDuration"] = kDefaultChatDuration;

  // outcome: enum, default null.
  if (!doc.isMember("outcome")) doc["outcome"] = Json::Value(Json::nullValue);

  // icebreaker: String, default ''.
  if (!doc.isMember("icebreaker") || doc["icebreaker"].isNull())
    doc["icebreaker"] = "";

  // timestamps: true -> createdAt / updatedAt on insert.
  if (!doc.isMember("createdAt") || doc["createdAt"].isNull())
    doc["createdAt"] = now;
  if (!doc.isMember("updatedAt") || doc["updatedAt"].isNull())
    doc["updatedAt"] = now;

  // Mongoose document version key.
  if (!doc.isMember("__v")) doc["__v"] = 0;

  return doc;
}

Json::Value sanitizeForOutput(Json::Value doc) {
  // No toJSON transform and no select:false / sensitive fields in this schema;
  // default Mongoose JSON keeps every field. Only drop the internal version key.
  if (doc.isObject()) doc.removeMember("__v");
  return doc;
}

// -----------------------------------------------------------------------------
// Helpers: build BSON for the embedded users/messages arrays from Json.
// -----------------------------------------------------------------------------
namespace {

// Parse an ISO-8601 UTC timestamp (YYYY-MM-DDTHH:MM:SS.mmmZ) to epoch millis.
// Returns std::nullopt if it cannot be parsed.
std::optional<long long> isoToMillis(const std::string& iso) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0;
  if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d.%dZ",
                  &y, &mo, &d, &h, &mi, &s, &ms) >= 6) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon  = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min  = mi;
    tm.tm_sec  = s;
#if defined(_WIN32)
    std::time_t t = _mkgmtime(&tm);
#else
    std::time_t t = timegm(&tm);
#endif
    return static_cast<long long>(t) * 1000 + ms;
  }
  return std::nullopt;
}

// Build a users[] subdocument array preserving each element's existing joinedAt
// (parsed back from the ISO string toJson produced) and decision. Used by
// recordDecision's save(), which must NOT reset joinedAt.
bsoncxx::array::value buildUsersArrayWithDecisions(const Json::Value& users) {
  bld::array arr;
  for (const Json::Value& u : users) {
    bld::document d;
    if (u.isMember("user")) {
      const std::string uid = idString(u["user"]);
      if (auto id = bj::tryOid(uid)) d.append(kvp("user", *id));
      else                          d.append(kvp("user", uid));
    }
    // joinedAt: preserve the stored instant.
    if (u.isMember("joinedAt") && u["joinedAt"].isString()) {
      if (auto ms = isoToMillis(u["joinedAt"].asString()))
        d.append(kvp("joinedAt", dateFromMillis(*ms)));
      else
        d.append(kvp("joinedAt", nowDate()));
    } else {
      d.append(kvp("joinedAt", nowDate()));
    }
    const std::string decision =
        u.isMember("decision") && u["decision"].isString() ? u["decision"].asString()
                                                            : "pending";
    d.append(kvp("decision", decision));
    // Preserve the existing Mongoose subdocument _id so save() doesn't churn it.
    if (u.isMember("_id") && u["_id"].isString()) {
      if (auto sid = bj::tryOid(u["_id"].asString())) d.append(kvp("_id", *sid));
    }
    arr.append(d.extract());
  }
  return arr.extract();
}

} // namespace

// -----------------------------------------------------------------------------
// Statics
// -----------------------------------------------------------------------------
Json::Value joinQueue(const std::string& userId) {
  try {
    auto col = pulse::db::collection(kCollection);

    // Already in an active session?
    // findOne({ 'users.user': userId,
    //           status: { $in: ['waiting','matched','chatting','deciding'] } })
    bld::document filter;
    appendUserId(filter, "users.user", userId);
    filter.append(kvp("status", activeStatuses()));

    auto existing = col.find_one(filter.view());
    if (existing) return sanitizeForOutput(bj::toJson(existing->view()));

    // Create a waiting session:
    //   { users:[{ user:userId, joinedAt:now }], status:'waiting' }
    // applyDefaults supplies the per-element decision='pending', the top-level
    // status/matchScore/.../icebreaker defaults, timestamps, and __v.
    Json::Value session(Json::objectValue);
    Json::Value usersArr(Json::arrayValue);
    Json::Value entry(Json::objectValue);
    entry["user"] = userId;
    usersArr.append(entry);
    session["users"] = usersArr;
    session["status"] = "waiting";
    session = applyDefaults(session);

    // Build the insert document. The users array holds the single joining user:
    //   { user: userId, joinedAt: now, decision: 'pending' }
    const long long nowMs = nowMillis();
    bld::array usersBson;
    {
      bld::document entryDoc;
      if (auto id = bj::tryOid(userId)) entryDoc.append(kvp("user", *id));
      else                              entryDoc.append(kvp("user", userId));
      entryDoc.append(kvp("joinedAt", dateFromMillis(nowMs)));
      entryDoc.append(kvp("decision", "pending"));
      entryDoc.append(kvp("_id", bsoncxx::oid{}));  // Mongoose subdoc _id
      usersBson.append(entryDoc.extract());
    }
    bld::document insert;
    insert.append(kvp("users", usersBson.extract()));
    insert.append(kvp("status", session["status"].asString()));
    insert.append(kvp("matchScore", session["matchScore"].asInt()));
    insert.append(kvp("matchReason", session["matchReason"].asString()));
    insert.append(kvp("messages", make_array()));
    insert.append(kvp("chatDuration", session["chatDuration"].asInt()));
    insert.append(kvp("outcome", bsoncxx::types::b_null{}));
    insert.append(kvp("icebreaker", session["icebreaker"].asString()));
    insert.append(kvp("createdAt", dateFromMillis(nowMs)));
    insert.append(kvp("updatedAt", dateFromMillis(nowMs)));
    insert.append(kvp("__v", 0));

    bsoncxx::v_noabi::stdx::optional<mongocxx::result::insert_one> inserted;
    try {
      inserted = col.insert_one(insert.view());
    } catch (const mongocxx::operation_exception& e) {
      if (!isDuplicateKey(e)) throw;
      // Another request admitted this user after our initial read. The partial
      // unique index chose the winner; return that canonical active session.
      if (auto raced = findActiveSession(col, userId)) return *raced;
      throw;
    }
    if (!inserted) return sanitizeForOutput(session);

    auto created = col.find_one(make_document(kvp("_id", inserted->inserted_id())));
    if (!created) return sanitizeForOutput(session);
    return sanitizeForOutput(bj::toJson(created->view()));
  } catch (const std::exception& e) {
    pulse::log::error("[roulette] joinQueue failed: {}", e.what());
    throw;
  }
}

std::optional<Json::Value> findMatch(const std::string& userId,
                                     const Json::Value& /*userVibes*/) {
  try {
    auto col = pulse::db::collection(kCollection);

    // Atomically claim the longest-waiting eligible document. Keeping the full
    // eligibility predicate in find_one_and_update means two matchers cannot
    // both append themselves to the same one-person session.
    const long long nowMs = nowMillis();
    bld::document filter;
    filter.append(kvp("status", "waiting"));
    {
      bld::document ne;
      if (auto id = bj::tryOid(userId)) ne.append(kvp("$ne", *id));
      else                              ne.append(kvp("$ne", userId));
      filter.append(kvp("users.user", ne.extract()));
    }
    filter.append(kvp("users.0.joinedAt",
                      make_document(kvp("$gte",
                          dateFromMillis(nowMs - 5LL * 60 * 1000)))));
    filter.append(kvp("users.1", make_document(kvp("$exists", false))));

    // Mutate waiting[0]:
    //   users.push({ user:userId, joinedAt:now })
    //   status='matched'; matchedAt=now;
    //   icebreaker = random; expiresAt = now + 15min
    bld::document newUserEntry;
    if (auto id = bj::tryOid(userId)) newUserEntry.append(kvp("user", *id));
    else                              newUserEntry.append(kvp("user", userId));
    newUserEntry.append(kvp("joinedAt", dateFromMillis(nowMs)));
    newUserEntry.append(kvp("decision", "pending"));
    newUserEntry.append(kvp("_id", bsoncxx::oid{}));  // Mongoose subdoc _id

    bld::document update;
    update.append(kvp("$push", make_document(kvp("users", newUserEntry.extract()))));
    update.append(kvp("$set", make_document(
        kvp("status", "matched"),
        kvp("matchedAt", dateFromMillis(nowMs)),
        kvp("icebreaker", randomIcebreaker()),
        kvp("expiresAt", dateFromMillis(nowMs + 15LL * 60 * 1000)),
        kvp("updatedAt", dateFromMillis(nowMs)))));
    update.append(kvp("$inc", make_document(kvp("__v", 1))));

    mongocxx::options::find_one_and_update fouOpts{};
    fouOpts.return_document(mongocxx::options::return_document::k_after);
    fouOpts.sort(make_document(kvp("users.0.joinedAt", 1)));

    bsoncxx::v_noabi::stdx::optional<bsoncxx::document::value> result;
    try {
      result = col.find_one_and_update(filter.view(), update.view(), fouOpts);
    } catch (const mongocxx::operation_exception& e) {
      if (!isDuplicateKey(e)) throw;
      // A concurrent request already placed this user into an active session.
      // Only surface it as a match when it really has a partner; a one-user
      // waiting document must flow through joinQueue instead.
      auto raced = findActiveSession(col, userId);
      if (raced && (*raced)["users"].isArray() && (*raced)["users"].size() >= 2)
        return raced;
      return std::nullopt;
    }
    if (!result) return std::nullopt;
    return sanitizeForOutput(bj::toJson(result->view()));
  } catch (const std::exception& e) {
    pulse::log::error("[roulette] findMatch failed: {}", e.what());
    throw;
  }
}

std::vector<Json::Value> getUserHistory(const std::string& userId, int limit) {
  try {
    auto col = pulse::db::collection(kCollection);

    // find({ 'users.user': userId, status:'completed' })
    //   .sort({ createdAt:-1 }).limit(limit)
    // NOTE: .populate('users.user', ...) is resolved at the service/controller
    // layer here; this returns the raw documents (user refs as ObjectId hex),
    // mirroring the unpopulated shape — population is a read-side join.
    bld::document filter;
    appendUserId(filter, "users.user", userId);
    filter.append(kvp("status", "completed"));

    mongocxx::options::find opts{};
    opts.sort(make_document(kvp("createdAt", -1)));
    opts.limit(static_cast<std::int64_t>(limit));

    std::vector<Json::Value> out;
    auto cursor = col.find(filter.view(), opts);
    for (const auto& doc : cursor) {
      out.push_back(sanitizeForOutput(bj::toJson(doc)));
    }
    return out;
  } catch (const std::exception& e) {
    pulse::log::error("[roulette] getUserHistory failed: {}", e.what());
    throw;
  }
}

// -----------------------------------------------------------------------------
// Instance helpers
// -----------------------------------------------------------------------------
std::optional<Json::Value> startChat(const bsoncxx::oid& sessionId) {
  try {
    auto col = pulse::db::collection(kCollection);

    // status='chatting'; chatStartedAt=now; save().
    auto filter = make_document(kvp("_id", sessionId));
    auto update = make_document(kvp("$set", make_document(
        kvp("status", "chatting"),
        kvp("chatStartedAt", nowDate()))));

    mongocxx::options::find_one_and_update opts{};
    opts.return_document(mongocxx::options::return_document::k_after);

    auto result = col.find_one_and_update(filter.view(), update.view(), opts);
    if (!result) return std::nullopt;
    return sanitizeForOutput(bj::toJson(result->view()));
  } catch (const std::exception& e) {
    pulse::log::error("[roulette] startChat failed: {}", e.what());
    throw;
  }
}

std::optional<Json::Value> addMessage(const bsoncxx::oid& sessionId,
                                      const std::string& senderId,
                                      const std::string& text) {
  try {
    auto col = pulse::db::collection(kCollection);

    // push { sender, text: text.substring(0,500), timestamp: now }
    const std::string clipped = text.size() > 500 ? text.substr(0, 500) : text;

    bld::document msg;
    if (auto id = bj::tryOid(senderId)) msg.append(kvp("sender", *id));
    else                                msg.append(kvp("sender", senderId));
    msg.append(kvp("text", clipped));
    msg.append(kvp("timestamp", nowDate()));
    msg.append(kvp("_id", bsoncxx::oid{}));  // Mongoose subdoc _id

    // $push with $slice:-100 caps the array at the last 100 messages (matches
    // `if (messages.length > 100) messages = messages.slice(-100)`).
    auto update = make_document(kvp("$push", make_document(
        kvp("messages", make_document(
            kvp("$each", make_array(msg.extract())),
            kvp("$slice", -100))))));

    mongocxx::options::find_one_and_update opts{};
    opts.return_document(mongocxx::options::return_document::k_after);

    auto result = col.find_one_and_update(
        make_document(kvp("_id", sessionId)), update.view(), opts);
    if (!result) return std::nullopt;

    // Return the appended (last) message subdocument.
    Json::Value doc = bj::toJson(result->view());
    if (doc.isMember("messages") && doc["messages"].isArray() &&
        !doc["messages"].empty()) {
      return doc["messages"][doc["messages"].size() - 1];
    }
    return Json::Value(Json::nullValue);
  } catch (const std::exception& e) {
    pulse::log::error("[roulette] addMessage failed: {}", e.what());
    throw;
  }
}

std::optional<Json::Value> recordDecision(const bsoncxx::oid& sessionId,
                                          const std::string& userId,
                                          const std::string& decision) {
  try {
    auto col = pulse::db::collection(kCollection);

    auto current = col.find_one(make_document(kvp("_id", sessionId)));
    if (!current) return std::nullopt;

    Json::Value doc = bj::toJson(current->view());

    // const userEntry = users.find(u => u.user.toString() === userId.toString())
    // if (!userEntry) throw new Error('User not in this session');
    bool found = false;
    if (doc.isMember("users") && doc["users"].isArray()) {
      for (Json::Value& u : doc["users"]) {
        if (idString(u["user"]) == userId) {
          u["decision"] = decision;
          found = true;
          break;
        }
      }
    }
    if (!found) throw std::runtime_error("User not in this session");

    // allDecided = users.every(u => u.decision !== 'pending')
    bool allDecided = true;
    std::vector<std::string> decisions;
    for (const Json::Value& u : doc["users"]) {
      const std::string d = u.isMember("decision") && u["decision"].isString()
                                ? u["decision"].asString() : "pending";
      decisions.push_back(d);
      if (d == "pending") allDecided = false;
    }

    std::string newStatus;
    std::string newOutcome;       // empty => leave outcome unchanged
    bool setOutcome = false;
    if (allDecided) {
      newStatus = "completed";
      const bool allConnect = std::all_of(decisions.begin(), decisions.end(),
          [](const std::string& d) { return d == "connect"; });
      const bool allPass = std::all_of(decisions.begin(), decisions.end(),
          [](const std::string& d) { return d == "pass"; });
      if (allConnect)      newOutcome = "mutual_connect";
      else if (allPass)    newOutcome = "mutual_pass";
      else                 newOutcome = "one_sided";
      setOutcome = true;
    } else {
      newStatus = "deciding";
    }

    // Persist: rewrite the users array (with the updated decision), status, and
    // (when completed) outcome — mirrors this.save().
    bld::document setDoc;
    setDoc.append(kvp("users", buildUsersArrayWithDecisions(doc["users"])));
    setDoc.append(kvp("status", newStatus));
    if (setOutcome) setDoc.append(kvp("outcome", newOutcome));

    auto update = make_document(kvp("$set", setDoc.extract()));

    mongocxx::options::find_one_and_update opts{};
    opts.return_document(mongocxx::options::return_document::k_after);
    auto result = col.find_one_and_update(
        make_document(kvp("_id", sessionId)), update.view(), opts);

    // return { outcome: this.outcome, status: this.status }
    Json::Value out(Json::objectValue);
    if (result) {
      Json::Value updated = bj::toJson(result->view());
      out["outcome"] = updated.isMember("outcome") ? updated["outcome"]
                                                    : Json::Value(Json::nullValue);
      out["status"] = updated.isMember("status") ? updated["status"]
                                                 : Json::Value(newStatus);
    } else {
      out["outcome"] = setOutcome ? Json::Value(newOutcome) : Json::Value(Json::nullValue);
      out["status"] = newStatus;
    }
    return out;
  } catch (const std::runtime_error&) {
    throw;  // propagate the "User not in this session" error verbatim.
  } catch (const std::exception& e) {
    pulse::log::error("[roulette] recordDecision failed: {}", e.what());
    throw;
  }
}

} // namespace pulse::models::roulette
