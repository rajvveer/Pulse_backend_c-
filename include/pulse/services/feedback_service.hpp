// feedback_service.hpp — real-time engagement feedback loop, ports
// src/services/feedbackService.js.
//
// Closes the learning loop: when a user engages with content (like / strong
// view / comment / share) we (1) nudge their cached taste vector toward the
// engaged item's embedding via an online EMA update so the NEXT feed reflects
// it, and (2) bump a per-item recent-engagement velocity counter in Redis so
// the ranker can reward genuinely-rising content. Everything is best-effort —
// a Redis hiccup degrades to the periodic recompute and never blocks the
// request.
//
// Redis keys / TTLs preserved 1:1 with the JS source:
//   uservec:${userId}        TTL = USER_VECTOR_TTL_SEC || 300s   (taste vector)
//   vel:${type}:${id}        TTL = VELOCITY_TTL_SEC    || 3600s  (velocity)
//
// A process-wide singleton mirrors the JS module singleton.
#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <json/json.h>

namespace pulse {

class FeedbackService {
public:
  static FeedbackService& instance();

  // Online-update the user's taste vector toward an engaged item's embedding.
  //   userId         user whose taste vector is reinforced
  //   itemEmbedding  the item's feature vector (preferred), OR an item object
  //                  to embed when the value is not an array
  //   weight         signal strength multiplier (like=1, comment=1.5, ...)
  // Best-effort: swallows all errors (periodic rebuild catches up).
  void reinforceUserVector(const std::string& userId,
                           const Json::Value& itemEmbedding,
                           double weight = 1.0);

  // Record a real-time engagement and reinforce taste.
  //   item         the engaged content (carries `embedding` when available)
  //   contentType  "post" (default) or "reel"
  //   action       "like" (default) | "comment" | "share" | "view"
  // Computes the action weight, reinforces the taste vector, and bumps the
  // per-item velocity counter. Best-effort on both legs.
  void recordEngagement(const std::string& userId,
                        const Json::Value& item,
                        const std::string& contentType = "post",
                        const std::string& action = "like");

  // Read recent engagement velocity for a set of item ids (Redis MGET).
  // Returns { itemId -> velocity }; missing/unparseable values map to 0.
  // On error degrades to an empty map.
  std::unordered_map<std::string, long long>
  getVelocities(const std::string& contentType,
                const std::vector<std::string>& itemIds);

private:
  FeedbackService();

  std::string userVecKey(const std::string& userId) const;
  std::string velocityKey(const std::string& type, const std::string& id) const;

  int64_t userVecTtl_;   // USER_VECTOR_TTL_SEC || 300
  double  alpha_;        // FEEDBACK_ALPHA      || 0.15
  int64_t velocityTtl_;  // VELOCITY_TTL_SEC    || 3600
};

// Convenience free-function accessor mirroring the JS module singleton.
inline FeedbackService& feedbackService() { return FeedbackService::instance(); }

} // namespace pulse
