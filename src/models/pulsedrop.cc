// pulsedrop.cc — implementation of the PulseDrop model port. See
// include/pulse/models/pulsedrop.hpp for the schema summary.
//
// Ground truth: src/models/PulseDrop.js.
#include "pulse/models/pulsedrop.hpp"

#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/oid.hpp>

#include <mongocxx/collection.hpp>
#include <mongocxx/cursor.hpp>
#include <mongocxx/options/index.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/update.hpp>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <regex>
#include <algorithm>
#include <cctype>

namespace pulse::models::pulsedrop {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;
using bld::make_array;
using pulse::bsonjson::oid;
using pulse::bsonjson::tryOid;
using pulse::bsonjson::oidToHex;
using pulse::bsonjson::nowIso8601;

namespace {

// 24 hours in milliseconds (the expiresAt default offset).
constexpr long long kTwentyFourHoursMs = 24LL * 60 * 60 * 1000;

// A bsoncxx b_date for "now", used by the date-comparison queries that the JS
// performed with `new Date()`.
bsoncxx::types::b_date nowDate() {
  return bsoncxx::types::b_date{std::chrono::system_clock::now()};
}

long long nowMillisLocal() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Convert an epoch-millis instant to an ISO-8601 UTC timestamp matching
// nowIso8601()'s format (YYYY-MM-DDTHH:MM:SS.mmmZ).
std::string isoFromMillis(long long ms) {
  std::time_t secs = static_cast<std::time_t>(ms / 1000);
  int millis = static_cast<int>(ms % 1000);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &secs);
#else
  gmtime_r(&secs, &tm);
#endif
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                tm.tm_min, tm.tm_sec, millis);
  return std::string(buf);
}

// Parse an ISO-8601 UTC timestamp (YYYY-MM-DDTHH:MM:SS.mmmZ) to epoch millis.
// Returns std::nullopt when unparseable.
std::optional<long long> millisFromIso(const std::string& iso) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0;
  int n = std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d.%dZ", &y, &mo, &d, &h, &mi,
                      &s, &ms);
  if (n < 6) return std::nullopt;
  std::tm tm{};
  tm.tm_year = y - 1900;
  tm.tm_mon = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min = mi;
  tm.tm_sec = s;
#if defined(_WIN32)
  std::time_t t = _mkgmtime(&tm);
#else
  std::time_t t = timegm(&tm);
#endif
  return static_cast<long long>(t) * 1000 + ms;
}

// Read a BSON numeric element as long long (int32/int64/double), defaulting 0.
long long readLong(const bsoncxx::document::view& view, const char* key) {
  auto it = view.find(key);
  if (it == view.end()) return 0;
  switch (it->type()) {
    case bsoncxx::type::k_int32: return it->get_int32().value;
    case bsoncxx::type::k_int64: return it->get_int64().value;
    case bsoncxx::type::k_double: return static_cast<long long>(it->get_double().value);
    default: return 0;
  }
}

// Lowercase a copy of `s` (JS String.prototype.toLowerCase for ASCII tags).
std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

} // namespace

// -----------------------------------------------------------------------------
// Indexes
// -----------------------------------------------------------------------------
void ensureIndexes() {
  try {
    auto col = pulse::db::collection(kCollection);

    // pulseDropSchema.index({ status: 1, expiresAt: 1 });
    col.create_index(make_document(kvp("status", 1), kvp("expiresAt", 1)));

    // pulseDropSchema.index({ trendingScore: -1 });
    col.create_index(make_document(kvp("trendingScore", -1)));

    // pulseDropSchema.index({ hashtags: 1 });
    col.create_index(make_document(kvp("hashtags", 1)));

    pulse::log::info("PulseDrop indexes ensured on collection '{}'", kCollection);
  } catch (const std::exception& e) {
    pulse::log::error("PulseDrop ensureIndexes failed: {}", e.what());
    throw;
  }
}

