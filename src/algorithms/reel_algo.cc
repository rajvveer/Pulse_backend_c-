// reel_algo.cc — C++ port of ReelAlgo v2.0 (reel feed ranking).
//
// Pure: JS wrapper fetches velocity + affinity maps and passes them in.
// Fixes baked in:
//  - completion signal now meaningful because trackView (JS) actually persists
//    avgWatchPercentage on the 0..1 scale these thresholds expect.
//  - CREATOR_COLD_START.MIN_QUALITY lowered from 0.3 (=30% engagement, never
//    reachable) to 0.06 to match the engagement-rate scale used elsewhere.
//
// Input JSON:
// { reels:[...], userId, nowMs, followingIds:[...],
//   velocityMap:{reelId:vel}, affinityMap:{authorId:aff},
//   sessionDepth, userAudioPrefs:{audioId:aff},
//   negativeSignals:{skippedCreators:[...],hiddenCategories:[...]} }
// Output JSON: [ rankedReel with _score ]
#include "common.hpp"
#include <map>
#include <set>

namespace pulse {
namespace {

struct Cfg {
  double HALF_LIFE_HOURS=24, MAX_AGE_HOURS=168;
  double W_likes=1.0,W_comments=2.5,W_shares=4.0,W_views=0.1,W_watch=3.0,W_saves=2.0;
  double W_completion=4.0,W_rewatch=5.0,W_skip=-2.0;
  double PERSONALIZATION=0.35,FOLLOW_BOOST=1.5;
  double VELOCITY_WEIGHT=2.0,CREATOR_SCORE_WEIGHT=0.15,VERIFIED_BOOST=1.1;
  double COMP_EXCELLENT=0.8,COMP_GOOD=0.5,COMP_POOR=0.2;
  double REWATCH_MIN=1.5;
  double AUDIO_TRENDING=1.3,AUDIO_AFFINITY=0.5;
  int COLD_MAX_POSTS=5; double COLD_BOOST=1.4, COLD_MIN_QUALITY=0.06; // FIX: was 0.3
  double PACE_EARLY=1.3,PACE_MID=0.9,PACE_LATE=1.2; int PACE_EARLY_T=5,PACE_MID_T=15;
  double NEG_SKIP=0.6,NEG_HIDE=0.2;
};
const Cfg C;

std::string authorId(const Json& reel) {
  if (reel["user"].isObject()) return reel["user"]["_id"].str();
  if (reel["user"].isString()) return reel["user"].str();
  if (reel["author"].isObject()) return reel["author"]["_id"].str();
  return reel["author"].str();
}
const Json& author(const Json& reel) {
  if (reel["user"].isObject()) return reel["user"];
  return reel["author"];
}

double engagementScore(const Json& reel) {
  const Json& s = reel["stats"];
  double score = s["likes"].num(reel["likes"].size()) * C.W_likes
               + s["comments"].num(reel["commentsCount"].num(0)) * C.W_comments
               + s["shares"].num(0) * C.W_shares
               + s["views"].num(0) * C.W_views
               + s["saves"].num(0) * C.W_saves;

  double completion = s["avgWatchPercentage"].num(0);
  if (completion >= C.COMP_EXCELLENT) score += C.W_completion * 1.5;
  else if (completion >= C.COMP_GOOD) score += C.W_completion;
  else if (completion < C.COMP_POOR && s["views"].num(0) > 50) score += C.W_skip;
  else score += completion * C.W_watch;

  double loops = s["avgLoops"].num(s["avgReplayCount"].num(0));
  if (loops >= C.REWATCH_MIN) score += C.W_rewatch * std::min(loops, 3.0);

  return std::max(0.0, score);
}

double freshnessBoost(double createdAt, double now) {
  double h = hoursSince(createdAt, now);
  if (h < 1) return 2.0; if (h < 6) return 1.5; if (h < 24) return 1.2; return 1.0;
}
double creatorScore(const Json& a) {
  if (!a.isObject()) return 0;
  double score = a["isVerified"].boolean(false) ? C.VERIFIED_BOOST : 0;
  double followers = a["stats"]["followers"].num(0);
  if (followers > 0) score += log10p(followers) * 0.5;
  score += a["stats"]["engagementRate"].num(0) * 2;
  return score * C.CREATOR_SCORE_WEIGHT;
}
double audioBoost(const Json& reel, const Json& prefs) {
  std::string audioId = reel["audio"].isObject() ? reel["audio"]["id"].str() : reel["audioId"].str();
  if (audioId.empty()) return 0;
  double boost = 0;
  if (reel["audio"]["isTrending"].boolean(false)) boost += C.AUDIO_TRENDING - 1;
  boost += prefs[audioId].num(0) * C.AUDIO_AFFINITY;
  return boost;
}
double coldStartBoost(const Json& a) {
  if (!a.isObject()) return 0;
  double posts = a["stats"]["posts"].num(a["stats"]["reels"].num(0));
  if (posts >= C.COLD_MAX_POSTS) return 0;
  double er = a["stats"]["engagementRate"].num(0);
  if (er >= C.COLD_MIN_QUALITY) return C.COLD_BOOST - 1;
  return 0.1;
}
std::string category(const Json& reel) {
  std::string c = reel["category"].str(); if (!c.empty()) return c;
  c = reel["vibe"].str(); if (!c.empty()) return c; return "general";
}

struct Scored { Json reel; double score; std::string cat; };

} // namespace

std::string run_reel_rank(const std::string& in) {
  Json input = Json::parse(in);
  double now = input["nowMs"].num(0);
  const Json& reels = input["reels"];
  if (!reels.isArray()) return Json::array().dump();

  std::set<std::string> followingSet, skippedCreators, hiddenCats;
  for (auto& x : input["followingIds"].arr()) followingSet.insert(x.str());
  for (auto& x : input["negativeSignals"]["skippedCreators"].arr()) skippedCreators.insert(x.str());
  for (auto& x : input["negativeSignals"]["hiddenCategories"].arr()) hiddenCats.insert(toLower(x.str()));
  const Json& velocityMap = input["velocityMap"];
  const Json& affinityMap = input["affinityMap"];
  const Json& audioPrefs = input["userAudioPrefs"];
  int sessionDepth = (int)input["sessionDepth"].num(0);

  std::vector<Scored> scored;
  for (auto& reel : reels.arr()) {
    std::string aid = authorId(reel);
    std::string rid = reel["_id"].str();
    const Json& a = author(reel);

    double score = engagementScore(reel);
    score = timeDecay(score, reel["createdAt"].num(now), now, C.HALF_LIFE_HOURS);
    if (hoursSince(reel["createdAt"].num(now), now) > C.MAX_AGE_HOURS) score = engagementScore(reel) * 0.01;
    score *= freshnessBoost(reel["createdAt"].num(now), now);
    score += velocityMap[rid].num(0) * C.VELOCITY_WEIGHT;

    double aff = affinityMap[aid].num(0) * C.PERSONALIZATION;
    if (followingSet.count(aid)) aff *= C.FOLLOW_BOOST;
    score += aff;
    score += creatorScore(a);
    score += audioBoost(reel, audioPrefs);
    score += coldStartBoost(a);

    if (!aid.empty() && skippedCreators.count(aid)) score *= C.NEG_SKIP;
    std::string cat = toLower(category(reel));
    if (!cat.empty() && hiddenCats.count(cat)) score *= C.NEG_HIDE;

    scored.push_back({reel, score, category(reel)});
  }

  std::sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b){ return a.score > b.score; });

  // Category diversity (no 3 same in a row; cap per batch)
  std::vector<Scored> result, deferred;
  std::map<std::string,int> catCounts;
  for (auto& s : scored) {
    catCounts[s.cat]++;
    std::string prev = result.size()>0 ? result.back().cat : "";
    std::string prev2 = result.size()>1 ? result[result.size()-2].cat : "";
    if (s.cat==prev && s.cat==prev2) deferred.push_back(s);
    else if (catCounts[s.cat] > 4) deferred.push_back(s);
    else result.push_back(s);
  }
  for (auto& s : deferred) {
    bool ins=false;
    for (size_t i=2;i<result.size();++i) if (result[i-1].cat != s.cat) { result.insert(result.begin()+i, s); ins=true; break; }
    if (!ins) result.push_back(s);
  }

  // Session pacing
  Json out = Json::array();
  for (size_t i=0;i<result.size();++i) {
    int pos = sessionDepth + (int)i;
    double m = pos < C.PACE_EARLY_T ? C.PACE_EARLY : pos < C.PACE_MID_T ? C.PACE_MID : C.PACE_LATE;
    Json r = result[i].reel;
    r["_score"] = result[i].score * m;
    out.push_back(r);
  }
  return out.dump();
}

} // namespace pulse
