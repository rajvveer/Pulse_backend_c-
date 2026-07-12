// common.hpp — shared utilities for the Pulse C++ algorithm kernels.
//
// Each algorithm exposes a single pure entry point:
//   std::string run_xxx(const std::string& inputJson);
// taking a JSON string (data pre-fetched by Node) and returning a JSON string.
// No DB access, no global state, no time source other than a caller-supplied
// `now` (so results are deterministic and testable).
#pragma once
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "json.hpp"

namespace pulse {

using pj::Json;

// ── Numeric helpers ──
inline double clampd(double v, double lo, double hi) { return std::max(lo, std::min(hi, v)); }

// Exponential time decay: score * 0.5^(ageHours / halfLife).
inline double timeDecay(double score, double createdAtMs, double nowMs, double halfLifeHours) {
  double ageHours = (nowMs - createdAtMs) / 3600000.0;
  if (ageHours < 0) ageHours = 0;
  return score * std::pow(0.5, ageHours / halfLifeHours);
}

inline double hoursSince(double createdAtMs, double nowMs) {
  double h = (nowMs - createdAtMs) / 3600000.0;
  return h < 0 ? 0 : h;
}

inline double log10p(double x) { return std::log10(x + 1.0); }
inline double log2p(double x) { return std::log2(x + 1.0); }

// Shannon-entropy-based diversity over a vector of non-negative weights.
// Returns 0..1 (1 = perfectly even spread).
inline double normalizedEntropy(const std::vector<double>& values) {
  double total = 0;
  for (double v : values) total += (v > 0 ? v : 0);
  if (total == 0) return 0;
  double entropy = 0;
  for (double v : values) {
    if (v > 0) { double p = v / total; entropy -= p * std::log2(p); }
  }
  double maxEntropy = std::log2((double)values.size());
  return maxEntropy > 0 ? entropy / maxEntropy : 0;
}

// ── String helpers ──
inline std::string toLower(const std::string& s) {
  std::string out = s;
  for (char& c : out) c = (char)std::tolower((unsigned char)c);
  return out;
}

// Whole-word membership: does `text` contain `word` bounded by non-alphanumerics?
// Used so 'goat' does NOT match inside 'scapegoat'. Both args assumed lowercase.
inline bool isWordChar(char c) {
  return std::isalnum((unsigned char)c) || c == '\'';
}
inline bool containsWord(const std::string& text, const std::string& word) {
  if (word.empty()) return false;
  size_t pos = 0;
  while ((pos = text.find(word, pos)) != std::string::npos) {
    bool leftOk = (pos == 0) || !isWordChar(text[pos - 1]);
    size_t end = pos + word.size();
    bool rightOk = (end >= text.size()) || !isWordChar(text[end]);
    if (leftOk && rightOk) return true;
    pos += 1;
  }
  return false;
}

// Plain substring containment (for multi-word phrases / emoji byte sequences).
inline bool containsSub(const std::string& text, const std::string& sub) {
  return !sub.empty() && text.find(sub) != std::string::npos;
}

// Count non-overlapping occurrences of `sub` in `text`.
inline int countOccurrences(const std::string& text, const std::string& sub) {
  if (sub.empty()) return 0;
  int count = 0; size_t pos = 0;
  while ((pos = text.find(sub, pos)) != std::string::npos) { ++count; pos += sub.size(); }
  return count;
}

// Tokenize on whitespace, stripping surrounding punctuation from each token.
inline std::vector<std::string> tokenize(const std::string& lower) {
  std::vector<std::string> tokens;
  std::string cur;
  for (char c : lower) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) tokens.push_back(cur);
  return tokens;
}

// Strip non-[a-z0-9'] from a token for whole-word comparison.
inline std::string cleanToken(const std::string& w) {
  std::string out;
  for (char c : w) if (std::isalnum((unsigned char)c) || c == '\'') out += c;
  return out;
}

// ── Algorithm entry points (defined in their .cc files) ──
std::string run_vibe_classify(const std::string& in);
std::string run_mood_detect(const std::string& in);
std::string run_interest_score(const std::string& in);
std::string run_feed_rank(const std::string& in);
std::string run_reel_rank(const std::string& in);
std::string run_comments_rank(const std::string& in);
std::string run_user_rank(const std::string& in);
std::string run_dna_match(const std::string& in);

} // namespace pulse