// -----------------------------------------------------------------------------
// Defaults / serialization
// -----------------------------------------------------------------------------
Json::Value applyDefaults(Json::Value doc) {
  if (!doc.isObject()) doc = Json::Value(Json::objectValue);

  // triggerType: enum ['viral','trending_hashtag','event','manual'], default
  // 'viral'.
  if (!doc.isMember("triggerType") || doc["triggerType"].isNull() ||
      (doc["triggerType"].isString() && doc["triggerType"].asString().empty())) {
    doc["triggerType"] = kTriggerTypeViral;
  }

  // participants: [] (array of { user, response, joinedAt:Date.now }).
  if (!doc.isMember("participants") || doc["participants"].isNull()) {
    doc["participants"] = Json::Value(Json::arrayValue);
  }

  // participantCount: { default: 0 }.
  if (!doc.isMember("participantCount")) doc["participantCount"] = 0;

  // responseCount: { default: 0 }.
  if (!doc.isMember("responseCount")) doc["responseCount"] = 0;

  // timestamps + lifecycle dates.
  const long long now = nowMillisLocal();
  const std::string nowIso = isoFromMillis(now);

  // startsAt: { default: Date.now }.
  if (!doc.isMember("startsAt") || doc["startsAt"].isNull()) {
    doc["startsAt"] = nowIso;
  }

  // expiresAt: required, default () => new Date(Date.now() + 24h).
  if (!doc.isMember("expiresAt") || doc["expiresAt"].isNull()) {
    doc["expiresAt"] = isoFromMillis(now + kTwentyFourHoursMs);
  }

  // status: enum ['active','expired','featured'], default 'active'.
  if (!doc.isMember("status") || doc["status"].isNull() ||
      (doc["status"].isString() && doc["status"].asString().empty())) {
    doc["status"] = kStatusActive;
  }

  // totalEngagement: { default: 0 }.
  if (!doc.isMember("totalEngagement")) doc["totalEngagement"] = 0;

  // trending: { default: false }.
  if (!doc.isMember("trending")) doc["trending"] = false;

  // trendingScore: { default: 0 }.
  if (!doc.isMember("trendingScore")) doc["trendingScore"] = 0;

  // hashtags: [String] — defaults to an empty array.
  if (!doc.isMember("hashtags") || doc["hashtags"].isNull()) {
    doc["hashtags"] = Json::Value(Json::arrayValue);
  }

  // featuredResponses: [ObjectId] — defaults to an empty array.
  if (!doc.isMember("featuredResponses") || doc["featuredResponses"].isNull()) {
    doc["featuredResponses"] = Json::Value(Json::arrayValue);
  }

  // timestamps: true -> createdAt / updatedAt on insert.
  if (!doc.isMember("createdAt") || doc["createdAt"].isNull()) {
    doc["createdAt"] = nowIso;
  }
  if (!doc.isMember("updatedAt") || doc["updatedAt"].isNull()) {
    doc["updatedAt"] = nowIso;
  }

  // Mongoose version key.
  if (!doc.isMember("__v")) doc["__v"] = 0;

  return doc;
}

Json::Value sanitizeForOutput(Json::Value doc) {
  if (!doc.isObject()) return doc;
  // No select:false fields and no custom toJSON transform in pulseDropSchema;
  // default Mongoose JSON keeps every field. We only drop the version key.
  doc.removeMember("__v");
  return doc;
}

// -----------------------------------------------------------------------------
// Statics
// -----------------------------------------------------------------------------
Json::Value getActiveDrops(int limit) {
  try {
    auto col = pulse::db::collection(kCollection);

    // { status: 'active', expiresAt: { $gt: now } }
    auto filter = make_document(
        kvp("status", kStatusActive),
        kvp("expiresAt", make_document(kvp("$gt", nowDate()))));

    mongocxx::options::find opts{};
    // .sort({ trending: -1, trendingScore: -1, participantCount: -1 })
    opts.sort(make_document(kvp("trending", -1),
                            kvp("trendingScore", -1),
                            kvp("participantCount", -1)));
    // .limit(limit)
    opts.limit(limit);

    Json::Value out(Json::arrayValue);
    auto cursor = col.find(filter.view(), opts);
    for (const auto& view : cursor) {
      out.append(pulse::bsonjson::toJson(view));
    }
    return out;
  } catch (const std::exception& e) {
    pulse::log::error("PulseDrop getActiveDrops failed: {}", e.what());
    throw;
  }
}

