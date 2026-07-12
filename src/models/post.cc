// post.cc — C++ port of src/models/Post.js. See post.hpp for the contract.
#include "pulse/models/post.hpp"

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
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/index.hpp>
#include <mongocxx/exception/exception.hpp>

#include <chrono>
#include <regex>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <string>

namespace pulse::models::post {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;
using bld::make_array;

namespace {

// "now" in epoch milliseconds (matches Date.now()).
long long nowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch()).count();
}

bsoncxx::types::b_date dateFromMillis(long long ms) {
  return bsoncxx::types::b_date{std::chrono::milliseconds{ms}};
}

// Parse an ISO-8601-ish date string into epoch millis (new Date(lastPostDate)).
// Accepts "YYYY-MM-DDTHH:MM:SS(.fff)(Z)" and "YYYY-MM-DD". Returns nullopt on
// an unparseable value (JS `new Date('bad')` -> Invalid Date; we treat that as
// "no boundary" by leaving it to the caller — here we simply skip the bound).
std::optional<long long> parseDateMillis(const std::string& s) {
  if (s.empty()) return std::nullopt;
  std::tm tm{};
  int year = 0, mon = 0, day = 0, hour = 0, min = 0, sec = 0;
  int matched = std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d",
                            &year, &mon, &day, &hour, &min, &sec);
  if (matched < 3) return std::nullopt;
  tm.tm_year = year - 1900;
  tm.tm_mon  = mon - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min  = min;
  tm.tm_sec  = sec;
#if defined(_WIN32)
  std::time_t t = _mkgmtime(&tm);
#else
  std::time_t t = timegm(&tm);
#endif
  if (t == static_cast<std::time_t>(-1)) return std::nullopt;
  long long ms = static_cast<long long>(t) * 1000;
  // Fractional seconds (".fff") if present.
  auto dot = s.find('.');
  if (dot != std::string::npos && dot + 1 < s.size()) {
    std::string frac;
    for (size_t i = dot + 1; i < s.size() && std::isdigit((unsigned char)s[i]); ++i)
      frac.push_back(s[i]);
    while (frac.size() < 3) frac.push_back('0');
    frac = frac.substr(0, 3);
    ms += std::stoll(frac);
  }
  return ms;
}

// Run a find with the given filter + find options, return a JSON array of docs.
Json::Value runFind(const bsoncxx::document::view_or_value& filter,
                    mongocxx::options::find opts) {
  Json::Value out(Json::arrayValue);
  try {
    auto col = pulse::db::collection(kCollection);
    auto cursor = col.find(filter, opts);
    for (const auto& doc : cursor) {
      out.append(pulse::bsonjson::toJson(doc));
    }
  } catch (const std::exception& e) {
    pulse::log::error("post find failed: {}", e.what());
  }
  return out;
}

std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

} // namespace

// ── Index management ───────────────────────────────────────────────────────
void ensureIndexes() {
  try {
    auto col = pulse::db::collection(kCollection);

    // Field-level: author: { index: true }
    col.create_index(make_document(kvp("author", 1)));

    // Field-level: vibe: { index: true }
    col.create_index(make_document(kvp("vibe", 1)));

    // postSchema.index({ author: 1, createdAt: -1 });
    col.create_index(make_document(kvp("author", 1), kvp("createdAt", -1)));

    // postSchema.index({ author: 1, isActive: 1, createdAt: -1 });
    col.create_index(make_document(
        kvp("author", 1), kvp("isActive", 1), kvp("createdAt", -1)));

    // postSchema.index({ isActive: 1, visibility: 1, createdAt: -1 });
    col.create_index(make_document(
        kvp("isActive", 1), kvp("visibility", 1), kvp("createdAt", -1)));

    // postSchema.index({ 'content.hashtags': 1 });
    col.create_index(make_document(kvp("content.hashtags", 1)));

    // postSchema.index({ isActive: 1, visibility: 1, createdAt: -1, 'stats.likes': -1 });
    col.create_index(make_document(
        kvp("isActive", 1), kvp("visibility", 1),
        kvp("createdAt", -1), kvp("stats.likes", -1)));

    // postSchema.index({ location: '2dsphere' });  (and the field-level
    // location.coordinates: '2dsphere' — same path tree; one 2dsphere on
    // `location` covers the GeoJSON Point stored there.)
    col.create_index(make_document(kvp("location", "2dsphere")));

    // postSchema.index({ createdAt: -1 });
    col.create_index(make_document(kvp("createdAt", -1)));

    // Weighted text index named "post_text_search".
    // postSchema.index(
    //   { 'content.text': 'text', 'content.hashtags': 'text' },
    //   { name, weights: { 'content.hashtags': 5, 'content.text': 1 } })
    {
      // Hold the weights document in a local so the view stored by
      // options::index::weights() stays valid until create_index runs.
      auto weightsDoc = make_document(
          kvp("content.hashtags", 5), kvp("content.text", 1));
      mongocxx::options::index textOpts{};
      textOpts.name("post_text_search");
      textOpts.weights(weightsDoc.view());
      col.create_index(
          make_document(kvp("content.text", "text"),
                        kvp("content.hashtags", "text")),
          textOpts);
    }
  } catch (const std::exception& e) {
    pulse::log::error("Post.ensureIndexes failed: {}", e.what());
  }
}

