// user_vector_service.hpp — C++ port of src/services/userVectorService.js.
//
// userVectorService — builds (and caches) a user's taste vector for retrieval.
//
// The vector lives in the SAME space as post embeddings (DIM = 34, L2-normalized),
// so cosine(userVec, postVec) ≈ predicted affinity. It's assembled from real
// signals:
//   - embeddings of recently LIKED posts (strongest taste signal)
//   - SocialDNA vibe strands (chill/hype/…)
//   - UserBehavior topic affinities
// Cached in Redis under `uservec:${userId}` with a short TTL (USER_VECTOR_TTL_SEC,
// default 300s) because it changes slowly relative to feed loads.
//
// The JS module exported free functions backed by a module singleton; here we
// mirror that with a class exposing a process-wide singleton accessor
// (UserVectorService::instance(), or the pulse::userVector() convenience). The
// vector itself is passed/returned as a Json::Value — a JSON array of numbers
// (Json::nullValue == the JS `null` "no signal yet" result) — so it threads
// directly into vector_retrieval::retrieveCandidates the same way the JS
// number[]|null did.
//
// The JS function took an injected `deps` object ({ Like, Post, SocialDNA,
// UserBehavior }) purely for test injection; the C++ models are concrete
// namespaces (pulse::models::like / post / socialdna / userbehavior), so the
// dependency collapses away and is called directly — exactly the queries the
// spec documents.
#pragma once
#include <string>
#include <vector>
#include <json/json.h>

namespace pulse {

class UserVectorService {
public:
  // Process-wide singleton (mirrors the JS module singleton).
  static UserVectorService& instance();

  // ── getUserVector(userId, deps) -> number[] (DIM=34) | null ──
  // Cache-aside with TTL. Reads `uservec:${userId}`; on a hit that is an array
  // of length DIM returns it, else builds a fresh vector via buildUserVector and
  // best-effort caches it with USER_VECTOR_TTL. Returns a JSON array of numbers,
  // or Json::nullValue when there's no signal yet.
  Json::Value getUserVector(const std::string& userId) const;

  // ── buildUserVector(userId, deps) -> number[] | null ──
  // Gathers the up-to-LIKED_SAMPLE most recent liked posts' feature inputs
  // (content/vibe/vibeScore/stats/createdAt/embedding), SocialDNA vibe strands,
  // and UserBehavior topic affinities, then calls embeddingService.userVector.
  // Returns Json::nullValue when there is no signal at all (no likes, no DNA, no
  // topics) — exactly the JS guard.
  Json::Value buildUserVector(const std::string& userId) const;

  // ── invalidate(userId) ──
  // Best-effort delete of the cached `uservec:${userId}` entry. Called when a
  // user engages (likes, etc.) to bust the stale vector.
  void invalidate(const std::string& userId) const;

  // ── seedFromOnboarding(userId, { topics, vibes }) -> number[] | null ──
  // Cold-start onboarding: build an immediate taste vector from the topics/vibes
  // a brand-new user explicitly picks at signup, BEFORE they have any history.
  // Each chosen topic → affinity 1; each chosen vibe → strand 100. Caches the
  // result like a normal user vector (USER_VECTOR_TTL). Returns the vector, or
  // Json::nullValue.
  Json::Value seedFromOnboarding(const std::string& userId,
                                 const std::vector<std::string>& topics,
                                 const std::vector<std::string>& vibes) const;

private:
  UserVectorService();
  UserVectorService(const UserVectorService&) = delete;
  UserVectorService& operator=(const UserVectorService&) = delete;

  long long ttl_;        // USER_VECTOR_TTL_SEC, default 300.
  static constexpr int kLikedSample = 30;  // recent liked posts to sample.
};

// Convenience accessor matching the JS `userVectorService` singleton usage.
inline UserVectorService& userVector() { return UserVectorService::instance(); }

} // namespace pulse
