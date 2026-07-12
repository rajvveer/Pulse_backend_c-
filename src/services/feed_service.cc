// feed_service.cc — C++ port of src/services/feedService.js.
// See include/pulse/services/feed_service.hpp for the contract.
//
// Preserves EVERY redis key string, TTL value, Mongo query shape, and response
// field name from the JS source. The ranking payload built here matches, field
// for field, what pulse::algos::feedRank (src/algorithms/feed_algo.cc) consumes.
//
// The dependencies the JS feedController imported are ported and used directly:
//   * userVectorService          -> pulse::userVector()
//   * vectorRetrievalService     -> pulse::services::vector_retrieval
//   * trustService               -> pulse::trust()
//   * feedAlgo (native wrapper)  -> rankPosts() here + pulse::algos::feedRank
//   * InterestProfiler / VibeClassifier -> pulse::algos::interestScore /
//                                          pulse::algos::vibeClassify
// Each external service is best-effort (wrapped in try/catch) so the feed
// always produces results, mirroring the JS try/catch fallbacks exactly.
#include "pulse/services/feed_service.hpp"

#include "pulse/cache.hpp"
#include "pulse/algorithms.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"

#include "pulse/models/post.hpp"
#include "pulse/models/follow.hpp"
#include "pulse/models/like.hpp"
#include "pulse/models/userbehavior.hpp"
#include "pulse/models/userengagement.hpp"

// The same internal services the JS feedController imported
// (userVectorService / vectorRetrievalService / trustService), already ported.
#include "pulse/services/user_vector_service.hpp"
#include "pulse/services/vector_retrieval_service.hpp"
#include "pulse/services/trust_service.hpp"

#include "pulse/db.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/oid.hpp>

#include <mongocxx/collection.hpp>
#include <mongocxx/cursor.hpp>
#include <mongocxx/pipeline.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/exception/exception.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <memory>
#include <set>
#include <unordered_set>
#include <map>
#include <string>
#include <vector>

namespace pulse::services::feed {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;
using bld::make_array;

namespace {

// ── JSON (JsonCpp) serialize / parse helpers ────────────────────────────────
// Compact serialization (no pretty indentation, no trailing newline) so the
// payload string handed to the native kernel is minimal.
std::string dumpJson(const Json::Value& v) {
  Json::StreamWriterBuilder b;
  b["indentation"] = "";
  b["commentStyle"] = "None";
  return Json::writeString(b, v);
}

Json::Value parseJson(const std::string& s) {
  Json::Value v;
  if (s.empty()) return v;
  Json::CharReaderBuilder b;
  std::string errs;
  std::unique_ptr<Json::CharReader> reader(b.newCharReader());
  if (!reader->parse(s.data(), s.data() + s.size(), &v, &errs)) {
    return Json::Value();  // null on parse failure
  }
  return v;
}

// "now" in epoch milliseconds (Date.now()).
long long nowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch()).count();
}