// ── Statics ─────────────────────────────────────────────────────────────────

// statics.getHomeFeed(userId, followingIds, options)
Json::Value getHomeFeed(const std::string& userId,
                        const std::vector<std::string>& followingIds,
                        const FeedOptions& options) {
  // author: { $in: [...followingIds, userId] }
  bld::array authorIn;
  for (const auto& id : followingIds) {
    if (auto o = pulse::bsonjson::tryOid(id)) authorIn.append(*o);
  }
  if (auto o = pulse::bsonjson::tryOid(userId)) authorIn.append(*o);

  bld::document filter;
  filter.append(kvp("isActive", true));
  filter.append(kvp("author", make_document(kvp("$in", authorIn.extract()))));
  filter.append(kvp("visibility",
                    make_document(kvp("$in", make_array("public", "followers")))));

  // createdAt: lastPostDate ? { $lt: new Date(lastPostDate) } : { $exists: true }
  if (auto ms = parseDateMillis(options.lastPostDate)) {
    filter.append(kvp("createdAt", make_document(kvp("$lt", dateFromMillis(*ms)))));
  } else {
    filter.append(kvp("createdAt", make_document(kvp("$exists", true))));
  }

  mongocxx::options::find opts{};
  opts.sort(make_document(kvp("createdAt", -1)));
  opts.limit(options.limit);

  return runFind(filter.extract(), std::move(opts));
}

// statics.getGlobalFeed(options)
Json::Value getGlobalFeed(const FeedOptions& options) {
  bld::document filter;
  filter.append(kvp("isActive", true));
  filter.append(kvp("visibility", "public"));

  if (auto ms = parseDateMillis(options.lastPostDate)) {
    filter.append(kvp("createdAt", make_document(kvp("$lt", dateFromMillis(*ms)))));
  } else {
    filter.append(kvp("createdAt", make_document(kvp("$exists", true))));
  }

  mongocxx::options::find opts{};
  opts.sort(make_document(kvp("createdAt", -1)));
  opts.limit(options.limit);

  return runFind(filter.extract(), std::move(opts));
}

// statics.getTrendingPosts(options)
Json::Value getTrendingPosts(const TrendingOptions& options) {
  // timeAgo = new Date(Date.now() - timeRange * 60 * 60 * 1000)
  long long timeAgoMs = nowMillis()
      - static_cast<long long>(options.timeRangeHours) * 60LL * 60LL * 1000LL;

  auto filter = make_document(
      kvp("isActive", true),
      kvp("visibility", "public"),
      kvp("createdAt", make_document(kvp("$gte", dateFromMillis(timeAgoMs)))));

  mongocxx::options::find opts{};
  // sort({ 'stats.likes': -1, 'stats.comments': -1 })
  opts.sort(make_document(kvp("stats.likes", -1), kvp("stats.comments", -1)));
  opts.limit(options.limit);

  return runFind(filter.view(), std::move(opts));
}

// statics.getNearbyPosts(coordinates, maxDistance, options)
Json::Value getNearbyPosts(const std::vector<double>& coordinates,
                           double maxDistance,
                           const NearbyOptions& options) {
  bld::array coords;
  for (double c : coordinates) coords.append(c);

  auto filter = make_document(
      kvp("isActive", true),
      kvp("visibility", "public"),
      kvp("location", make_document(kvp("$near", make_document(
          kvp("$geometry", make_document(
              kvp("type", "Point"),
              kvp("coordinates", coords.extract()))),
          kvp("$maxDistance", maxDistance))))));

  mongocxx::options::find opts{};
  opts.limit(options.limit);
  // (JS getNearbyPosts has no explicit .sort(); $near already orders by distance.)

  return runFind(filter.view(), std::move(opts));
}

// ── Instance methods ─────────────────────────────────────────────────────────

// methods.isLikedBy(userId) -> this.likes.some(id => id.toString() === userId)
bool isLikedBy(const Json::Value& likes, const std::string& userId) {
  if (!likes.isArray()) return false;
  for (const auto& id : likes) {
    std::string s;
    if (id.isString()) s = id.asString();
    else if (id.isObject() && id.isMember("_id")) s = id["_id"].asString();
    if (s == userId) return true;
  }
  return false;
}

// ── Insert defaults + output sanitization ───────────────────────────────────

