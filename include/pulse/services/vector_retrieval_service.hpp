// vector_retrieval_service.hpp — C++ port of src/services/vectorRetrievalService.js.
//
// "Retrieve then rank": candidate generation by vector similarity. Instead of
// feeding the ranker a recency-sorted pool, we first RETRIEVE the posts whose
// embeddings are most similar to the user's taste vector, then hand that
// semantically-relevant set to the C++ ranker.
//
// Backend selection (auto):
//   1. Atlas $vectorSearch — when VECTOR_SEARCH_INDEX is set. Scales to the
//      whole catalog.
//   2. In-process cosine   — fallback: pull a bounded recent candidate window,
//      score in-process. Works everywhere, bounded cost.
//
// The JS module is a singleton of free functions; this header mirrors it as a
// namespace of free functions. The userVec / embedding / candidate documents
// are passed as Json::Value (Float arrays for vectors, objects/arrays for
// posts), matching the lean Mongoose documents the JS handed around.
#pragma once
#include <string>
#include <vector>
#include <json/json.h>

namespace pulse::services::vector_retrieval {

// ── Constants (mirror the JS module-level constants) ──────────────────────────
//   USE_ATLAS        = !!process.env.VECTOR_SEARCH_INDEX
//   FALLBACK_WINDOW  = parseInt(process.env.VECTOR_FALLBACK_WINDOW, 10) || 600
// Both are derived from the environment once, exactly like the JS module's
// load-time evaluation. Exposed as functions so they read config()/env() the
// same way the JS read process.env at require time.
bool useAtlas();        // true when VECTOR_SEARCH_INDEX is set (non-empty).
int  fallbackWindow();  // VECTOR_FALLBACK_WINDOW or 600.

// ── retrieveCandidates ────────────────────────────────────────────────────────
// retrieveCandidates({ Post, userVec, filter = {}, limit = 200 })
//
//   userVec : the user taste vector (DIM-length, normalized). Pass an empty /
//             null Json::Value (or empty array) for a brand-new user — the
//             function then returns a recency window (ranker handles cold start).
//   filter  : base Mongo filter (visibility/isActive/etc.) merged into the query
//             as a JSON object (mirrors `...filter`). Defaults to {}.
//   limit   : how many candidates to return. Defaults to 200.
//
// Returns a JSON array of candidate post documents (lean), best-match first,
// with the `author` join applied — matching the JS .populate('author', ...).
Json::Value retrieveCandidates(const Json::Value& userVec,
                               const Json::Value& filter = Json::Value(Json::objectValue),
                               int limit = 200);

// ── atlasVectorSearch ─────────────────────────────────────────────────────────
// Atlas $vectorSearch path. Builds the exact aggregation pipeline the JS issued
// ($vectorSearch on path 'embedding' with numCandidates = max(limit*10, 200),
// then $addFields _vscore = $meta vectorSearchScore), runs it on the posts
// collection, joins authors, and returns the documents. Throws on driver error
// (the caller catches and falls back, mirroring the JS try/catch).
Json::Value atlasVectorSearch(const Json::Value& userVec,
                              const Json::Value& filter,
                              int limit);

// ── inProcessCosine ───────────────────────────────────────────────────────────
// In-process cosine fallback. Pulls a bounded recent window of posts that HAVE
// embeddings (sorted createdAt:-1, limited to FALLBACK_WINDOW), scores each by
// cosine against userVec, sorts descending, and returns the top `limit` posts
// each tagged with `_vscore`. Degrades to a plain recency window when no post
// has an embedding yet.
Json::Value inProcessCosine(const Json::Value& userVec,
                            const Json::Value& filter,
                            int limit);

// ── rankByCosine ──────────────────────────────────────────────────────────────
// Pure helper used by the eval harness + tests: rank an in-memory candidate list
// by cosine to a user vector (no DB). Each candidate is scored against its
// `embedding` field, or — when absent — the feature vector derived from the post
// (embeddingService.featureVector). Returns the top `limit` candidates,
// best-match first.
Json::Value rankByCosine(const Json::Value& userVec,
                         const Json::Value& candidates,
                         int limit);

} // namespace pulse::services::vector_retrieval
