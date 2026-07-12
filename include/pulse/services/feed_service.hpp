// feed_service.hpp — C++ port of src/services/feedService.js (Pulse feed system).
//
// Feed candidate generation + ranking. Builds the JSON input for the native
// feedRank algorithm (pulse::algos::feedRank), consumes its output, paginates,
// dedups, and caches (feed:* keys + TTLs). Covers the follow-graph, location,
// and trending feeds.
//
// The JS lived as a module of exported controller helpers (getHomeFeed,
// getFollowingFeed, getForYouFeed, getGlobalFeed, getTrendingPosts,
// getNearbyPosts) plus internal helpers (getFollowGraph, processPosts,
// getCandidateSet, getForYouCandidates, rankPosts). This header mirrors that
// module as a namespace of free functions — there is no per-instance state, so
// no singleton is needed (the JS module itself held none beyond the shared
// cache singleton it delegated to).
//
// EVERY redis key format, TTL, Mongo query shape, and response field name from
// the JS source is preserved verbatim. The ranking signal gathering builds the
// EXACT JSON payload pulse::algos::feedRank consumes (see src/algorithms/
// feed_algo.cc::run_feed_rank).
//
// Cache keys & TTLs (verbatim from JS):
//   followgraph:{userId}                 TTL 60s
//   feed:candidate:global                TTL 30s   (shared by all users)
//   feed:candidate:foryou                TTL 30s   (shared by all users)
//   feed:candidate:trending:{timeRange}  TTL 30s   (by time range)
#pragma once
#include <string>
#include <vector>
#include <optional>
#include <json/json.h>

namespace pulse::services::feed {

// ── Cache key prefixes / TTLs (preserved 1:1 from feedService.js) ──────────
inline constexpr int kFollowGraphTtl = 60;   // followgraph:{userId}
inline constexpr int kCandidateTtl   = 30;   // feed:candidate:* (CANDIDATE_TTL)

// Retrieve-then-rank limit for the For-You vector path (RETRIEVE_LIMIT).
inline constexpr int kRetrieveLimit  = 200;

// Velocity window (hours) used when fetching per-post like velocity for ranking.
inline constexpr double kVelocityWindowHours = 0.5;  // VELOCITY_WINDOW_HOURS

// ── Follow graph ───────────────────────────────────────────────────────────
// getFollowGraph(userId) -> { followingIds, friendIds }.
// friendIds = mutual follows (intersection of followingIds and followerIds).
// Cached under followgraph:{userId} for kFollowGraphTtl seconds.
struct FollowGraph {
  std::vector<std::string> followingIds;
  std::vector<std::string> friendIds;
};
FollowGraph getFollowGraph(const std::string& userId);

// ── Candidate generation helpers ───────────────────────────────────────────
// getCandidateSet(kind) — newest 200 public active posts, cached (shared by all
// users) under feed:candidate:{kind} for kCandidateTtl seconds.
// kind is "global" or "foryou".
Json::Value getCandidateSet(const std::string& kind);

// getForYouCandidates(userId) — retrieve-then-rank candidate blending. Falls
// back to the shared fresh set on cold start or any retrieval error.
Json::Value getForYouCandidates(const std::string& userId);

// ── Ranking (native addon wrapper, feedAlgo.rankPosts) ─────────────────────
struct RankOptions {
  std::vector<std::string> followingIds;
  std::vector<std::string> mutualIds;
  std::vector<std::string> friendIds;
  std::vector<std::string> trendingHashtags;
  bool includeVelocity = true;  // feedAlgo.js default: includeVelocity = true
};

// rankPosts(posts, userId, options) — gathers DB-derived ranking signals,
// builds the feedRank JSON payload, calls pulse::algos::feedRank, and returns
// the ranked posts (preserving _score). Returns the input order on any error.
Json::Value rankPosts(const Json::Value& posts,
                      const std::string& userId,
                      const RankOptions& options);

// ── Post post-processing (attach like status, mask anonymous, strip signals)─
// processPosts(posts, userId) — attaches isLiked, masks anonymous authors, and
// clears the internal ranking signals (_score/_velocity/_engagementScore).
Json::Value processPosts(const Json::Value& posts, const std::string& userId);

// ── Feed builders (controller business logic, framework-agnostic) ──────────
// Each returns the COMPLETE success-response JSON object the controller writes
// to the HTTP body verbatim (the exact shape the JS res.json(...) produced).
// userId is the authenticated user. Pagination / query params are passed in by
// the caller, but each builder also clamps defensively to match the JS clamps.

struct PageParams {
  int page  = 1;   // clamped to 1..50
  int limit = 20;  // clamped to 1..50
};

// Each builder returns the COMPLETE success-response JSON object the JS
// controller passed to res.json(...) — i.e. { success:true, data:[...posts],
// pagination:{...}, ... }. `data` is the post array directly (NOT wrapped).
// The controller layer serializes this verbatim; on an exception it instead
// returns res.status(500).json({ success:false, message:'Failed to load feed' }).

// getHomeFeed: follow-graph-aware bounded candidate fetch + rank + paginate.
// vibe is "auto" or one of the VIBES (empty == "auto", matching `req.query.vibe
// || 'auto'`). Response: { success, data, pagination:{page,limit,hasMore}, vibe }.
Json::Value getHomeFeed(const std::string& userId,
                        const PageParams& page,
                        const std::string& vibe);

// getFollowingFeed: keyset pagination over posts from followed authors.
// `before` is an ISO date cursor (empty == not provided). Response:
// { success, data, pagination:{page,limit,hasMore,nextCursor,feedType:'following'} }
// (empty-follow case: data:[], pagination.page hardcoded to 1).
Json::Value getFollowingFeed(const std::string& userId,
                             const PageParams& page,
                             const std::string& before);

// getForYouFeed: retrieve-then-rank personalized feed. Response:
// { success, data, pagination:{page,limit,hasMore,feedType:'foryou'} }.
Json::Value getForYouFeed(const std::string& userId, const PageParams& page);

// getGlobalFeed: shared candidate set + rank + paginate. Response:
// { success, data, pagination:{page,limit,hasMore} }.
Json::Value getGlobalFeed(const std::string& userId, const PageParams& page);

// getTrendingPosts: cached trending candidates + trending rank. The caller
// resolves timeRangeHours as `parseInt(req.query.timeRange) || 6` (JS default
// 6 hours); it is clamped here to 1..168. limit is clamped 1..50. Response:
// { success, data }.
Json::Value getTrendingPosts(const std::string& userId,
                             int limit,
                             int timeRangeHours);

// getNearbyPosts: MongoDB geospatial query + process. Coordinates [lon, lat].
// maxDistance in meters (caller resolves `parseInt(maxDistance) || 1000`,
// clamped here to 50km). The controller returns 400 { success:false,
// message:'Location required' } when longitude/latitude are missing BEFORE
// calling this. Response: { success, data }.
Json::Value getNearbyPosts(const std::string& userId,
                           double longitude,
                           double latitude,
                           double maxDistance,
                           int limit);

} // namespace pulse::services::feed