Json::Value applyDefaults(Json::Value doc) {
  if (!doc.isObject()) doc = Json::Value(Json::objectValue);

  // content (text/media/hashtags/mentions are user-supplied; ensure subdoc
  // exists so hashtag derivation below is safe).
  if (!doc.isMember("content") || !doc["content"].isObject())
    doc["content"] = Json::Value(Json::objectValue);
  Json::Value& content = doc["content"];

  // location.type default 'Point' (only when a location object is present —
  // Mongoose only stamps the default on the nested path when the parent is set;
  // for parity we mirror that: stamp type when location is an object).
  if (doc.isMember("location") && doc["location"].isObject()) {
    if (!doc["location"].isMember("type"))
      doc["location"]["type"] = "Point";
  }

  // stats defaults: likes/comments/shares/views = 0
  if (!doc.isMember("stats") || !doc["stats"].isObject())
    doc["stats"] = Json::Value(Json::objectValue);
  Json::Value& stats = doc["stats"];
  if (!stats.isMember("likes"))    stats["likes"]    = 0;
  if (!stats.isMember("comments")) stats["comments"] = 0;
  if (!stats.isMember("shares"))   stats["shares"]   = 0;
  if (!stats.isMember("views"))    stats["views"]    = 0;

  // visibility default 'public' (enum: public|followers|private)
  if (!doc.isMember("visibility")) doc["visibility"] = "public";

  // Boolean flag defaults.
  if (!doc.isMember("allowComments")) doc["allowComments"] = true;
  if (!doc.isMember("isAnonymous"))   doc["isAnonymous"]   = false;
  if (!doc.isMember("isActive"))      doc["isActive"]      = true;
  if (!doc.isMember("isEdited"))      doc["isEdited"]      = false;
  if (!doc.isMember("isPinned"))      doc["isPinned"]      = false;
  if (!doc.isMember("isReported"))    doc["isReported"]    = false;
  if (!doc.isMember("isRepost"))      doc["isRepost"]      = false;

  // reportCount default 0
  if (!doc.isMember("reportCount")) doc["reportCount"] = 0;

  // vibe default 'general' (enum: chill|hype|sad|funny|creative|general)
  if (!doc.isMember("vibe")) doc["vibe"] = "general";

  // vibeScore defaults: chill/hype/sad/funny/creative = 0
  if (!doc.isMember("vibeScore") || !doc["vibeScore"].isObject())
    doc["vibeScore"] = Json::Value(Json::objectValue);
  Json::Value& vs = doc["vibeScore"];
  if (!vs.isMember("chill"))    vs["chill"]    = 0;
  if (!vs.isMember("hype"))     vs["hype"]     = 0;
  if (!vs.isMember("sad"))      vs["sad"]      = 0;
  if (!vs.isMember("funny"))    vs["funny"]    = 0;
  if (!vs.isMember("creative")) vs["creative"] = 0;

  // embeddingVersion default 0 (embedding has default: undefined -> not stamped).
  if (!doc.isMember("embeddingVersion")) doc["embeddingVersion"] = 0;

  // pre('save'): hashtags derived from content.text on insert.
  //   const hashtags = text.match(/#[\w]+/g);
  //   this.content.hashtags = hashtags.map(t => t.substring(1).toLowerCase());
  if (content.isMember("text") && content["text"].isString()) {
    const std::string text = content["text"].asString();
    static const std::regex hashtagRe(R"(#[\w]+)");
    Json::Value tags(Json::arrayValue);
    auto begin = std::sregex_iterator(text.begin(), text.end(), hashtagRe);
    auto end = std::sregex_iterator();
    bool any = false;
    for (auto it = begin; it != end; ++it) {
      any = true;
      std::string tag = it->str();          // includes leading '#'
      tags.append(toLower(tag.substr(1)));   // strip '#', lowercase
    }
    if (any) content["hashtags"] = tags;     // only overwrite when matches exist
  }

  // pre('save'): when likes are modified, stats.likes = likes.length.
  if (doc.isMember("likes") && doc["likes"].isArray()) {
    stats["likes"] = static_cast<int>(doc["likes"].size());
  }

  // timestamps: { createdAt, updatedAt } stamped on insert.
  std::string now = pulse::bsonjson::nowIso8601();
  if (!doc.isMember("createdAt")) doc["createdAt"] = now;
  if (!doc.isMember("updatedAt")) doc["updatedAt"] = now;

  // Mongoose version key.
  if (!doc.isMember("__v")) doc["__v"] = 0;

  return doc;
}

Json::Value sanitizeForOutput(Json::Value doc) {
  if (!doc.isObject()) return doc;
  // select:false fields never shipped to clients.
  doc.removeMember("embedding");
  doc.removeMember("embeddingVersion");
  // Version key dropped from the response shape.
  doc.removeMember("__v");
  return doc;
}

} // namespace pulse::models::post
