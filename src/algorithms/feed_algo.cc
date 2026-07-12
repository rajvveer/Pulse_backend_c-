// feed_algo.cc — C++ port of feedAlgo v2.0 (post feed ranking).
//
// Pure: the JS wrapper fetches everything (velocity map, affinity map, friend
// likes, seen post ids, user behavior) and passes it in. The seenTopics bug
// (lean Map -> plain object) is structurally avoided: topics arrive as a plain
// JSON object and we read keys directly.
//
// Input JSON:
// {
//   posts: [ post... ],          // each: _id, author{_id,isVerified}, stats{}, content{}, createdAt(ms), vibe
//   userId, nowMs,
//   followingIds: [...], mutualIds: [...], friendIds: [...],
//   trendingHashtags: [...],
//   velocityMap: { postId: velocity },
//   affinityMap: { authorId: affinity },
//   friendLikes: { postId: count },          // precomputed social proof
//   seenPostIds: [...],
//   relevanceMap: { postId: 0..10 },         // from interest profiler (optional)
//   isColdStart: bool, sessionDepth: number,
//   vibe: "auto"|...                          // optional filter
// }
// Output JSON: [ rankedPost with _score ]    (order is final)
#include "common.hpp"
#include <map>
#include <set>
#include <unordered_map>

namespace pulse {
namespace {

struct Cfg {
  double HALF_LIFE_HOURS = 36, MAX_AGE_HOURS = 168;
  double W_likes=1.0, W_comments=3.0, W_shares=4.0, W_views=0.05;
  double PERSONALIZATION=0.6, FOLLOW_BOOST=1.8, MUTUAL_BOOST=2.2, INTEREST_BOOST=2.5;
  double TRENDING_HASHTAG_BOOST=1.5, VELOCITY_WEIGHT=3.0;
  double VRR_HIGH=0.55, VRR_DISC=0.30;
  double PACE_EARLY=1.3, PACE_MID=0.9, PACE_LATE=1.2; int PACE_EARLY_T=5, PACE_MID_T=15;
  double SOCIAL_FRIEND_BOOST=0.8, SOCIAL_MAX=3.0; int SOCIAL_MIN_FRIENDS=1;
  double CURIOSITY_CONTROVERSIAL=1.2;
  double FRESH_NEW=1.3, FRESH_SEEN=0.7;
  int FATIGUE_MAX_AUTHOR=5; double FATIGUE_TOPIC_THRESH=0.7, FATIGUE_TOPIC_PENALTY=0.8;
  double COLD_VERIFIED=1.4;
  int QUALITY_OLD_HOURS=120; double QUALITY_MIN_OLD=1;
  // ── Fairness / diversity (NEW) ──
  // Down-weight authors over-represented in the candidate pool so a few prolific
  // creators don't monopolize exposure (cuts the Gini coefficient). Penalty is
  // log-scaled: the Nth post from one author gets score / (1 + FAIRNESS_K*ln(N)).
  double FAIRNESS_K=0.35;
  // Diversity interleave: avoid >MAX_SAME_VIBE_RUN consecutive same-vibe items.
  int MAX_SAME_VIBE_RUN=2;
  // ── Exploration / exploitation (NEW) ──
  // Add an "uncertainty bonus" so under-shown content gets a chance instead of
  // the feed collapsing into a filter bubble. Each post carries an `explore`
  // signal in [0,1] (high = fresh + low recent exposure); the bonus is a
  // fraction of the post's own score so it nudges rather than dominates.
  // Optimistic-initial-value / UCB-style: reward what we know least about.
  // Kept CONSERVATIVE (0.06): the eval harness showed higher rates trade real
  // offline relevance for marginal novelty. Exploration's true payoff is
  // long-term (learning taste over sessions) and can't surface in a one-shot
  // offline eval, so we cap it where it doesn't measurably hurt relevance.
  double EXPLORATION_RATE=0.06;
  // ── Integrity (NEW) ──
  // Low-trust authors (bots/sybils/follow-rings) are down-weighted, not removed.
  // factor = TRUST_FLOOR + (1-TRUST_FLOOR)*trust → trust 1 keeps full score,
  // trust 0 keeps only TRUST_FLOOR. baitMap multiplies directly (1 = clean).
  double TRUST_FLOOR=0.4;
};
const Cfg C;

std::string mediaType(const Json& post) {
  const Json& media = post["content"]["media"];
  if (!media.isArray() || media.size() == 0) return "text_only";
  bool v=false,g=false,i=false;
  for (auto& m : media.arr()) { std::string t=m["type"].str(); if(t=="video")v=true; if(t=="gif")g=true; if(t=="image")i=true; }
  if (v) return "video"; if (g) return "gif"; if (i) return "image"; return "text_only";
}
double mediaBoost(const std::string& t) {
  if (t=="image") return 1.1; if (t=="video") return 1.3; if (t=="gif") return 1.05; return 1.0;
}
std::string authorId(const Json& post) {
  const Json& a = post["author"];
  return a.isObject() ? a["_id"].str() : a.str();
}
double postBaseScore(const Json& post) {
  const Json& s = post["stats"];
  double likes = s["likes"].num(0);
  double score = likes * C.W_likes + s["comments"].num(0) * C.W_comments
               + s["shares"].num(0) * C.W_shares + s["views"].num(0) * C.W_views;
  score *= mediaBoost(mediaType(post));
  return score;
}

struct Scored { Json post; double score; std::string vrr; std::string aid; std::string vibe; };

} // namespace

std::string run_feed_rank(const std::string& in) {
  Json input = Json::parse(in);
  double now = input["nowMs"].num(0);
  const Json& posts = input["posts"];
  if (!posts.isArray()) return Json::array().dump();

  std::set<std::string> followingSet, mutualSet, friendSet, seenSet;
  for (auto& x : input["followingIds"].arr()) followingSet.insert(x.str());
  for (auto& x : input["mutualIds"].arr()) mutualSet.insert(x.str());
  for (auto& x : input["friendIds"].arr()) friendSet.insert(x.str());
  for (auto& x : input["seenPostIds"].arr()) seenSet.insert(x.str());

  std::set<std::string> trendingTags;
  for (auto& x : input["trendingHashtags"].arr()) trendingTags.insert(toLower(x.str()));

  const Json& trustMap = input["trustMap"];  // authorId -> trust [0,1]
  const Json& baitMap = input["baitMap"];    // postId -> bait penalty [0,1]
  const Json& velocityMap = input["velocityMap"];
  const Json& affinityMap = input["affinityMap"];
  const Json& friendLikes = input["friendLikes"];
  const Json& relevanceMap = input["relevanceMap"];
  bool isCold = input["isColdStart"].boolean(false);
  int sessionDepth = (int)input["sessionDepth"].num(0);

  // ── Quality gate ──
  std::vector<Json> quality;
  for (auto& post : posts.arr()) {
    if (post["isAnonymous"].boolean(false)) { quality.push_back(post); continue; }
    double hrs = hoursSince(post["createdAt"].num(now), now);
    if (hrs < C.QUALITY_OLD_HOURS) { quality.push_back(post); continue; }
    const Json& s = post["stats"];
    double eng = s["likes"].num(0) + s["comments"].num(0) + s["shares"].num(0);
    if (eng >= C.QUALITY_MIN_OLD) quality.push_back(post);
  }

  // ── Score ──
  std::vector<Scored> scored;
  scored.reserve(quality.size());
  for (auto& post : quality) {
    std::string aid = authorId(post);
    std::string pid = post["_id"].str();

    double score = timeDecay(postBaseScore(post), post["createdAt"].num(now), now, C.HALF_LIFE_HOURS);
    double hrs = hoursSince(post["createdAt"].num(now), now);
    if (hrs > C.MAX_AGE_HOURS) score = postBaseScore(post) * 0.005;

    score += velocityMap[pid].num(0) * C.VELOCITY_WEIGHT;

    // trending hashtag boost
    const Json& tags = post["content"]["hashtags"];
    if (tags.isArray()) {
      int hit = 0;
      for (auto& t : tags.arr()) if (trendingTags.count(toLower(t.str()))) hit++;
      score += hit * C.TRENDING_HASHTAG_BOOST;
    }

    score += affinityMap[aid].num(0) * C.PERSONALIZATION;
    if (mutualSet.count(aid)) score *= C.MUTUAL_BOOST;
    else if (followingSet.count(aid)) score *= C.FOLLOW_BOOST;

    double rel = relevanceMap.contains(pid) ? relevanceMap[pid].num(5.0) : 5.0;
    score += rel * C.INTEREST_BOOST;

    // freshness
    bool seen = seenSet.count(pid) > 0;
    score *= seen ? C.FRESH_SEEN : C.FRESH_NEW;

    // curiosity (controversial = high comment/like ratio)
    const Json& s = post["stats"];
    double comments = s["comments"].num(0), likes = s["likes"].num(0);
    if (likes > 0 && comments / likes > 0.15) score += C.CURIOSITY_CONTROVERSIAL;

    // social proof
    double friendCount = friendLikes[pid].num(0);
    if (friendCount >= C.SOCIAL_MIN_FRIENDS) {
      double boost = std::min(friendCount * C.SOCIAL_FRIEND_BOOST, C.SOCIAL_MAX);
      score *= (1 + boost);
    }

    // cold start
    if (isCold) {
      double b = 1.0;
      if (post["author"]["isVerified"].boolean(false)) b *= C.COLD_VERIFIED;
      double eng = likes + comments * 2;
      if (eng > 50) b *= 1.2; if (eng > 200) b *= 1.3;
      if (hrs < 12) b *= 1.2;
      score *= b;
    }

    // exploration: reward under-shown / fresh content (UCB-style uncertainty
    // bonus). `explore` ∈ [0,1] is supplied by Node (fresh + low recent
    // exposure → high); if absent, derive from age so brand-new posts still get
    // a look. The bonus is a fraction of the post's own score → nudge not flood.
    double explore = post["explore"].num(-1);
    if (explore < 0) explore = std::max(0.0, 1.0 - hrs / 48.0); // 0 at 48h old
    score += score * C.EXPLORATION_RATE * explore;

    // ── Integrity: down-weight low-trust authors + engagement-bait ──
    if (trustMap.isObject() && !aid.empty() && trustMap.contains(aid)) {
      double trust = trustMap[aid].num(1.0);
      score *= C.TRUST_FLOOR + (1.0 - C.TRUST_FLOOR) * clampd(trust, 0, 1);
    }
    if (baitMap.isObject() && baitMap.contains(pid)) {
      score *= clampd(baitMap[pid].num(1.0), 0, 1);
    }

    double combined = (rel / 10.0) * 0.6 + (log10p(postBaseScore(post)) / 5.0) * 0.4;
    std::string vrr = combined > 0.7 ? "HIGH" : combined > 0.4 ? "DISCOVERY" : "WILDCARD";

    scored.push_back({post, score, vrr, aid, post["vibe"].str()});
  }

  // ── Content fatigue (author + topic saturation, approximated by author only
  //    here since topics need text scan; author cap is the dominant effect) ──
  {
    std::unordered_map<std::string,int> authorCounts;
    for (auto& sc : scored) {
      if (!sc.aid.empty()) {
        int& cnt = authorCounts[sc.aid];
        cnt++;
        if (cnt > C.FATIGUE_MAX_AUTHOR) sc.score *= 0.5;
      }
    }
  }

  // ── Creator-exposure fairness ──
  // Process candidates in current-best order; each subsequent post from the
  // same author is progressively discounted (log-scaled). This pulls down
  // authors who flood the candidate pool WITHOUT erasing them, lowering the
  // exposure Gini and lifting catalog coverage — measured in the eval harness.
  {
    // Pre-sort by raw score so "Nth post" is counted from the strongest first.
    std::sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b){ return a.score > b.score; });
    std::unordered_map<std::string,int> seen;
    for (auto& sc : scored) {
      if (sc.aid.empty()) continue;
      int n = ++seen[sc.aid];
      if (n > 1) sc.score /= (1.0 + C.FAIRNESS_K * std::log((double)n));
    }
  }

  // ── Single sort (after fairness) ──
  std::sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b){ return a.score > b.score; });

  // ── VRR distribution ──
  std::vector<Scored> distributed;
  if (scored.size() >= 10) {
    std::vector<Scored> hi, disc, wild;
    for (auto& s : scored) { if (s.vrr=="HIGH") hi.push_back(s); else if (s.vrr=="DISCOVERY") disc.push_back(s); else wild.push_back(s); }
    size_t len = scored.size();
    size_t hc = (size_t)(len * C.VRR_HIGH), dc = (size_t)(len * C.VRR_DISC);
    for (size_t i=0;i<hc && i<hi.size();++i) distributed.push_back(hi[i]);
    for (size_t i=0;i<dc && i<disc.size();++i) distributed.push_back(disc[i]);
    size_t wc = len > hc + dc ? len - hc - dc : 0;
    for (size_t i=0;i<wc && i<wild.size();++i) distributed.push_back(wild[i]);
    // append any leftovers to preserve completeness
    if (distributed.size() < scored.size()) {
      std::set<std::string> in;
      for (auto& s : distributed) in.insert(s.post["_id"].str());
      for (auto& s : scored) if (!in.count(s.post["_id"].str())) distributed.push_back(s);
    }
  } else {
    distributed = scored;
  }

  // ── Author + vibe diversity interleave ──
  // Defer an item if placing it would create a 3rd consecutive same-author OR
  // exceed MAX_SAME_VIBE_RUN consecutive same-vibe items, then re-insert
  // deferred items at the first slot that breaks BOTH runs. This restores the
  // vibe-diversity the retrieval step compresses, without re-sorting by score.
  auto vibeRun = [&](const std::vector<Scored>& v, const std::string& vibe) {
    int run = 0;
    for (auto it = v.rbegin(); it != v.rend(); ++it) { if (it->vibe == vibe) run++; else break; }
    return run;
  };
  std::vector<Scored> diverse, deferred;
  for (auto& s : distributed) {
    std::string prev = diverse.size()>0 ? diverse.back().aid : "";
    std::string prev2 = diverse.size()>1 ? diverse[diverse.size()-2].aid : "";
    bool authorRun = !s.aid.empty() && s.aid==prev && s.aid==prev2;
    bool vibeRunExceeded = !s.vibe.empty() && vibeRun(diverse, s.vibe) >= C.MAX_SAME_VIBE_RUN;
    if (authorRun || vibeRunExceeded) deferred.push_back(s); else diverse.push_back(s);
  }
  for (auto& s : deferred) {
    bool ins=false;
    for (size_t i=2;i<diverse.size();++i) {
      bool authorOk = diverse[i-1].aid != s.aid;
      bool vibeOk = diverse[i-1].vibe != s.vibe || diverse[i-2].vibe != s.vibe;
      if (authorOk && vibeOk) { diverse.insert(diverse.begin()+i, s); ins=true; break; }
    }
    if (!ins) diverse.push_back(s);
  }

  // ── Session pacing (score tag) ──
  Json out = Json::array();
  for (size_t i=0;i<diverse.size();++i) {
    int pos = sessionDepth + (int)i;
    double m = pos < C.PACE_EARLY_T ? C.PACE_EARLY : pos < C.PACE_MID_T ? C.PACE_MID : C.PACE_LATE;
    Json p = diverse[i].post;
    p["_score"] = diverse[i].score * m;
    out.push_back(p);
  }
  return out.dump();
}

} // namespace pulse