std::optional<Json::Value> createFromViral(const std::string& postId,
                                           const CreateFromViralData& data) {
  try {
    // const Post = mongoose.model('Post');
    // const post = await Post.findById(postId);
    auto postOid = tryOid(postId);
    if (!postOid) return std::nullopt;  // invalid id -> no post found

    auto posts = pulse::db::collection("posts");
    auto post = posts.find_one(make_document(kvp("_id", *postOid)));
    // if (!post) throw new Error('Post not found');
    if (!post) return std::nullopt;

    // const hashtags = post.content?.match(/#\w+/g) || [];
    std::vector<std::string> hashtags;
    {
      auto view = post->view();
      auto it = view.find("content");
      if (it != view.end() && it->type() == bsoncxx::type::k_string) {
        std::string content(it->get_string().value);
        static const std::regex tagRe(R"(#\w+)");
        for (auto m = std::sregex_iterator(content.begin(), content.end(), tagRe);
             m != std::sregex_iterator(); ++m) {
          hashtags.push_back(m->str());
        }
      }
    }

    // title: data.title || `🔥 ${hashtags[0] || 'Trending'} Moment`
    std::string title;
    if (data.title && !data.title->empty()) {
      title = *data.title;
    } else {
      std::string first = hashtags.empty() ? std::string("Trending") : hashtags[0];
      title = std::string("\xF0\x9F\x94\xA5 ") + first + " Moment";  // 🔥
    }

    // description: data.description || 'Join the viral wave!'
    std::string description =
        (data.description && !data.description->empty())
            ? *data.description
            : std::string("Join the viral wave!");

    // hashtags: hashtags.map(h => h.toLowerCase())
    Json::Value hashtagsJson(Json::arrayValue);
    for (const auto& h : hashtags) hashtagsJson.append(toLower(h));

    // Build the JSON insert document, then apply schema defaults.
    Json::Value doc(Json::objectValue);
    doc["title"] = title;
    doc["description"] = description;
    doc["triggerPost"] = postId;
    doc["triggerType"] = kTriggerTypeViral;
    doc["hashtags"] = hashtagsJson;
    doc["trending"] = true;
    doc = applyDefaults(std::move(doc));

    // Build the BSON insert doc. triggerPost must be stored as a real ObjectId.
    bld::document builder{};
    builder.append(kvp("title", doc["title"].asString()));
    builder.append(kvp("description", doc["description"].asString()));
    builder.append(kvp("triggerPost", *postOid));
    builder.append(kvp("triggerType", doc["triggerType"].asString()));
    builder.append(kvp("status", doc["status"].asString()));
    builder.append(kvp("participantCount",
                       static_cast<std::int64_t>(doc["participantCount"].asInt64())));
    builder.append(kvp("responseCount",
                       static_cast<std::int64_t>(doc["responseCount"].asInt64())));
    builder.append(kvp("totalEngagement",
                       static_cast<std::int64_t>(doc["totalEngagement"].asInt64())));
    builder.append(kvp("trending", doc["trending"].asBool()));
    builder.append(kvp("trendingScore",
                       static_cast<std::int64_t>(doc["trendingScore"].asInt64())));
    builder.append(kvp("startsAt", doc["startsAt"].asString()));
    builder.append(kvp("expiresAt", doc["expiresAt"].asString()));
    builder.append(kvp("createdAt", doc["createdAt"].asString()));
    builder.append(kvp("updatedAt", doc["updatedAt"].asString()));
    builder.append(kvp("__v", static_cast<std::int32_t>(doc["__v"].asInt())));
    builder.append(kvp("participants", make_array()));      // []
    builder.append(kvp("featuredResponses", make_array())); // []
    builder.append(kvp("hashtags", [&](bld::sub_array sa) {
      for (const auto& h : doc["hashtags"]) sa.append(h.asString());
    }));

    auto insertDoc = builder.extract();
    auto result = pulse::db::collection(kCollection).insert_one(insertDoc.view());

    // Return the inserted document as JSON (with the generated _id).
    Json::Value outDoc = pulse::bsonjson::toJson(insertDoc.view());
    if (result && result->inserted_id().type() == bsoncxx::type::k_oid) {
      outDoc["_id"] = oidToHex(result->inserted_id().get_oid().value);
    }
    return outDoc;
  } catch (const std::exception& e) {
    pulse::log::error("PulseDrop createFromViral failed: {}", e.what());
    throw;
  }
}

long long expireOld() {
  try {
    auto col = pulse::db::collection(kCollection);

    // updateMany({ status:'active', expiresAt:{ $lte: now } },
    //            { $set: { status:'expired' } })
    auto filter = make_document(
        kvp("status", kStatusActive),
        kvp("expiresAt", make_document(kvp("$lte", nowDate()))));
    auto update = make_document(
        kvp("$set", make_document(kvp("status", kStatusExpired))));

    auto result = col.update_many(filter.view(), update.view());
    if (!result) return 0;
    return result->modified_count();
  } catch (const std::exception& e) {
    pulse::log::error("PulseDrop expireOld failed: {}", e.what());
    throw;
  }
}

