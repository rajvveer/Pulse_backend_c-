// vector_retrieval_service.cc — C++ port of src/services/vectorRetrievalService.js.
// See vector_retrieval_service.hpp for the contract.
//
// Mirrors the JS module 1:1: the same auto backend selection (Atlas
// $vectorSearch vs in-process cosine), the same Mongo queries (filter / sort /
// limit / embedding existence guard), the same FALLBACK_WINDOW bound, the same
// numCandidates = max(limit*10, 200), and the same _vscore output field.
//
// Dependencies on the JS embeddingService:
//   * cosine(userVec, postVec) — pure dot product of two equal-length, already
//     L2-normalized vectors. Replicated locally here (identical math) so this
//     unit is self-contained; the embeddingService port exposes the same.
//   * featureVector(post) — used only by rankByCosine's `p.embedding ||
//     featureVector(p)` fallback. Declared as a forward dependency provided by
//     the embeddingService port (mirrors `require('./embeddingService')`).
#include "pulse/services/vector_retrieval_service.hpp"

#include "pulse/db.hpp"
#include "pulse/config.hpp"
#include "pulse/logger.hpp"
#include "pulse/bson_json.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>

#include <mongocxx/collection.hpp>
#include <mongocxx/cursor.hpp>
#include <mongocxx/pipeline.hpp>
#include <mongocxx/options/find.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// ── embeddingService forward dependency ───────────────────────────────────────
// featureVector lives in the embeddingService port (sibling unit, mirrors the
// JS `require('./embeddingService')`). It is only reached by rankByCosine's
// `p.embedding || featureVector(p)` fallback when a candidate carries no
// `embedding`. Declared here (not via a shared header) to keep this unit's
// public surface matching the JS module exactly; the embeddingService port
// provides the definition.
namespace pulse::services::embedding {
Json::Value featureVector(const Json::Value& post);
} // namespace pulse::services::embedding

