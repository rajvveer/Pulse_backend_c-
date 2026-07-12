// trust_service.hpp — C++ port of src/services/trustService.js.
//
// trustService — integrity / anti-gaming signals for ranking.
//
// Produces two ranking inputs the C++ feed kernel consumes:
//  - authorTrust ∈ [0,1]: how legitimate an author looks. Bots, sybils, and
//    brand-new throwaway accounts score low; established, engaged-with accounts
//    score high. Low-trust authors are down-weighted (not removed) so the feed
//    resists engagement farming + follow rings without nuking new real users.
//  - baitPenalty ∈ [0,1] per post: 1 = clean, lower = engagement-bait ("like if
//    you agree", "comment X", "follow for follow", reaction-farming).
//
// Trust is computed from cheap, already-stored signals (no new infra) and CACHED
// in Redis under the key `trust:${id}` with TTL = TRUST_TTL_SEC (default 600s).
// It's intentionally conservative: it shapes ranking, it is NOT a
// moderation/ban decision.
//
// The JS module exported free functions backed by a module singleton; here we
// mirror that with a class exposing a process-wide singleton accessor
// (TrustService::instance(), or the pulse::trust() convenience). Authors and
// posts are passed as Json::Value (the lean docs the ranker already holds), the
// same objects the JS code received.
#pragma once
#include <string>
#include <json/json.h>

namespace pulse {

class TrustService {
public:
  // Process-wide singleton (mirrors the JS module singleton).
  static TrustService& instance();

  // Result of buildSignals: a trustMap (authorId -> score) and a baitMap
  // (postId -> penalty). Both are JSON objects keyed by 24-hex id strings,
  // matching the { trustMap, baitMap } the JS service returned.
  struct Signals {
    Json::Value trustMap;  // Json::objectValue: authorId -> number
    Json::Value baitMap;   // Json::objectValue: postId   -> number
  };

  // ── baitPenalty(post) -> number (0.3 .. 1.0) ──
  // Pure, no DB/cache. Extracts post.content.text || post.caption || ''.
  // Empty text => 1 (clean). Otherwise counts BAIT_PATTERN matches; 0 hits => 1,
  // else max(0.3, 1 - hits*0.25). 1 hit -> 0.75, 2 -> 0.5, 3+ -> 0.3 (floor).
  double baitPenalty(const Json::Value& post) const;

  // ── computeAuthorTrust(author) -> number (0 .. 1) ──
  // Pure, no DB/cache. Null/empty author => 0.5; verified author => 1.0.
  // Otherwise a neutral 0.5 prior adjusted by account maturity, real audience,
  // follow-ring / bot signals, and bought-follower signal, then clamped to [0,1].
  double computeAuthorTrust(const Json::Value& author) const;

  // ── getAuthorTrust(author) -> number ──
  // Cached author-trust lookup. Resolves the author id from author._id || author
  // (a bare string id is accepted too). No id => 0.5. Reads `trust:${id}` from
  // Redis; on hit (a parseable number) returns it, else computes via
  // computeAuthorTrust and best-effort caches it with TRUST_TTL.
  double getAuthorTrust(const Json::Value& author) const;

  // ── buildSignals(candidates) -> { trustMap, baitMap } ──
  // candidates is a JSON array of post docs. Dedups authors (author || user),
  // computes bait inline per post, and resolves trust once per author.
  Signals buildSignals(const Json::Value& candidates) const;

  // ── invalidate(authorId) ──
  // Best-effort delete of the cached `trust:${authorId}` entry. authorId may be
  // a bare hex string or an ObjectId-shaped JSON value.
  void invalidate(const Json::Value& authorId) const;
  void invalidate(const std::string& authorId) const;

private:
  TrustService();
  TrustService(const TrustService&) = delete;
  TrustService& operator=(const TrustService&) = delete;

  long long trustTtl_;  // TRUST_TTL_SEC, default 600.
};

// Convenience accessor matching the JS `trustService` singleton usage.
inline TrustService& trust() { return TrustService::instance(); }

} // namespace pulse