int clampInt(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

// Extract the hex _id from a post JSON document (lean docs carry _id as the
// 24-char hex string after bsonjson::toJson).
std::string postId(const Json::Value& p) {
  if (!p.isObject()) return "";
  const Json::Value& id = p["_id"];
  if (id.isString()) return id.asString();
  if (id.isObject() && id.isMember("_id")) return id["_id"].asString();
  return "";
}

// Extract the author hex id from a post (author may be populated object or a
// raw id string).
std::string authorId(const Json::Value& p) {
  if (!p.isObject()) return "";
  const Json::Value& a = p["author"];
  if (a.isString()) return a.asString();
  if (a.isObject()) {
    if (a.isMember("_id")) {
      const Json::Value& id = a["_id"];
      if (id.isString()) return id.asString();
    }
  }
  return "";
}

// getMediaType(post): [] -> 'text_only'; video -> 'video'; gif -> 'gif';
// image -> 'image'; else 'text_only'. (feedAlgo _fallback getMediaType.)
std::string mediaType(const Json::Value& p) {
  const Json::Value& media = (p.isObject() && p["content"].isObject())
                                 ? p["content"]["media"] : Json::Value();
  if (!media.isArray() || media.empty()) return "text_only";
  bool v = false, g = false, i = false;
  for (const auto& m : media) {
    std::string t = (m.isObject() && m["type"].isString()) ? m["type"].asString() : "";
    if (t == "video") v = true;
    if (t == "gif")   g = true;
    if (t == "image") i = true;
  }
  if (v) return "video";
  if (g) return "gif";
  if (i) return "image";
  return "text_only";
}

double mediaBoost(const std::string& t) {
  if (t == "image") return 1.1;
  if (t == "video") return 1.3;
  if (t == "gif")   return 1.05;
  return 1.0;  // text_only / default
}

double asNum(const Json::Value& v, double def = 0.0) {
  return (v.isNumeric()) ? v.asDouble() : def;
}

// calculatePostScore(post): WEIGHTS.likes=1, comments=3, shares=4, views=0.05,
// times MEDIA_BOOST[mediaType]. Mirrors feedAlgo _fallback calculatePostScore;
// stats.likes falls back to post.likes.length when absent.
double calculatePostScore(const Json::Value& p) {
  const Json::Value& stats = (p.isObject() && p["stats"].isObject())
                                 ? p["stats"] : Json::Value(Json::objectValue);
  double likes = 0;
  if (stats.isMember("likes") && stats["likes"].isNumeric())
    likes = stats["likes"].asDouble();
  else if (p.isObject() && p["likes"].isArray())
    likes = static_cast<double>(p["likes"].size());
  double score = likes * 1.0
               + asNum(stats["comments"]) * 3.0
               + asNum(stats["shares"]) * 4.0
               + asNum(stats["views"]) * 0.05;
  score *= mediaBoost(mediaType(p));
  return score;
}

// Parse an ISO-8601-ish date string into epoch millis (new Date(str)). Returns
// nullopt on an unparseable value (JS Invalid Date -> we skip the bound).
std::optional<long long> parseDateMillis(const std::string& s) {
  if (s.empty()) return std::nullopt;
  int year = 0, mon = 0, day = 0, hour = 0, min = 0, sec = 0;
  int matched = std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d",
                            &year, &mon, &day, &hour, &min, &sec);
  if (matched < 3) return std::nullopt;
  std::tm tm{};
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

bsoncxx::types::b_date dateFromMillis(long long ms) {
  return bsoncxx::types::b_date{std::chrono::milliseconds{ms}};
}

// Convert a post's createdAt (ISO string or numeric ms) to epoch millis. The
// feedRank kernel expects createdAt as milliseconds (msFields(posts,
// ['createdAt']) in the JS wrapper).
long long createdAtMillis(const Json::Value& p, long long fallback) {
  if (!p.isObject() || !p.isMember("createdAt")) return fallback;
  const Json::Value& c = p["createdAt"];
  if (c.isNumeric()) return static_cast<long long>(c.asDouble());
  if (c.isString()) {
    if (auto ms = parseDateMillis(c.asString())) return *ms;
  }
  return fallback;
}

// Run a Mongo find on the posts collection with filter + opts -> JSON array.
Json::Value runPostFind(const bsoncxx::document::view_or_value& filter,
                        mongocxx::options::find opts) {
  Json::Value out(Json::arrayValue);
  try {
    auto col = pulse::db::collection(pulse::models::post::kCollection);
    auto cursor = col.find(filter, opts);
    for (const auto& doc : cursor) {
      out.append(pulse::bsonjson::toJson(doc));
    }
  } catch (const std::exception& e) {
    pulse::log::error("feed post find failed: {}", e.what());
  }
  return out;
}

// .populate('author', <select>) — hydrate each post's author ObjectId into the
// projected user subdocument in one query. Posts whose author cannot be resolved
// keep the original ref (Mongoose populate leaves a dangling ref as-is). Mirrors
// vectorRetrievalService.populateAuthors.
//   includeStats: the home/global/foryou feeds populate
//     'username name avatar profile isVerified stats'; the trending feed
//     populates 'username name avatar profile isVerified' (NO stats). The flag
//     selects which exact projection runs.
void populateAuthors(Json::Value& posts, bool includeStats = true) {
  if (!posts.isArray() || posts.empty()) return;

  std::vector<std::string> ids;
  for (const auto& p : posts) {
    if (p.isMember("author")) {
      const Json::Value& a = p["author"];
      std::string hex;
      if (a.isString()) hex = a.asString();
      else if (a.isObject() && a.isMember("_id") && a["_id"].isString())
        hex = a["_id"].asString();
      if (!hex.empty() && std::find(ids.begin(), ids.end(), hex) == ids.end())
        ids.push_back(hex);
    }
  }
  if (ids.empty()) return;

  Json::Value byId(Json::objectValue);
  try {
    bld::array in;
    for (const auto& hex : ids)
      if (auto o = pulse::bsonjson::tryOid(hex)) in.append(*o);
    auto filter = make_document(kvp("_id", make_document(kvp("$in", in.extract()))));
    // Projection mirrors the populate select string (Mongoose adds _id).
    bld::document proj;
    proj.append(kvp("username", 1), kvp("name", 1), kvp("avatar", 1),
                kvp("profile", 1), kvp("isVerified", 1));
    if (includeStats) proj.append(kvp("stats", 1));
    auto projection = proj.extract();
    mongocxx::options::find opts{};
    opts.projection(projection.view());

    auto col = pulse::db::collection("users");
    auto cursor = col.find(filter.view(), opts);
    for (const auto& doc : cursor) {
      Json::Value u = pulse::bsonjson::toJson(doc);
      if (u.isMember("_id") && u["_id"].isString())
        byId[u["_id"].asString()] = u;
    }
  } catch (const std::exception& e) {
    pulse::log::error("[feed] author populate failed: {}", e.what());
    return;
  }

  for (auto& p : posts) {
    if (!p.isMember("author")) continue;
    const Json::Value& a = p["author"];
    std::string hex;
    if (a.isString()) hex = a.asString();
    else if (a.isObject() && a.isMember("_id") && a["_id"].isString())
      hex = a["_id"].asString();
    if (!hex.empty() && byId.isMember(hex)) p["author"] = byId[hex];
  }
}

// Build a JSON array of strings from a vector.
Json::Value toJsonArray(const std::vector<std::string>& v) {
  Json::Value a(Json::arrayValue);
  for (const auto& s : v) a.append(s);
  return a;
}

// classifyVibe(post) — single-post wrapper over the vibeClassify kernel
// (VibeClassifier.classify). Returns { vibe, vibeScore, confidence, ... } or an
// empty object on error.
Json::Value classifyVibe(const Json::Value& post) {
  try {
    Json::Value in = post.isObject() ? post : Json::Value(Json::objectValue);
    std::string out = pulse::algos::vibeClassify(dumpJson(in));
    Json::Value v = parseJson(out);
    if (v.isObject()) return v;
  } catch (const std::exception& e) {
    pulse::log::warn("vibe classify failed: {}", e.what());
  }
  return Json::Value(Json::objectValue);
}

// applyVibe(posts, vibe) — ports feedAlgo.rankPostsWithVibe's PRE-RANK steps:
//   1. For each post: keep its existing vibe when set and != 'general';
//      otherwise classify it (stamp vibe / vibeScore / _vibeConfidence).
//   2. When vibe != 'auto': filter to
//        p.vibe === vibe || (p.vibeScore && p.vibeScore[vibe] > 1.5)
//      then boostByVibe(cp, vibe, 1.5): matching posts get _score *= 1.5 (here
//      _score is absent pre-rank, so (0)*1.5 = 0) and _vibeMatch = true.
// Returns the transformed candidate array. The caller then runs plain rankPosts.
Json::Value applyVibe(const Json::Value& posts, const std::string& vibe) {
  Json::Value cp(Json::arrayValue);
  if (!posts.isArray()) return cp;

  for (const auto& src : posts) {
    Json::Value post = src;
    bool hasVibe = post.isObject() && post.isMember("vibe")
                   && post["vibe"].isString()
                   && post["vibe"].asString() != "general";
    if (hasVibe) {
      cp.append(post);
      continue;
    }
    Json::Value c = classifyVibe(post);
    if (c.isObject()) {
      if (c.isMember("vibe"))       post["vibe"]            = c["vibe"];
      if (c.isMember("vibeScore"))  post["vibeScore"]       = c["vibeScore"];
      if (c.isMember("confidence")) post["_vibeConfidence"] = c["confidence"];
    }
    cp.append(post);
  }

  // vibe == 'auto' (or empty) -> no filter/boost.
  if (vibe.empty() || vibe == "auto") return cp;

  // Filter: p.vibe === vibe || (p.vibeScore && p.vibeScore[vibe] > 1.5).
  Json::Value filtered(Json::arrayValue);
  for (const auto& p : cp) {
    bool matchVibe = p.isObject() && p.isMember("vibe")
                     && p["vibe"].isString() && p["vibe"].asString() == vibe;
    bool matchScore = p.isObject() && p.isMember("vibeScore")
                      && p["vibeScore"].isObject()
                      && p["vibeScore"].isMember(vibe)
                      && p["vibeScore"][vibe].isNumeric()
                      && p["vibeScore"][vibe].asDouble() > 1.5;
    if (matchVibe || matchScore) filtered.append(p);
  }

  // boostByVibe(filtered, vibe, 1.5): matching posts get _score *= 1.5 and
  // _vibeMatch = true. (_score is undefined pre-rank -> (post._score || 0).)
  Json::Value boosted(Json::arrayValue);
  for (const auto& src : filtered) {
    Json::Value p = src;
    bool matchesVibe =
        (p.isObject() && p.isMember("vibe") && p["vibe"].isString()
         && p["vibe"].asString() == vibe)
        || (p.isObject() && p.isMember("vibeScore") && p["vibeScore"].isObject()
            && p["vibeScore"].isMember(vibe) && p["vibeScore"][vibe].isNumeric()
            && p["vibeScore"][vibe].asDouble() > 1.0);
    if (matchesVibe) {
      double prev = (p.isMember("_score") && p["_score"].isNumeric())
                        ? p["_score"].asDouble() : 0.0;
      p["_score"]     = prev * 1.5;
      p["_vibeMatch"] = true;
    }
    boosted.append(p);
  }
  return boosted;
}

} // namespace

