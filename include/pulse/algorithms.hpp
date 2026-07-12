// algorithms.hpp — public dispatcher for the 8 Pulse algorithm kernels.
//
// In the Node backend these lived behind a N-API addon (native/src/addon.cc).
// Here we call the SAME pure C++ kernels directly, in-process — no addon, no
// JS round-trip. Each kernel takes a JSON string (data pre-fetched from Mongo /
// Redis by the service layer) and returns a JSON string. The dispatcher mirrors
// the addon's exported names so the service/controller layer reads the same.
#pragma once
#include <string>

// Forward-declare the kernel entry points instead of including common.hpp.
// common.hpp does `using pj::Json;` inside namespace pulse, which (when pulled
// into a controller/service TU that also includes the model headers) shadows
// JsonCpp's global ::Json with pj::Json and breaks every `Json::Value` in those
// headers. The dispatcher only needs these declarations, so we avoid the leak.
namespace pulse {
std::string run_vibe_classify(const std::string& in);
std::string run_mood_detect(const std::string& in);
std::string run_interest_score(const std::string& in);
std::string run_feed_rank(const std::string& in);
std::string run_reel_rank(const std::string& in);
std::string run_comments_rank(const std::string& in);
std::string run_user_rank(const std::string& in);
std::string run_dna_match(const std::string& in);
} // namespace pulse

namespace pulse::algos {

// Mirror of the N-API surface (addon.cc). Names match the JS wrappers:
//   vibeClassify, moodDetect, interestScore, feedRank, reelRank,
//   commentsRank, userRank, dnaMatch
inline std::string vibeClassify(const std::string& in)  { return pulse::run_vibe_classify(in); }
inline std::string moodDetect(const std::string& in)    { return pulse::run_mood_detect(in); }
inline std::string interestScore(const std::string& in) { return pulse::run_interest_score(in); }
inline std::string feedRank(const std::string& in)      { return pulse::run_feed_rank(in); }
inline std::string reelRank(const std::string& in)      { return pulse::run_reel_rank(in); }
inline std::string commentsRank(const std::string& in)  { return pulse::run_comments_rank(in); }
inline std::string userRank(const std::string& in)      { return pulse::run_user_rank(in); }
inline std::string dnaMatch(const std::string& in)      { return pulse::run_dna_match(in); }

} // namespace pulse::algos