namespace pulse::services::vector_retrieval {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;
using bld::make_array;

// Collection name from the Post model port ("posts"). Hardcoded here rather than
// including the model header so this service mirrors the JS, which received the
// `Post` model by reference and only ever issued find/aggregate against it.
namespace { constexpr const char* kPostCollection = "posts"; }

namespace {

// embeddingService.cosine(a, b): dot product of two equal-length, already
// normalized vectors. Returns 0 when either is missing/empty or lengths differ.
double cosine(const Json::Value& a, const Json::Value& b) {
  if (!a.isArray() || !b.isArray()) return 0.0;
  if (a.size() != b.size()) return 0.0;
  double dot = 0.0;
  for (Json::ArrayIndex i = 0; i < a.size(); ++i) {
    dot += a[i].asDouble() * b[i].asDouble();
  }
  return dot;  // both are unit vectors
}

// Convert a userVec Json::Value (array of numbers) into a bson array for the
// $vectorSearch queryVector.
bld::array userVecToBsonArray(const Json::Value& userVec) {
  bld::array arr;
  if (userVec.isArray()) {
    for (const auto& x : userVec) arr.append(x.asDouble());
  }
  return arr;
}

// Append every key/value of a JSON object filter onto a bson document builder.
// Mirrors the JS object spread `...filter`. Supports the scalar/array/object
// values a base Mongo filter carries (visibility/isActive/etc.). Done via the
// JSON->BSON converter so nested operators ($in, $ne, ...) survive intact.
void appendFilter(bld::document& doc, const Json::Value& filter) {
  if (!filter.isObject()) return;
  for (const auto& key : filter.getMemberNames()) {
    bsoncxx::document::value sub = pulse::bsonjson::fromJson(
        [&] { Json::Value w(Json::objectValue); w[key] = filter[key]; return w; }());
    // fromJson built a one-key doc; lift that key onto `doc`.
    for (const auto& el : sub.view()) {
      doc.append(kvp(el.key(), el.get_value()));
    }
  }
}

// Has the userVec any elements? (JS: `!userVec || !userVec.length`.)
bool hasVec(const Json::Value& userVec) {
  return userVec.isArray() && userVec.size() > 0;
}

// Run a find against `posts` with the given filter + options, return JSON array.
Json::Value runFind(const bsoncxx::document::view_or_value& filter,
                    mongocxx::options::find opts) {
  Json::Value out(Json::arrayValue);
  try {
    auto col = pulse::db::collection(kPostCollection);
    auto cursor = col.find(filter, opts);
    for (const auto& doc : cursor) {
      out.append(pulse::bsonjson::toJson(doc));
    }
  } catch (const std::exception& e) {
    pulse::log::error("[vectorRetrieval] post find failed: {}", e.what());
  }
  return out;
}

// statics.populate('author', 'username name avatar profile isVerified stats'):
// resolve each post's `author` ObjectId to the projected user fields. The JS
// .populate replaces the author id with that subdocument; we do the same join
// (aggregate/find returns the raw author id; here we hydrate it). Posts whose
// author cannot be resolved keep the original value (mirrors populate leaving an
// unresolved ref as-is when the referenced doc is gone).
void populateAuthors(Json::Value& posts) {
  if (!posts.isArray() || posts.empty()) return;

  // Collect distinct author ids.
  std::vector<std::string> ids;
  for (const auto& p : posts) {
    if (p.isMember("author")) {
      const Json::Value& a = p["author"];
      std::string hex;
      if (a.isString()) hex = a.asString();
      else if (a.isObject() && a.isMember("_id") && a["_id"].isString())
        hex = a["_id"].asString();
      if (!hex.empty() &&
          std::find(ids.begin(), ids.end(), hex) == ids.end())
        ids.push_back(hex);
    }
  }
  if (ids.empty()) return;

  // Fetch the projected author docs in one query (User.populate select:
  // 'username name avatar profile isVerified stats').
  Json::Value byId(Json::objectValue);
  try {
    bld::array in;
    for (const auto& hex : ids) {
      if (auto o = pulse::bsonjson::tryOid(hex)) in.append(*o);
    }
    auto filter = make_document(kvp("_id", make_document(kvp("$in", in.extract()))));

    // Projection: the populate select string (Mongoose auto-includes _id).
    auto projection = make_document(
        kvp("username", 1), kvp("name", 1), kvp("avatar", 1),
        kvp("profile", 1), kvp("isVerified", 1), kvp("stats", 1));
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
    pulse::log::error("[vectorRetrieval] author populate failed: {}", e.what());
    return;
  }

  // Replace the author ref with the hydrated doc where available.
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

// The recency window used for both the no-userVec path and the in-process
// "no embeddings yet" degrade path:
//   Post.find({ isActive:true, visibility:'public', ...filter })
//     .sort({ createdAt:-1 }).limit(limit).populate('author', ...).lean()
Json::Value recencyWindow(const Json::Value& filter, int limit) {
  bld::document f;
  f.append(kvp("isActive", true));
  f.append(kvp("visibility", "public"));
  appendFilter(f, filter);

  mongocxx::options::find opts{};
  opts.sort(make_document(kvp("createdAt", -1)));
  opts.limit(limit);

  Json::Value posts = runFind(f.extract(), std::move(opts));
  populateAuthors(posts);
  return posts;
}

} // namespace

// ── Constants ─────────────────────────────────────────────────────────────────
bool useAtlas() {
  // USE_ATLAS = !!process.env.VECTOR_SEARCH_INDEX
  return !pulse::config().env("VECTOR_SEARCH_INDEX", "").empty();
}

int fallbackWindow() {
  // FALLBACK_WINDOW = parseInt(process.env.VECTOR_FALLBACK_WINDOW, 10) || 600
  // JS: a parsed 0 (or NaN when unset/non-numeric) is falsy -> 600; any other
  // parsed integer is kept verbatim. envInt yields 0 on unset/unparseable.
  long long w = pulse::config().envInt("VECTOR_FALLBACK_WINDOW", 0);
  return w != 0 ? static_cast<int>(w) : 600;
}

// ── retrieveCandidates ────────────────────────────────────────────────────────
Json::Value retrieveCandidates(const Json::Value& userVec,
                               const Json::Value& filter,
                               int limit) {
  // No taste vector (brand-new user) -> recency window; ranker handles cold start.
  if (!hasVec(userVec)) {
    return recencyWindow(filter, limit);
  }

  if (useAtlas()) {
    try {
      return atlasVectorSearch(userVec, filter, limit);
    } catch (const std::exception& err) {
      pulse::log::warn("[vectorRetrieval] Atlas $vectorSearch failed, falling back: {}",
                       err.what());
    }
  }
  return inProcessCosine(userVec, filter, limit);
}

// ── atlasVectorSearch ─────────────────────────────────────────────────────────
Json::Value atlasVectorSearch(const Json::Value& userVec,
                              const Json::Value& filter,
                              int limit) {
  const std::string indexName = pulse::config().env("VECTOR_SEARCH_INDEX", "");
  // numCandidates = Math.max(limit * 10, 200)
  const int numCandidates = std::max(limit * 10, 200);

  // filter: { isActive: true, visibility: 'public', ...filter }
  bld::document vsFilter;
  vsFilter.append(kvp("isActive", true));
  vsFilter.append(kvp("visibility", "public"));
  appendFilter(vsFilter, filter);

  // Pipeline:
  //   { $vectorSearch: { index, path:'embedding', queryVector, numCandidates,
  //                      limit, filter } }
  //   { $addFields: { _vscore: { $meta: 'vectorSearchScore' } } }
  mongocxx::pipeline pipe;
  pipe.append_stage(make_document(kvp("$vectorSearch", make_document(
      kvp("index", indexName),
      kvp("path", "embedding"),
      kvp("queryVector", userVecToBsonArray(userVec).extract()),
      kvp("numCandidates", numCandidates),
      kvp("limit", limit),
      kvp("filter", vsFilter.extract())))));
  pipe.add_fields(make_document(kvp("_vscore",
      make_document(kvp("$meta", "vectorSearchScore")))));

  Json::Value docs(Json::arrayValue);
  auto col = pulse::db::collection(kPostCollection);
  auto cursor = col.aggregate(pipe);
  for (const auto& doc : cursor) {
    docs.append(pulse::bsonjson::toJson(doc));
  }

  // populate authors (aggregate doesn't auto-populate)
  populateAuthors(docs);
  return docs;
}

// ── inProcessCosine ───────────────────────────────────────────────────────────
Json::Value inProcessCosine(const Json::Value& userVec,
                            const Json::Value& filter,
                            int limit) {
  // Pull a bounded recent window that HAS embeddings; score in-process.
  //   { isActive:true, visibility:'public',
  //     embedding: { $exists:true, $ne:[] }, ...filter }
  bld::document f;
  f.append(kvp("isActive", true));
  f.append(kvp("visibility", "public"));
  f.append(kvp("embedding", make_document(
      kvp("$exists", true),
      kvp("$ne", make_array()))));   // $ne: []
  appendFilter(f, filter);

  mongocxx::options::find opts{};
  opts.sort(make_document(kvp("createdAt", -1)));
  opts.limit(fallbackWindow());      // FALLBACK_WINDOW (600 default)

  Json::Value window = runFind(f.extract(), std::move(opts));
  populateAuthors(window);

  // No embeddings yet -> degrade to recency so the feed still works.
  if (!window.isArray() || window.empty()) {
    return recencyWindow(filter, limit);
  }

  // scored = window.map(p => ({ p, s: cosine(userVec, p.embedding) }))
  struct Scored { const Json::Value* p; double s; };
  std::vector<Scored> scored;
  scored.reserve(window.size());
  for (const auto& p : window) {
    scored.push_back({&p, cosine(userVec, p["embedding"])});
  }

  // scored.sort((a, b) => b.s - a.s)  — descending by score, stable to mirror
  // V8's stable Array.prototype.sort.
  std::stable_sort(scored.begin(), scored.end(),
                   [](const Scored& a, const Scored& b) { return a.s > b.s; });

  // .slice(0, limit).map(x => ({ ...x.p, _vscore: x.s }))
  Json::Value out(Json::arrayValue);
  const int n = std::min<int>(limit < 0 ? 0 : limit, static_cast<int>(scored.size()));
  for (int i = 0; i < n; ++i) {
    Json::Value doc = *scored[i].p;     // copy ({ ...x.p })
    doc["_vscore"] = scored[i].s;
    out.append(std::move(doc));
  }
  return out;
}

// ── rankByCosine ──────────────────────────────────────────────────────────────
Json::Value rankByCosine(const Json::Value& userVec,
                         const Json::Value& candidates,
                         int limit) {
  if (!candidates.isArray()) return Json::Value(Json::arrayValue);

  // scored = candidates.map(p => ({ p, s: cosine(userVec, p.embedding ||
  //                                               featureVector(p)) }))
  struct Scored { const Json::Value* p; double s; };
  std::vector<Scored> scored;
  scored.reserve(candidates.size());
  for (const auto& p : candidates) {
    // p.embedding || featureVector(p): in JS an array (even empty []) is truthy,
    // so the fallback fires only when `embedding` is absent or null/undefined.
    Json::Value vec;
    if (p.isMember("embedding") && !p["embedding"].isNull()) {
      vec = p["embedding"];
    } else {
      vec = pulse::services::embedding::featureVector(p);
    }
    scored.push_back({&p, cosine(userVec, vec)});
  }

  std::stable_sort(scored.begin(), scored.end(),
                   [](const Scored& a, const Scored& b) { return a.s > b.s; });

  // .slice(0, limit).map(x => x.p)
  Json::Value out(Json::arrayValue);
  const int n = std::min<int>(limit < 0 ? 0 : limit, static_cast<int>(scored.size()));
  for (int i = 0; i < n; ++i) out.append(*scored[i].p);
  return out;
}

} // namespace pulse::services::vector_retrieval