// ── Follow graph ────────────────────────────────────────────────────────────
FollowGraph getFollowGraph(const std::string& userId) {
  const std::string key = "followgraph:" + userId;

  // getOrSet: cache-aside under followgraph:{userId}, TTL 60s.
  std::string cached = pulse::cache().getOrSet(
      key,
      [&]() -> std::string {
        // Promise.all([ Follow.getFollowingIds, Follow.getFollowerIds ])
        std::vector<std::string> followingIds =
            pulse::models::follow::getFollowingIds(userId);
        std::vector<std::string> followerIds =
            pulse::models::follow::getFollowerIds(userId);

        // friends = followingIds ∩ followerIds (mutual follows).
        std::unordered_set<std::string> followers(followerIds.begin(),
                                                  followerIds.end());
        std::vector<std::string> friendIds;
        for (const auto& id : followingIds)
          if (followers.count(id)) friendIds.push_back(id);

        Json::Value out(Json::objectValue);
        out["followingIds"] = toJsonArray(followingIds);
        out["friendIds"]    = toJsonArray(friendIds);
        return dumpJson(out);
      },
      kFollowGraphTtl);

  FollowGraph g;
  Json::Value j = parseJson(cached);
  if (j.isObject()) {
    for (const auto& x : j["followingIds"])
      if (x.isString()) g.followingIds.push_back(x.asString());
    for (const auto& x : j["friendIds"])
      if (x.isString()) g.friendIds.push_back(x.asString());
  }
  return g;
}

// ── Candidate generation ──────────────────────────────────────────────────
Json::Value getCandidateSet(const std::string& kind) {
  const std::string key = "feed:candidate:" + kind;

  std::string cached = pulse::cache().getOrSet(
      key,
      [&]() -> std::string {
        // Post.find({ isActive:true, visibility:'public' })
        //   .sort({ createdAt:-1 }).limit(200)
        //   .populate('author', 'username name avatar profile isVerified stats')
        auto filter = make_document(
            kvp("isActive", true),
            kvp("visibility", "public"));
        mongocxx::options::find opts{};
        opts.sort(make_document(kvp("createdAt", -1)));
        opts.limit(200);
        Json::Value posts = runPostFind(filter.view(), std::move(opts));
        populateAuthors(posts);
        return dumpJson(posts);
      },
      kCandidateTtl);

  Json::Value posts = parseJson(cached);
  if (!posts.isArray()) posts = Json::Value(Json::arrayValue);
  return posts;
}

Json::Value getForYouCandidates(const std::string& userId) {
  // 1. Shared fresh set: getCandidateSet('foryou').
  Json::Value fresh = getCandidateSet("foryou");

  try {
    // 2. Build the user taste vector. Cold start (null) -> recency pool.
    Json::Value userVec = pulse::userVector().getUserVector(userId);
    if (!userVec.isArray() || userVec.empty()) return fresh;

    // 3. Retrieve vector-similar posts:
    //    retrieveCandidates({ Post, userVec, filter:{author:{$ne:userId}},
    //                         limit: RETRIEVE_LIMIT }).
    Json::Value filter(Json::objectValue);
    filter["author"] = Json::Value(Json::objectValue);
    filter["author"]["$ne"] = userId;
    Json::Value retrieved =
        pulse::services::vector_retrieval::retrieveCandidates(
            userVec, filter, kRetrieveLimit);
    if (!retrieved.isArray() || retrieved.empty()) return fresh;

    // 4. Merge retrieved (relevance) + fresh (novelty/recency), de-duped by _id.
    std::set<std::string> seen;
    Json::Value merged(Json::arrayValue);
    for (const auto& p : retrieved) {
      std::string id = postId(p);
      if (!id.empty() && seen.insert(id).second) merged.append(p);
    }
    for (const auto& p : fresh) {
      std::string id = postId(p);
      if (!id.empty() && seen.insert(id).second) merged.append(p);
    }
    return merged;
  } catch (const std::exception& e) {
    // Best-effort: degrade to the shared candidate set on any error.
    pulse::log::warn("[feed] retrieval failed, using shared candidate set: {}",
                     e.what());
    return fresh;
  }
}

