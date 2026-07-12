// user_vector_service.cc — implementation. Ports src/services/userVectorService.js.
//
// Builds (and caches) a user's taste vector for retrieval. The vector lives in
// the SAME 34-dim space as post embeddings, so cosine(userVec, postVec) ≈
// predicted affinity. Assembled from recently-liked-post embeddings, SocialDNA
// vibe strands, and UserBehavior topic affinities, then cached in Redis under
// `uservec:${userId}` (USER_VECTOR_TTL_SEC, default 300s).
//
// The JS `deps` arg ({ Like, Post, SocialDNA, UserBehavior }) was purely for test
// injection; here the concrete model namespaces are called directly.
#include "pulse/services/user_vector_service.hpp"

#include "pulse/services/embedding_service.hpp"
#include "pulse/cache.hpp"
#include "pulse/config.hpp"
#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"

#include "pulse/models/like.hpp"
#include "pulse/models/post.hpp"
#include "pulse/models/socialdna.hpp"
#include "pulse/models/userbehavior.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/oid.hpp>

#include <mongocxx/collection.hpp>
#include <mongocxx/cursor.hpp>
#include <mongocxx/options/find.hpp>

#include <string>
#include <vector>
#include <memory>

namespace pulse {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;

namespace {

// Compact JSON serialization (no indentation), matching how cacheService.js
// JSON.stringify's the value before SETEX.
std::string dumpCompact(const Json::Value& v) {
  Json::StreamWriterBuilder w;
  w["indentation"] = "";
  return Json::writeString(w, v);
}

// Serialize a std::vector<double> as a JSON array of numbers.
Json::Value vecToJson(const std::vector<double>& v) {
  Json::Value arr(Json::arrayValue);
  for (double x : v) arr.append(x);
  return arr;
}

} // namespace

UserVectorService::UserVectorService() {
  // TTL = parseInt(process.env.USER_VECTOR_TTL_SEC, 10) || 300
  ttl_ = config().envInt("USER_VECTOR_TTL_SEC", 300);
  if (ttl_ <= 0) ttl_ = 300;
}

UserVectorService& UserVectorService::instance() {
  static UserVectorService s;
  return s;
}

Json::Value UserVectorService::getUserVector(const std::string& userId) const {
  const std::string key = "uservec:" + userId;

  // try { const cached = await cacheService.get(key);
  //       if (cached && Array.isArray(cached) && cached.length === DIM) return cached; }
  // catch { /* fall through */ }
  try {
    auto cached = cache().get(key);
    if (cached && !cached->empty()) {
      Json::CharReaderBuilder rb;
      Json::Value parsed;
      std::string errs;
      std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
      const char* begin = cached->c_str();
      const char* end = begin + cached->size();
      if (reader->parse(begin, end, &parsed, &errs) &&
          parsed.isArray() &&
          static_cast<int>(parsed.size()) == EmbeddingService::kDim) {
        return parsed;
      }
    }
  } catch (...) { /* fall through */ }

  Json::Value vec = buildUserVector(userId);
  if (!vec.isNull()) {
    // try { await cacheService.set(key, vec, TTL); } catch { /* best effort */ }
    try {
      cache().set(key, dumpCompact(vec), ttl_);
    } catch (...) { /* best effort */ }
  }
  return vec;
}

Json::Value UserVectorService::buildUserVector(const std::string& userId) const {
  // 1. Recently liked posts → their embeddings are the core taste signal.
  Json::Value likedPosts(Json::arrayValue);
  try {
    auto maybeUser = pulse::bsonjson::tryOid(userId);

    // Like.find({ user: userId, targetType: 'post' })
    //   .sort({ createdAt: -1 }).limit(LIKED_SAMPLE).select('targetId').lean()
    std::vector<bsoncxx::oid> ids;     // ObjectId targetIds (for the $in query)
    std::vector<std::string> idStrs;   // non-ObjectId targetIds (parity fallback)
    {
      auto likeCol = pulse::db::collection(pulse::models::like::kCollection);

      bld::document filter{};
      if (maybeUser) filter.append(kvp("user", *maybeUser));
      else           filter.append(kvp("user", userId));
      filter.append(kvp("targetType", pulse::models::like::kTargetTypePost));

      mongocxx::options::find opts{};
      opts.sort(make_document(kvp("createdAt", -1)));
      opts.limit(kLikedSample);
      opts.projection(make_document(kvp("targetId", 1)));

      auto cursor = likeCol.find(filter.view(), opts);
      for (auto&& d : cursor) {
        auto el = d["targetId"];
        if (el && el.type() == bsoncxx::type::k_oid) {
          ids.push_back(el.get_oid().value);
        } else if (el && el.type() == bsoncxx::type::k_string) {
          idStrs.emplace_back(el.get_string().value);
        }
      }
    }

    // if (ids.length) Post.find({ _id: { $in: ids } })
    //   .select('content vibe vibeScore stats createdAt embedding').lean()
    if (!ids.empty() || !idStrs.empty()) {
      auto postCol = pulse::db::collection(pulse::models::post::kCollection);

      bld::array idArray{};
      for (const auto& id : ids) idArray.append(id);
      for (const auto& s : idStrs) idArray.append(s);

      mongocxx::options::find opts{};
      // select('content vibe vibeScore stats createdAt embedding')
      opts.projection(make_document(
          kvp("content", 1),
          kvp("vibe", 1),
          kvp("vibeScore", 1),
          kvp("stats", 1),
          kvp("createdAt", 1),
          kvp("embedding", 1)));

      auto cursor = postCol.find(
          make_document(kvp("_id", make_document(kvp("$in", idArray)))), opts);
      for (auto&& d : cursor) {
        likedPosts.append(pulse::bsonjson::toJson(d));
      }
    }
  } catch (...) { /* optional */ }

  // 2. SocialDNA vibe strands.
  Json::Value vibeStrands = Json::nullValue;
  try {
    // SocialDNA.findOne({ user: userId }).select('strands').lean()
    auto maybeUser = pulse::bsonjson::tryOid(userId);
    auto dnaCol = pulse::db::collection(pulse::models::socialdna::kCollection);

    bld::document filter{};
    if (maybeUser) filter.append(kvp("user", *maybeUser));
    else           filter.append(kvp("user", userId));

    mongocxx::options::find opts{};
    opts.projection(make_document(kvp("strands", 1)));

    auto maybeDoc = dnaCol.find_one(filter.view(), opts);
    if (maybeDoc) {
      Json::Value dna = pulse::bsonjson::toJson(maybeDoc->view());
      // vibeStrands = dna?.strands || null
      if (dna.isObject() && dna.isMember("strands") && !dna["strands"].isNull()) {
        vibeStrands = dna["strands"];
      }
    }
  } catch (...) { /* optional */ }

  // 3. UserBehavior topic affinities.
  Json::Value topicAffinities = Json::nullValue;
  try {
    // const prefs = await UserBehavior.getPreferences(userId)
    // topicAffinities = prefs?.topics || null
    Json::Value prefs = pulse::models::userbehavior::getPreferences(userId);
    if (prefs.isObject() && prefs.isMember("topics") && !prefs["topics"].isNull()) {
      // JS truthiness: any object (even {}) is truthy -> kept as-is.
      topicAffinities = prefs["topics"];
    }
  } catch (...) { /* optional */ }

  // if (likedPosts.length === 0 && !vibeStrands && !topicAffinities) return null;
  if (likedPosts.empty() && vibeStrands.isNull() && topicAffinities.isNull()) {
    return Json::nullValue;
  }

  // return embeddingService.userVector({ engagedPosts: likedPosts, vibeStrands,
  //                                      topicAffinities });
  Json::Value args(Json::objectValue);
  args["engagedPosts"] = likedPosts;
  args["vibeStrands"] = vibeStrands;
  args["topicAffinities"] = topicAffinities;

  std::vector<double> vec = embedding().userVector(args);
  return vecToJson(vec);
}

void UserVectorService::invalidate(const std::string& userId) const {
  // return cacheService.del(`uservec:${userId}`).catch(() => {})
  try { cache().del("uservec:" + userId); } catch (...) { /* swallow */ }
}

Json::Value UserVectorService::seedFromOnboarding(
    const std::string& userId,
    const std::vector<std::string>& topics,
    const std::vector<std::string>& vibes) const {
  // const topicAffinities = {}; for (const t of topics) topicAffinities[t] = 1;
  Json::Value topicAffinities(Json::objectValue);
  for (const auto& t : topics) topicAffinities[t] = 1;

  // const vibeStrands = {}; for (const v of vibes) vibeStrands[v] = 100;
  Json::Value vibeStrands(Json::objectValue);
  for (const auto& v : vibes) vibeStrands[v] = 100;

  // embeddingService.userVector({
  //   engagedPosts: [],
  //   topicAffinities: Object.keys(topicAffinities).length ? topicAffinities : null,
  //   vibeStrands: Object.keys(vibeStrands).length ? vibeStrands : null,
  // })
  Json::Value args(Json::objectValue);
  args["engagedPosts"] = Json::Value(Json::arrayValue);
  args["topicAffinities"] =
      topicAffinities.empty() ? Json::Value(Json::nullValue) : topicAffinities;
  args["vibeStrands"] =
      vibeStrands.empty() ? Json::Value(Json::nullValue) : vibeStrands;

  std::vector<double> vec = embedding().userVector(args);
  Json::Value out = vecToJson(vec);

  // if (vec) { try { await cacheService.set(`uservec:${userId}`, vec, TTL); }
  //            catch { /* best effort */ } }
  // embeddingService.userVector always returns a vector, so this always caches.
  if (!out.isNull()) {
    try {
      cache().set("uservec:" + userId, dumpCompact(out), ttl_);
    } catch (...) { /* best effort */ }
  }
  return out;
}

} // namespace pulse
