// embedding_service.hpp — C++ port of src/services/embeddingService.js.
//
// Turns posts/reels and users into fixed-dimension, L2-normalized vectors for
// semantic candidate retrieval. Two modes, same output shape (a DIM-length unit
// vector):
//   - FEATURE mode (default, free, no external deps): an interpretable
//     concatenation of signal blocks —
//       [ 22 topic dims | 5 vibe dims | 3 media dims | 4 quality/meta dims ].
//   - SEMANTIC mode (opt-in via EMBED_PROVIDER=openai|gemini + API key): real
//     text embeddings fetched over HTTP, blended with the feature block. The
//     index-compatible vector returned is ALWAYS the feature vector (the JS keeps
//     the semantic vector on a side channel only), so dimensionality is stable.
//
// The dimension is STABLE (DIM = 34) so vectors are comparable across items and
// a single Atlas vector index works regardless of mode.
//
// Exposed as a class with a process-wide singleton accessor (`pulse::embedding()`)
// mirroring the JS `module.exports = { ... }` module singleton.
#pragma once
#include <string>
#include <vector>
#include <optional>
#include <json/json.h>

namespace pulse {

class EmbeddingService {
public:
  // Singleton accessor (mirrors the JS module singleton).
  static EmbeddingService& instance();

  // ── Stable constants (mirror the JS exports DIM / TOPIC_KEYS / VIBES) ──
  // 22 stable topic keys from InterestProfiler.TOPIC_PATTERNS, in declaration
  // order (the order the JS Object.keys() yields).
  static const std::vector<std::string>& topicKeys();
  // ['chill', 'hype', 'sad', 'funny', 'creative'].
  static const std::vector<std::string>& vibes();
  static constexpr int kMediaDims = 3;  // image, video, gif
  static constexpr int kMetaDims  = 4;  // quality, recency, hasMedia, lengthBucket
  // DIM = TOPIC_KEYS.length + VIBES.length + MEDIA_DIMS + META_DIMS = 34.
  static constexpr int kDim = 22 + 5 + kMediaDims + kMetaDims;
  int dim() const { return kDim; }

  // The resolved provider: (EMBED_PROVIDER || 'feature') lowercased.
  const std::string& provider() const { return provider_; }

  // ── Core vector math ──────────────────────────────────────────────────────
  // L2-normalize: returns v/|v|, or v unchanged when |v| == 0.
  std::vector<double> l2normalize(const std::vector<double>& v) const;

  // Cosine similarity of two equal-length, already-normalized vectors. Returns
  // the dot product (both are unit vectors). 0 when either is empty or lengths
  // differ.
  double cosine(const std::vector<double>& a, const std::vector<double>& b) const;

  // ── Feature blocks (each returns a fixed-length sub-vector) ───────────────
  std::vector<double> topicBlock(const Json::Value& post) const;  // length 22
  std::vector<double> vibeBlock(const Json::Value& post) const;   // length 5
  std::vector<double> mediaBlock(const Json::Value& post) const;  // length 3
  std::vector<double> metaBlock(const Json::Value& post) const;   // length 4

  // ── Full vectors (all DIM=34, L2-normalized) ─────────────────────────────
  // Feature vector for a post/reel-as-post-shape.
  std::vector<double> featureVector(const Json::Value& post) const;

  // Normalize a Reel doc into the post shape featureVector expects (reels live
  // in the SAME vector space as posts: caption->text, always video media).
  Json::Value reelToEmbeddable(const Json::Value& reel) const;

  // Feature vector for a Reel.
  std::vector<double> reelVector(const Json::Value& reel) const;

  // Build a USER taste vector by averaging the feature vectors of engaged
  // content, plus optional explicit topic affinities and vibe strands.
  // `args` is an object mirroring the JS destructured arg:
  //   { engagedPosts?: Post[], topicAffinities?: object, vibeStrands?: object }
  // Returns a DIM-length unit vector in the SAME space as posts.
  std::vector<double> userVector(const Json::Value& args) const;

  // ── Embedding entry points ────────────────────────────────────────────────
  // Async embed entry point. In feature mode this is synchronous-fast; in
  // semantic mode it fetches the provider embedding then discards it (the JS
  // keeps the feature vector as the index-compatible one). ALWAYS returns a
  // DIM-length feature vector.
  std::vector<double> embedPost(const Json::Value& post) const;

  // Pluggable semantic provider (only used if EMBED_PROVIDER set + key present).
  // Returns the provider's raw embedding, or std::nullopt when unavailable / no
  // text / on any error.
  std::optional<std::vector<double>> semanticEmbed(const std::string& text) const;

private:
  EmbeddingService();

  // Mirror of InterestProfiler.extractTopics(post): returns the set of topic
  // labels (known category keys + raw hashtags + 'social') derived from the post
  // content. embeddingService only consults membership of the 22 TOPIC_KEYS.
  std::vector<std::string> extractTopics(const Json::Value& post) const;

  std::string provider_;  // (EMBED_PROVIDER || 'feature').toLowerCase()
};

// Convenience free function matching the JS module singleton usage.
inline EmbeddingService& embedding() { return EmbeddingService::instance(); }

} // namespace pulse