// ── Ranking ────────────────────────────────────────────────────────────────
Json::Value rankPosts(const Json::Value& posts,
                      const std::string& userId,
                      const RankOptions& options) {
  // 1. No posts -> [].
  if (!posts.isArray() || posts.empty())
    return Json::Value(Json::arrayValue);

  const long long now = nowMillis();

  // Collect post + author ids for batch signal fetches.
  std::vector<std::string> postIds;
  std::vector<std::string> authorIds;
  postIds.reserve(posts.size());
  {
    std::unordered_set<std::string> authorSeen;
    for (const auto& p : posts) {
      std::string pid = postId(p);
      if (!pid.empty()) postIds.push_back(pid);
      std::string aid = authorId(p);
      if (!aid.empty() && authorSeen.insert(aid).second)
        authorIds.push_back(aid);
    }
  }

  // 3a. UserBehavior-derived signals: seenPostIds (24h), isColdStart,
  //     sessionDepth. Mirrors feedAlgo.js:
  //       userBehavior = await UserBehavior.getPreferences(userId);
  //       sessionDepth = userBehavior.sessionDepth || 0;
  //       seenPostIds  = await UserBehavior.getSeenPostIds(userId, 24);
  //       isColdStart  = (userBehavior.totalInteractions || 0) < 30;
  //     catch (_) { isColdStart = true; }   // and no user -> true.
  // (COLD_START.MIN_INTERACTIONS = 30. getPreferences omits totalInteractions,
  //  so for an authenticated user the (undefined || 0) < 30 test is true —
  //  preserved verbatim, not "fixed".)
  Json::Value seenPostIds(Json::arrayValue);
  bool isColdStart = false;
  bool haveUserBehavior = false;
  int sessionDepth = 0;
  if (!userId.empty()) {
    try {
      Json::Value prefs = pulse::models::userbehavior::getPreferences(userId);
      haveUserBehavior = true;
      if (prefs.isObject() && prefs.isMember("sessionDepth")
          && prefs["sessionDepth"].isNumeric())
        sessionDepth = prefs["sessionDepth"].asInt();

      std::set<std::string> seen =
          pulse::models::userbehavior::getSeenPostIds(userId, 24.0);
      for (const auto& id : seen) seenPostIds.append(id);

      double totalInteractions =
          (prefs.isObject() && prefs.isMember("totalInteractions")
           && prefs["totalInteractions"].isNumeric())
              ? prefs["totalInteractions"].asDouble()
              : 0.0;
      isColdStart = totalInteractions < 30.0;  // COLD_START.MIN_INTERACTIONS
    } catch (const std::exception& e) {
      pulse::log::warn("feed rank: behavior signals failed: {}", e.what());
      isColdStart = true;  // catch (_) { isColdStart = true; }
    }
  } else {
    isColdStart = true;  // no authenticated user -> cold start
  }

  // 3b. Author affinities (user engagement with each candidate's author).
  Json::Value affinityMap(Json::objectValue);
  if (!userId.empty() && !authorIds.empty()) {
    try {
      std::map<std::string, double> aff =
          pulse::models::userengagement::getBatchAffinities(userId, authorIds);
      for (const auto& kv : aff) affinityMap[kv.first] = kv.second;
    } catch (const std::exception& e) {
      pulse::log::warn("feed rank: affinity fetch failed: {}", e.what());
    }
  }

  // 3c. Velocity map (likes per VELOCITY_WINDOW_HOURS per post).
  Json::Value velocityMap(Json::objectValue);
  if (options.includeVelocity && !postIds.empty()) {
    try {
      std::map<std::string, double> vel =
          pulse::models::like::getBatchLikeVelocities(
              pulse::models::like::kTargetTypePost, postIds,
              kVelocityWindowHours);
      for (const auto& kv : vel) velocityMap[kv.first] = kv.second;
    } catch (const std::exception& e) {
      pulse::log::warn("feed rank: velocity fetch failed: {}", e.what());
    }
  }

  // 3d. Friend likes (social proof): for each candidate, how many of the user's
  //     friends liked it. friendIds ∩ likers(post).
  Json::Value friendLikes(Json::objectValue);
  if (!options.friendIds.empty() && !postIds.empty()) {
    try {
      // Build ObjectId arrays for the aggregation.
      bld::array userIn;
      for (const auto& f : options.friendIds)
        if (auto o = pulse::bsonjson::tryOid(f)) userIn.append(*o);
      bld::array targetIn;
      for (const auto& pid : postIds)
        if (auto o = pulse::bsonjson::tryOid(pid)) targetIn.append(*o);

      auto col = pulse::db::collection(pulse::models::like::kCollection);
      mongocxx::pipeline pipe{};
      pipe.match(make_document(
          kvp("targetType", pulse::models::like::kTargetTypePost),
          kvp("user", make_document(kvp("$in", userIn.extract()))),
          kvp("targetId", make_document(kvp("$in", targetIn.extract())))));
      pipe.group(make_document(
          kvp("_id", "$targetId"),
          kvp("count", make_document(kvp("$sum", 1)))));
      auto cursor = col.aggregate(pipe);
      for (const auto& doc : cursor) {
        auto v = doc["_id"];
        std::string pid;
        if (v && v.type() == bsoncxx::type::k_oid)
          pid = pulse::bsonjson::oidToHex(v.get_oid().value);
        long long count = 0;
        auto cv = doc["count"];
        if (cv) {
          if (cv.type() == bsoncxx::type::k_int32) count = cv.get_int32().value;
          else if (cv.type() == bsoncxx::type::k_int64) count = cv.get_int64().value;
          else if (cv.type() == bsoncxx::type::k_double) count = (long long)cv.get_double().value;
        }
        if (!pid.empty()) friendLikes[pid] = static_cast<Json::Int64>(count);
      }
    } catch (const std::exception& e) {
      pulse::log::warn("feed rank: friend likes failed: {}", e.what());
    }
  }

  // 3e. Per-post relevance via InterestProfiler.batchScorePosts(posts, userId).
  //     Mirrors the JS wrapper exactly:
  //       prefs   = getPreferences(userId); seenIds = getSeenPostIds(userId,24);
  //       payload = { posts, prefs: normalizePrefs(prefs) };
  //       scored  = interestScore(payload)  -> [{ postId, relevance }];
  //       for each post: rel = byId.get(id) ?? 1.0;
  //                      if (seenIds.has(id)) rel *= SEEN_PENALTY (0.7);
  //                      relevanceMap[id] = rel (-> feedAlgo reads _relevanceScore)
  //     Only when (userId && userBehavior) — same gate as feedAlgo.js.
  Json::Value relevanceMap(Json::objectValue);
  if (!userId.empty() && haveUserBehavior) {
    try {
      Json::Value prefs = pulse::models::userbehavior::getPreferences(userId);

      // normalizePrefs: { topics, mediaTypes, postLengths, authorAffinities }.
      Json::Value np(Json::objectValue);
      np["topics"]      = (prefs.isObject() && prefs["topics"].isObject())
                              ? prefs["topics"] : Json::Value(Json::objectValue);
      np["mediaTypes"]  = (prefs.isObject() && prefs["mediaTypes"].isObject())
                              ? prefs["mediaTypes"] : Json::Value(Json::objectValue);
      np["postLengths"] = (prefs.isObject() && prefs["postLengths"].isObject())
                              ? prefs["postLengths"] : Json::Value(Json::objectValue);
      // authorAffinities: present-and-non-empty -> the map, else null.
      if (prefs.isObject() && prefs["authorAffinities"].isObject()
          && !prefs["authorAffinities"].empty())
        np["authorAffinities"] = prefs["authorAffinities"];
      else
        np["authorAffinities"] = Json::Value(Json::nullValue);

      Json::Value ipInput(Json::objectValue);
      ipInput["posts"] = posts;
      ipInput["prefs"] = np;

      std::string ipOut = pulse::algos::interestScore(dumpJson(ipInput));
      Json::Value scored = parseJson(ipOut);

      // byId: postId -> relevance.
      std::map<std::string, double> byId;
      if (scored.isArray()) {
        for (const auto& s : scored) {
          if (s.isObject() && s.isMember("postId") && s.isMember("relevance")) {
            std::string pid = s["postId"].isString() ? s["postId"].asString() : "";
            if (!pid.empty()) byId[pid] = s["relevance"].asDouble();
          }
        }
      }

      // Seen set (within 24h) for the SEEN_PENALTY overlay.
      std::set<std::string> seenIds;
      for (const auto& id : seenPostIds)
        if (id.isString()) seenIds.insert(id.asString());

      constexpr double kSeenPenalty = 0.7;  // CONFIG.FRESHNESS.SEEN_PENALTY
      for (const auto& p : posts) {
        std::string id = postId(p);
        if (id.empty()) continue;
        auto it = byId.find(id);
        double rel = (it != byId.end()) ? it->second : 1.0;  // ?? 1.0
        if (seenIds.count(id)) rel *= kSeenPenalty;
        relevanceMap[id] = rel;
      }
    } catch (const std::exception& e) {
      pulse::log::warn("feed rank: relevance fetch failed: {}", e.what());
    }
  }

  // 3f. Integrity signals: author trust (cached) + per-post engagement-bait.
  //     ({ trustMap, baitMap } = await trustService.buildSignals(posts));
  //     best-effort — on error the maps stay empty (kernel reads a missing entry
  //     as "clean": trust 1.0 / bait 1.0), exactly like the JS try/catch.
  Json::Value trustMap(Json::objectValue);
  Json::Value baitMap(Json::objectValue);
  try {
    pulse::TrustService::Signals sig = pulse::trust().buildSignals(posts);
    if (sig.trustMap.isObject()) trustMap = sig.trustMap;
    if (sig.baitMap.isObject())  baitMap  = sig.baitMap;
  } catch (const std::exception& e) {
    pulse::log::warn("feed rank: trust signals failed: {}", e.what());
  }

  // 4. Build the feedRank payload. createdAt is normalized to epoch millis
  //    (msFields(posts, ['createdAt'])) so the kernel's time-decay math matches.
  Json::Value postsMs(Json::arrayValue);
  for (const auto& p : posts) {
    Json::Value q = p;
    q["createdAt"] = static_cast<Json::Int64>(createdAtMillis(p, now));
    postsMs.append(q);
  }

  Json::Value payload(Json::objectValue);
  payload["posts"]            = postsMs;
  payload["userId"]           = userId.empty() ? Json::Value(Json::nullValue)
                                               : Json::Value(userId);
  payload["nowMs"]            = static_cast<Json::Int64>(now);
  payload["trustMap"]         = trustMap;
  payload["baitMap"]          = baitMap;
  payload["followingIds"]     = toJsonArray(options.followingIds);
  payload["mutualIds"]        = toJsonArray(options.mutualIds);
  payload["friendIds"]        = toJsonArray(options.friendIds);
  payload["trendingHashtags"] = toJsonArray(options.trendingHashtags);
  payload["velocityMap"]      = velocityMap;
  payload["affinityMap"]      = affinityMap;
  payload["friendLikes"]      = friendLikes;
  payload["relevanceMap"]     = relevanceMap;
  payload["seenPostIds"]      = seenPostIds;
  payload["isColdStart"]      = isColdStart;
  payload["sessionDepth"]     = sessionDepth;
  // NOTE: feedAlgo.js never sets a top-level `vibe` in the feedRank payload —
  // vibe handling happens BEFORE ranking (rankPostsWithVibe classifies / filters
  // / boosts, then calls plain rankPosts). The kernel reads each post's own
  // `vibe` field for diversity. RankOptions::vibe is therefore intentionally not
  // forwarded into the payload here.

  // 5. Call native addon: JSON.parse(feedRank(JSON.stringify(payload))).
  try {
    std::string ranked = pulse::algos::feedRank(dumpJson(payload));
    Json::Value out = parseJson(ranked);
    if (out.isArray()) return out;
  } catch (const std::exception& e) {
    pulse::log::error("feedRank failed, falling back to input order: {}", e.what());
  }

  // 6. Fall back to the input order on error.
  return posts;
}

