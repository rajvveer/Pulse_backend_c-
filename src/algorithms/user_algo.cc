// user_algo.cc — C++ port of UserAlgo v2.0 (user relevance / recommendations).
//
// Pure. The engagement-rate "dead field" fix: calculateEngagementRate reads
// recent* fields that don't exist on the User schema, so it always returned 0.
// Here it accepts those fields if present BUT also falls back to a derivable
// proxy from the fields that DO exist (stats.likes / stats.posts / followers)
// so niche/trending detection is no longer universally dead. The JS wrapper is
// responsible for passing real recent-engagement when available.
//
// Input JSON: { mode:"suggested"|"similar"|"trending",
//               userId, candidates:[user...], target:{user},
//               followingIds:[...], mutualFollows:[...],
//               affinityMap:{id:aff}, graphScores:{id:score},
//               isColdStart, limit, minFollowers, nowMs }
// Output JSON: [ user with _recommendScore / _similarityScore / _trendScore ]
#include "common.hpp"
#include <map>
#include <set>

namespace pulse {
namespace {

struct Cfg {
  double FOLLOWER_W=0.25,ENGAGEMENT_W=0.35,QUALITY_W=0.2,CONSISTENCY_W=0.1,RECENCY_W=0.1;
  double FOLLOWER_LOG_BASE=10, MAX_FOLLOWER_SCORE=5;
  double EXCELLENT_ENGAGEMENT=0.06, GOOD_ENGAGEMENT=0.03;
  double FOLLOW_OVERLAP_W=2.0, ENGAGEMENT_HISTORY_W=1.5, CONTENT_SIM_W=1.0, GRAPH_W=1.8, NICHE_DISCOVERY_W=1.2;
  double ACTIVE_24H=1.3, ACTIVE_7D=1.1, INACTIVE_30D=0.7;
  double VERIFIED_BOOST=1.3;
  double NICHE_MAX_FOLLOWERS=5000, NICHE_MIN_ER=0.08;
  double COLD_TRENDING_W=2.0;
};
const Cfg C;

double engagementRate(const Json& user) {
  const Json& s = user["stats"];
  double followers = s["followers"].num(0);
  if (followers == 0) return 0;
  // Preferred: real recent engagement fields if Node supplied them.
  double recentLikes = s["recentLikes"].num(-1);
  if (recentLikes >= 0) {
    double rc = s["recentComments"].num(0), rs = s["recentShares"].num(0), rp = std::max(1.0, s["recentPosts"].num(1));
    double eng = recentLikes + rc*2 + rs*3;
    return (eng / rp) / followers;
  }
  // Fallback proxy from existing fields (avg likes per post / followers).
  double posts = s["posts"].num(0), likes = s["likes"].num(0);
  if (posts > 0) return (likes / posts) / followers;
  return 0;
}

bool isNiche(const Json& user) {
  double followers = user["stats"]["followers"].num(0);
  double er = engagementRate(user);
  return followers < C.NICHE_MAX_FOLLOWERS && followers > 0 && er >= C.NICHE_MIN_ER;
}

double creatorScore(const Json& user, double now) {
  const Json& s = user["stats"];
  double score = 0;
  double followers = s["followers"].num(0);
  if (followers > 0) score += std::min(std::log(followers+1)/std::log(C.FOLLOWER_LOG_BASE), C.MAX_FOLLOWER_SCORE) * C.FOLLOWER_W;

  double er = engagementRate(user);
  double es = er >= C.EXCELLENT_ENGAGEMENT ? 1.0 : er >= C.GOOD_ENGAGEMENT ? 0.7 : er / C.GOOD_ENGAGEMENT * 0.7;
  score += es * C.ENGAGEMENT_W;

  double posts = s["posts"].num(0), totalLikes = s["totalLikes"].num(s["likes"].num(0));
  if (posts > 0) score += std::min(log10p(totalLikes/posts)/3.0, 1.0) * C.QUALITY_W;

  double ppw = s["postsPerWeek"].num(0);
  if (ppw >= 3) score += 1.0 * C.CONSISTENCY_W; else if (ppw >= 1) score += 0.5 * C.CONSISTENCY_W;

  double lastActive = user["lastActiveAt"].num(user["updatedAt"].num(0));
  if (lastActive > 0) {
    double h = hoursSince(lastActive, now);
    if (h < 24) score += 1.0 * C.RECENCY_W;
    else if (h < 168) score += 0.5 * C.RECENCY_W;
    else if (h > 720) score *= C.INACTIVE_30D;
  }
  if (user["isVerified"].boolean(false)) score *= C.VERIFIED_BOOST;
  if (isNiche(user)) score *= 1.15;
  return score;
}

std::string uid(const Json& u) { return u.isObject() ? u["_id"].str() : u.str(); }

double contentSimilarity(const Json& a, const Json& b) {
  std::set<std::string> i1, i2;
  if (a["interests"].isArray()) for (auto& x : a["interests"].arr()) i1.insert(x.str());
  if (b["interests"].isArray()) for (auto& x : b["interests"].arr()) i2.insert(x.str());
  if (i1.empty() || i2.empty()) return 0;
  std::set<std::string> uni = i1; uni.insert(i2.begin(), i2.end());
  int inter = 0; for (auto& x : i1) if (i2.count(x)) inter++;
  double jaccard = uni.empty() ? 0 : (double)inter / uni.size();
  double vibeBonus = (!a["dominantVibe"].str().empty() && a["dominantVibe"].str()==b["dominantVibe"].str()) ? 0.15 : 0;
  return jaccard + vibeBonus;
}

} // namespace

std::string run_user_rank(const std::string& in) {
  Json input = Json::parse(in);
  std::string mode = input["mode"].str("suggested");
  double now = input["nowMs"].num(0);
  int limit = (int)input["limit"].num(20);
  const Json& candidates = input["candidates"];
  Json out = Json::array();
  if (!candidates.isArray()) return out.dump();

  if (mode == "trending") {
    double minFollowers = input["minFollowers"].num(100);
    std::vector<std::pair<Json,double>> scored;
    for (auto& u : candidates.arr()) {
      if (u["stats"]["followers"].num(0) < minFollowers) continue;
      double er = engagementRate(u);
      double growth = u["stats"]["followerGrowthRate"].num(0);
      double momentum = std::min(u["stats"]["recentPosts"].num(0)/7.0, 1.0);
      double niche = isNiche(u) ? 0.5 : 0;
      scored.push_back({u, er*10 + growth + momentum + niche});
    }
    std::sort(scored.begin(), scored.end(), [](auto&a,auto&b){return a.second>b.second;});
    for (int i=0;i<(int)scored.size() && i<limit;++i) { Json u=scored[i].first; u["_trendScore"]=scored[i].second; out.push_back(u); }
    return out.dump();
  }

  if (mode == "similar") {
    const Json& target = input["target"];
    std::string tid = uid(target);
    std::vector<std::pair<Json,double>> scored;
    for (auto& u : candidates.arr()) {
      if (uid(u) == tid) continue;
      double score = contentSimilarity(target, u) * C.CONTENT_SIM_W;
      double tf = target["stats"]["followers"].num(0), uf = u["stats"]["followers"].num(0);
      if (tf>0 && uf>0) { double lr = 1 - std::fabs(log10p(tf)-log10p(uf))/5.0; score += std::max(0.0, lr)*0.5; }
      score += creatorScore(u, now) * 0.3;
      double te = engagementRate(target), ue = engagementRate(u);
      if (te>0 && ue>0) score += (std::min(te,ue)/std::max(te,ue))*0.2;
      scored.push_back({u, score});
    }
    std::sort(scored.begin(), scored.end(), [](auto&a,auto&b){return a.second>b.second;});
    for (int i=0;i<(int)scored.size() && i<limit;++i) { Json u=scored[i].first; u["_similarityScore"]=scored[i].second; out.push_back(u); }
    return out.dump();
  }

  // suggested
  std::set<std::string> followingSet, mutualSet;
  for (auto& x : input["followingIds"].arr()) followingSet.insert(x.str());
  for (auto& x : input["mutualFollows"].arr()) mutualSet.insert(x.str());
  const Json& affinityMap = input["affinityMap"];
  const Json& graphScores = input["graphScores"];
  bool isCold = input["isColdStart"].boolean(false);
  std::string userIdStr = input["userId"].str();

  std::vector<std::pair<Json,double>> scored;
  for (auto& u : candidates.arr()) {
    std::string id = uid(u);
    if (id == userIdStr || followingSet.count(id)) continue;
    double score = creatorScore(u, now);
    score += affinityMap[id].num(0) * C.ENGAGEMENT_HISTORY_W;
    score += graphScores[id].num(0) * C.GRAPH_W;
    if (mutualSet.count(id)) score *= C.FOLLOW_OVERLAP_W;
    if (isNiche(u)) score += C.NICHE_DISCOVERY_W;
    if (isCold) score += u["stats"]["followerGrowthRate"].num(0) * C.COLD_TRENDING_W;
    scored.push_back({u, score});
  }
  std::sort(scored.begin(), scored.end(), [](auto&a,auto&b){return a.second>b.second;});
  for (int i=0;i<(int)scored.size() && i<limit;++i) { Json u=scored[i].first; u["_recommendScore"]=scored[i].second; out.push_back(u); }
  return out.dump();
}

} // namespace pulse