// -----------------------------------------------------------------------------
// Instance methods
// -----------------------------------------------------------------------------
JoinResult join(const std::string& dropId,
                const std::string& userId,
                const std::string& responsePostId) {
  try {
    auto col = pulse::db::collection(kCollection);

    auto dropOid = tryOid(dropId);
    if (!dropOid) return JoinResult{};  // invalid id -> not found

    auto existing = col.find_one(make_document(kvp("_id", *dropOid)));
    if (!existing) return JoinResult{};  // no matching drop

    auto view = existing->view();

    long long participantCount = readLong(view, "participantCount");
    long long responseCount = readLong(view, "responseCount");

    const bool hasResponse = !responsePostId.empty();
    auto userOid = oid(userId);

    // Determine whether userId is already a participant, mirroring:
    //   this.participants.find(p => p.user.toString() === userId.toString())
    bool alreadyJoined = false;
    int participantIndex = -1;
    {
      auto pIt = view.find("participants");
      if (pIt != view.end() && pIt->type() == bsoncxx::type::k_array) {
        int idx = 0;
        for (const auto& el : pIt->get_array().value) {
          if (el.type() == bsoncxx::type::k_document) {
            auto pdoc = el.get_document().value;
            auto uIt = pdoc.find("user");
            if (uIt != pdoc.end() && uIt->type() == bsoncxx::type::k_oid &&
                uIt->get_oid().value == userOid) {
              alreadyJoined = true;
              participantIndex = idx;
              break;
            }
          }
          ++idx;
        }
      }
    }

    bsoncxx::builder::basic::document updateBuilder{};

    if (alreadyJoined) {
      // existing participant: if responsePostId, set its response + responseCount++
      if (hasResponse) {
        responseCount += 1;
        std::string responseField =
            "participants." + std::to_string(participantIndex) + ".response";
        updateBuilder.append(kvp("$set", [&](bld::sub_document sd) {
          sd.append(kvp(responseField, oid(responsePostId)));
          sd.append(kvp("responseCount", static_cast<std::int64_t>(responseCount)));
        }));
      }
      // else: nothing changes (JS save() is still called, but values are equal).
    } else {
      // push new participant { user, response, joinedAt: now }
      participantCount += 1;
      if (hasResponse) responseCount += 1;

      bsoncxx::types::b_date joinedAt = nowDate();
      auto participantDoc =
          hasResponse
              ? make_document(kvp("user", userOid),
                              kvp("response", oid(responsePostId)),
                              kvp("joinedAt", joinedAt))
              // response defaults to null (responsePostId === null).
              : make_document(kvp("user", userOid),
                              kvp("response", bsoncxx::types::b_null{}),
                              kvp("joinedAt", joinedAt));

      updateBuilder.append(kvp("$push",
          make_document(kvp("participants", participantDoc))));
      updateBuilder.append(kvp("$set", [&](bld::sub_document sd) {
        sd.append(kvp("participantCount", static_cast<std::int64_t>(participantCount)));
        if (hasResponse)
          sd.append(kvp("responseCount", static_cast<std::int64_t>(responseCount)));
      }));
    }

    // this.trendingScore = this.participantCount * 2 + this.responseCount * 5;
    long long trendingScore = participantCount * 2 + responseCount * 5;

    // Always set the recomputed trendingScore (the JS recomputes + saves
    // unconditionally). Merge into the existing $set when present.
    {
      bld::document finalUpdate{};
      auto extracted = updateBuilder.extract();
      auto uview = extracted.view();

      bld::document setDoc{};
      auto setIt = uview.find("$set");
      if (setIt != uview.end() && setIt->type() == bsoncxx::type::k_document) {
        for (const auto& el : setIt->get_document().value) {
          setDoc.append(kvp(el.key(), el.get_value()));
        }
      }
      setDoc.append(kvp("trendingScore", static_cast<std::int64_t>(trendingScore)));

      // Carry over $push if it was set.
      auto pushIt = uview.find("$push");
      if (pushIt != uview.end()) {
        finalUpdate.append(kvp("$push", pushIt->get_value()));
      }
      finalUpdate.append(kvp("$set", setDoc.extract()));

      auto upd = finalUpdate.extract();
      col.update_one(make_document(kvp("_id", *dropOid)), upd.view());
    }

    JoinResult res;
    res.found = true;
    res.participantCount = participantCount;
    res.responseCount = responseCount;
    res.trendingScore = trendingScore;
    return res;
  } catch (const std::exception& e) {
    pulse::log::error("PulseDrop join failed: {}", e.what());
    throw;
  }
}

std::string getTimeRemaining(long long expiresAtMillis, long long nowMillis) {
  long long remaining = expiresAtMillis - nowMillis;
  if (remaining <= 0) return "0h 0m";

  long long hours = remaining / (1000LL * 60 * 60);
  long long mins = (remaining % (1000LL * 60 * 60)) / (1000LL * 60);
  return std::to_string(hours) + "h " + std::to_string(mins) + "m";
}

std::string getTimeRemaining(const std::string& expiresAtIso) {
  auto ms = millisFromIso(expiresAtIso);
  if (!ms) return "0h 0m";  // unparseable -> treat as elapsed
  return getTimeRemaining(*ms, nowMillisLocal());
}

} // namespace pulse::models::pulsedrop