// ── Post post-processing ─────────────────────────────────────────────────
Json::Value processPosts(const Json::Value& posts, const std::string& userId) {
  if (!posts.isArray()) return Json::Value(Json::arrayValue);

  // 1. Extract post ids.
  std::vector<std::string> postIds;
  postIds.reserve(posts.size());
  for (const auto& p : posts) {
    std::string pid = postId(p);
    if (!pid.empty()) postIds.push_back(pid);
  }

  // 2. Fetch the user's likes for these posts.
  std::set<std::string> likedSet;
  if (!userId.empty() && !postIds.empty()) {
    try {
      likedSet = pulse::models::like::getLikedIds(
          userId, pulse::models::like::kTargetTypePost, postIds);
    } catch (const std::exception& e) {
      pulse::log::warn("processPosts: getLikedIds failed: {}", e.what());
    }
  }

  // 3. Per-post: mask anonymous author, ensure stats.likes, set isLiked, drop
  //    ranking signals.
  Json::Value out(Json::arrayValue);
  for (const auto& src : posts) {
    Json::Value p = src;
    std::string pid = postId(p);

    if (p.isMember("isAnonymous") && p["isAnonymous"].asBool()) {
      Json::Value anon(Json::objectValue);
      anon["_id"]        = Json::Value(Json::nullValue);
      anon["username"]   = "anonymous";
      anon["name"]       = "Anonymous";
      anon["avatar"]     = "https://res.cloudinary.com/pulse/image/upload/v1/defaults/anonymous-avatar.png";
      anon["isVerified"] = false;
      p["author"] = anon;
    }

    // if (!postObj.stats) postObj.stats = {};
    // postObj.stats.likes = postObj.stats.likes || 0;
    if (!p.isMember("stats") || !p["stats"].isObject())
      p["stats"] = Json::Value(Json::objectValue);
    if (!p["stats"].isMember("likes") || !p["stats"]["likes"].isNumeric())
      p["stats"]["likes"] = 0;

    p["isLiked"] = likedSet.count(pid) > 0;

    // Don't leak ranking signals to clients (set to undefined -> dropped).
    p.removeMember("_score");
    p.removeMember("_velocity");
    p.removeMember("_engagementScore");

    out.append(p);
  }
  return out;
}

// ── Feed builders ─────────────────────────────────────────────────────────
Json::Value getHomeFeed(const std::string& userId,
                        const PageParams& page,
                        const std::string& vibeIn) {
  int limit = clampInt(page.limit, 1, 50);
  int pageN = clampInt(page.page, 1, 50);

  // const vibe = req.query.vibe || 'auto';
  const std::string vibe = vibeIn.empty() ? "auto" : vibeIn;

  FollowGraph graph = getFollowGraph(userId);

  // Bounded candidate fetch: newest min(limit*5, 100) public-or-followers posts.
  //   $or: [ { visibility:'public' },
  //          { visibility:'followers', author:{$in:[...followingIds, userId]} } ]
  bld::array authorIn;
  for (const auto& id : graph.followingIds)
    if (auto o = pulse::bsonjson::tryOid(id)) authorIn.append(*o);
  if (auto o = pulse::bsonjson::tryOid(userId)) authorIn.append(*o);

  bld::array orArr;
  orArr.append(make_document(kvp("visibility", "public")));
  orArr.append(make_document(
      kvp("visibility", "followers"),
      kvp("author", make_document(kvp("$in", authorIn.extract())))));

  auto filter = make_document(
      kvp("isActive", true),
      kvp("$or", orArr.extract()));

  int fetchLimit = std::min(limit * 5, 100);
  mongocxx::options::find opts{};
  opts.sort(make_document(kvp("createdAt", -1)));
  opts.limit(fetchLimit);

  Json::Value candidates = runPostFind(filter.view(), std::move(opts));
  // .populate('author', 'username name avatar profile isVerified stats')
  populateAuthors(candidates);

  // Rank: rankPostsWithVibe(posts, userId, { followingIds, friendIds,
  //                                          includeVelocity:true, vibe }).
  // rankPostsWithVibe first classifies / filters / boosts by vibe (applyVibe),
  // then runs plain rankPosts (the vibe is NOT forwarded into the feedRank
  // payload — see rankPosts).
  Json::Value vibeCandidates = applyVibe(candidates, vibe);

  RankOptions ro;
  ro.followingIds    = graph.followingIds;
  ro.friendIds       = graph.friendIds;
  ro.includeVelocity = true;
  Json::Value ranked = rankPosts(vibeCandidates, userId, ro);

  // Paginate: slice(startIndex, startIndex + limit).
  int startIndex = (pageN - 1) * limit;
  Json::Value pagePosts(Json::arrayValue);
  if (ranked.isArray()) {
    for (int i = startIndex; i < (int)ranked.size() && i < startIndex + limit; ++i)
      pagePosts.append(ranked[i]);
  }
  bool hasMore = ranked.isArray() && (startIndex + limit) < (int)ranked.size();

  Json::Value processed = processPosts(pagePosts, userId);

  // res.json({ success:true, data, pagination:{page,limit,hasMore}, vibe }).
  Json::Value resp(Json::objectValue);
  resp["success"] = true;
  resp["data"]    = processed;
  Json::Value pg(Json::objectValue);
  pg["page"]    = pageN;
  pg["limit"]   = limit;
  pg["hasMore"] = hasMore;
  resp["pagination"] = pg;
  resp["vibe"]  = vibe;
  return resp;
}

Json::Value getFollowingFeed(const std::string& userId,
                             const PageParams& page,
                             const std::string& before) {
  int limit = clampInt(page.limit, 1, 50);
  int pageN = clampInt(page.page, 1, 50);

  FollowGraph graph = getFollowGraph(userId);

  // No followingIds -> empty array.
  //   res.json({ success:true, data:[],
  //     pagination:{ page:1, limit, hasMore:false, feedType:'following' } }).
  if (graph.followingIds.empty()) {
    Json::Value resp(Json::objectValue);
    resp["success"] = true;
    resp["data"]    = Json::Value(Json::arrayValue);
    Json::Value pg(Json::objectValue);
    pg["page"]     = 1;
    pg["limit"]    = limit;
    pg["hasMore"]  = false;
    pg["feedType"] = "following";
    resp["pagination"] = pg;
    return resp;
  }

  // Query: author:{$in:followingIds}, isActive:true,
  //        visibility:{$in:['public','followers']},
  //        createdAt:{$lt:before} (when provided and valid).
  bld::array authorIn;
  for (const auto& id : graph.followingIds)
    if (auto o = pulse::bsonjson::tryOid(id)) authorIn.append(*o);

  bld::document filter;
  filter.append(kvp("author", make_document(kvp("$in", authorIn.extract()))));
  filter.append(kvp("isActive", true));
  filter.append(kvp("visibility",
                    make_document(kvp("$in", make_array("public", "followers")))));
  if (auto ms = parseDateMillis(before))
    filter.append(kvp("createdAt", make_document(kvp("$lt", dateFromMillis(*ms)))));

  // Fetch limit+1 to compute hasMore.
  mongocxx::options::find opts{};
  opts.sort(make_document(kvp("createdAt", -1)));
  opts.limit(limit + 1);

  Json::Value fetched = runPostFind(filter.extract(), std::move(opts));
  // .populate('author', 'username name avatar profile isVerified stats')
  populateAuthors(fetched);

  bool hasMore = fetched.isArray() && (int)fetched.size() > limit;

  Json::Value sliced(Json::arrayValue);
  if (fetched.isArray()) {
    int n = hasMore ? limit : (int)fetched.size();
    for (int i = 0; i < n; ++i) sliced.append(fetched[i]);
  }

  // nextCursor = last post's createdAt.
  Json::Value nextCursor(Json::nullValue);
  if (sliced.isArray() && sliced.size() > 0) {
    const Json::Value& last = sliced[sliced.size() - 1];
    if (last.isObject() && last.isMember("createdAt"))
      nextCursor = last["createdAt"];
  }

  Json::Value processed = processPosts(sliced, userId);

  // res.json({ success:true, data,
  //   pagination:{ page, limit, hasMore, nextCursor, feedType:'following' } }).
  Json::Value resp(Json::objectValue);
  resp["success"] = true;
  resp["data"]    = processed;
  Json::Value pg(Json::objectValue);
  pg["page"]       = pageN;
  pg["limit"]      = limit;
  pg["hasMore"]    = hasMore;
  pg["nextCursor"] = nextCursor;
  pg["feedType"]   = "following";
  resp["pagination"] = pg;
  return resp;
}

Json::Value getForYouFeed(const std::string& userId, const PageParams& page) {
  int limit = clampInt(page.limit, 1, 50);
  int pageN = clampInt(page.page, 1, 50);

  FollowGraph graph = getFollowGraph(userId);

  Json::Value candidates = getForYouCandidates(userId);

  // feedAlgo.getForYouFeed first drops the caller's own posts:
  //   const filtered = posts.filter(p => aid(p) !== userId?.toString());
  Json::Value filtered(Json::arrayValue);
  if (candidates.isArray()) {
    for (const auto& p : candidates)
      if (authorId(p) != userId) filtered.append(p);
  }

  // Rank: getForYouFeed(userId, posts, { followingIds, includeVelocity:true }).
  RankOptions ro;
  ro.followingIds    = graph.followingIds;
  ro.includeVelocity = true;
  Json::Value ranked = rankPosts(filtered, userId, ro);

  int startIndex = (pageN - 1) * limit;
  Json::Value pagePosts(Json::arrayValue);
  if (ranked.isArray()) {
    for (int i = startIndex; i < (int)ranked.size() && i < startIndex + limit; ++i)
      pagePosts.append(ranked[i]);
  }
  bool hasMore = ranked.isArray() && (startIndex + limit) < (int)ranked.size();

  Json::Value processed = processPosts(pagePosts, userId);

  // res.json({ success:true, data,
  //   pagination:{ page, limit, hasMore, feedType:'foryou' } }).
  Json::Value resp(Json::objectValue);
  resp["success"] = true;
  resp["data"]    = processed;
  Json::Value pg(Json::objectValue);
  pg["page"]     = pageN;
  pg["limit"]    = limit;
  pg["hasMore"]  = hasMore;
  pg["feedType"] = "foryou";
  resp["pagination"] = pg;
  return resp;
}

Json::Value getGlobalFeed(const std::string& userId, const PageParams& page) {
  int limit = clampInt(page.limit, 1, 50);
  int pageN = clampInt(page.page, 1, 50);

  FollowGraph graph = getFollowGraph(userId);

  // Shared candidate set: getCandidateSet('global').
  Json::Value candidates = getCandidateSet("global");

  // Rank: rankPosts(posts, userId, { followingIds, friendIds,
  //                                  includeVelocity:true }).
  RankOptions ro;
  ro.followingIds    = graph.followingIds;
  ro.friendIds       = graph.friendIds;
  ro.includeVelocity = true;
  Json::Value ranked = rankPosts(candidates, userId, ro);

  int startIndex = (pageN - 1) * limit;
  Json::Value pagePosts(Json::arrayValue);
  if (ranked.isArray()) {
    for (int i = startIndex; i < (int)ranked.size() && i < startIndex + limit; ++i)
      pagePosts.append(ranked[i]);
  }
  bool hasMore = ranked.isArray() && (startIndex + limit) < (int)ranked.size();

  Json::Value processed = processPosts(pagePosts, userId);

  // res.json({ success:true, data, pagination:{ page, limit, hasMore } }).
  Json::Value resp(Json::objectValue);
  resp["success"] = true;
  resp["data"]    = processed;
  Json::Value pg(Json::objectValue);
  pg["page"]    = pageN;
  pg["limit"]   = limit;
  pg["hasMore"] = hasMore;
  resp["pagination"] = pg;
  return resp;
}

Json::Value getTrendingPosts(const std::string& userId,
                             int limit,
                             int timeRangeHours) {
  int lim = clampInt(limit, 1, 50);
  int timeRange = clampInt(timeRangeHours, 1, 168);

  // Cached trending candidates under feed:candidate:trending:{timeRange}.
  const std::string key = "feed:candidate:trending:" + std::to_string(timeRange);
  std::string cached = pulse::cache().getOrSet(
      key,
      [&]() -> std::string {
        // cutoff = new Date(Date.now() - timeRange*60*60*1000)
        long long cutoff = nowMillis()
            - static_cast<long long>(timeRange) * 60LL * 60LL * 1000LL;
        // Post.find({ isActive:true, visibility:'public',
        //             createdAt:{$gte:cutoff} })
        //   .sort({ 'stats.likes':-1 }).limit(100)
        //   .populate('author', 'username name avatar profile isVerified')  // no stats
        auto filter = make_document(
            kvp("isActive", true),
            kvp("visibility", "public"),
            kvp("createdAt", make_document(kvp("$gte", dateFromMillis(cutoff)))));
        mongocxx::options::find opts{};
        opts.sort(make_document(kvp("stats.likes", -1)));
        opts.limit(100);
        Json::Value posts = runPostFind(filter.view(), std::move(opts));
        populateAuthors(posts, /*includeStats=*/false);
        return dumpJson(posts);
      },
      kCandidateTtl);

  Json::Value candidates = parseJson(cached);
  if (!candidates.isArray()) candidates = Json::Value(Json::arrayValue);

  // Rank: feedAlgo.getTrendingPosts(posts, { timeRange, limit }) — this is the
  // LIGHT pure-JS path (one batch velocity query + sort), NOT the native
  // feedRank pipeline. Ported verbatim from _fallback/feedAlgo.getTrendingPosts:
  //   cutoff  = now - timeRange*3600000
  //   recent  = posts.filter(p => new Date(p.createdAt) >= cutoff)
  //   if recent.empty -> []
  //   velocityMap = Like.getBatchLikeVelocities('post', recent ids, 0.5)
  //   wv = recent.map(p => ({ ...p,
  //          _velocity: (velocityMap[id] || 0) * VELOCITY_WEIGHT(3.0),
  //          _engagementScore: calculatePostScore(p) }))
  //   wv.sort((a,b) => (b._velocity - a._velocity) || (b._engagementScore - a._engagementScore))
  //   wv.slice(0, limit)
  const long long now = nowMillis();
  const long long cutoff =
      now - static_cast<long long>(timeRange) * 3600000LL;

  std::vector<Json::Value> recent;
  std::vector<std::string> recentIds;
  for (const auto& p : candidates) {
    long long created = createdAtMillis(p, now);
    if (created >= cutoff) {
      recent.push_back(p);
      std::string id = postId(p);
      if (!id.empty()) recentIds.push_back(id);
    }
  }

  Json::Value limited(Json::arrayValue);
  if (!recent.empty()) {
    std::map<std::string, double> velocityMap;
    try {
      velocityMap = pulse::models::like::getBatchLikeVelocities(
          pulse::models::like::kTargetTypePost, recentIds,
          kVelocityWindowHours);  // CONFIG.VELOCITY_WINDOW_HOURS = 0.5
    } catch (const std::exception& e) {
      pulse::log::warn("trending: velocity fetch failed: {}", e.what());
    }

    struct WV { Json::Value post; double velocity; double engagement; };
    std::vector<WV> wv;
    wv.reserve(recent.size());
    for (auto& p : recent) {
      std::string id = postId(p);
      double v = 0.0;
      auto it = velocityMap.find(id);
      if (it != velocityMap.end()) v = it->second;
      double velocity = v * 3.0;  // CONFIG.VELOCITY_WEIGHT = 3.0
      double engagement = calculatePostScore(p);
      Json::Value q = p;
      q["_velocity"]         = velocity;
      q["_engagementScore"]  = engagement;
      wv.push_back({q, velocity, engagement});
    }

    // sort by _velocity desc, tie-break _engagementScore desc (stable to match
    // JS Array.sort stability for equal keys).
    std::stable_sort(wv.begin(), wv.end(), [](const WV& a, const WV& b) {
      if (a.velocity != b.velocity) return a.velocity > b.velocity;
      return a.engagement > b.engagement;
    });

    for (int i = 0; i < (int)wv.size() && i < lim; ++i)
      limited.append(wv[i].post);
  }

  // processPosts(trendingPosts, userId) — like status uses the REAL caller.
  Json::Value processed = processPosts(limited, userId);

  // res.json({ success:true, data }).
  Json::Value resp(Json::objectValue);
  resp["success"] = true;
  resp["data"]    = processed;
  return resp;
}

Json::Value getNearbyPosts(const std::string& userId,
                           double longitude,
                           double latitude,
                           double maxDistance,
                           int limit) {
  int lim = clampInt(limit, 1, 50);
  // maxDistance clamped to 50km (50000 m).
  double dist = std::min(maxDistance, 50000.0);

  // Post.getNearbyPosts([lon, lat], maxDistance, { limit }).
  //   The static .populate('author', 'username name avatar profile isVerified')
  //   (no stats) is a controller-layer join here — applied after the geo query.
  pulse::models::post::NearbyOptions opts;
  opts.limit = lim;
  Json::Value nearby = pulse::models::post::getNearbyPosts(
      {longitude, latitude}, dist, opts);
  populateAuthors(nearby, /*includeStats=*/false);

  Json::Value processed = processPosts(nearby, userId);

  // res.json({ success:true, data }).
  Json::Value resp(Json::objectValue);
  resp["success"] = true;
  resp["data"]    = processed;
  return resp;
}

} // namespace pulse::services::feed
