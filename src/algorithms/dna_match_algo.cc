// dna_match_algo.cc — C++ port of DNAMatchAlgo v2.0 matching math.
//
// Pure math only. The DB-bound orchestration (findTwins scanning SocialDNA, the
// weekly job) stays in JS — the JS wrapper fetches candidates and calls this to
// score them, and now serves precomputed twins + clamps limit + caps candidates
// (the full-scan fix). The weighted-sum-vs-count confidence fix: confidence is
// computed from an explicit interactionCount when provided, not the weighted
// totalSignals sum.
//
// Input JSON modes:
//  { mode:"compatibility", a:{strands,totalSignals,dominantVibe,interactionCount},
//    b:{...} }
//  { mode:"batch", user:{strands,totalSignals,dominantVibe,interactionCount},
//    candidates:[ {user, strands, totalSignals, dominantVibe, interactionCount} ],
//    limit }
// Output: compatibility object, or sorted matches array.
#include "common.hpp"
#include <map>

namespace pulse {
namespace {

const std::vector<std::string> VIBES = {"chill","hype","sad","funny","creative"};
std::map<std::string,double> vibeIdf() { return {{"chill",1.0},{"hype",1.0},{"sad",1.3},{"funny",1.1},{"creative",1.5}}; }

const int MIN_SIGNALS_FOR_MATCH = 10;
const int TWIN_THRESHOLD = 85;
const int HIGH_MATCH_THRESHOLD = 70;
const double DIVERSITY_BONUS = 0.05;
const int CONF_STRONG = 50, CONF_MODERATE = 20;

double strand(const Json& strands, const std::string& v) { return strands[v].num(0); }

int matchPercent(const Json& sa, const Json& sb) {
  auto idf = vibeIdf();
  double dot=0, magA=0, magB=0, totalA=0, totalB=0;
  for (auto& v : VIBES) {
    double a = strand(sa, v) * idf[v];
    double b = strand(sb, v) * idf[v];
    dot += a*b; magA += a*a; magB += b*b;
    totalA += strand(sa, v); totalB += strand(sb, v);
  }
  magA = std::sqrt(magA); magB = std::sqrt(magB);
  if (magA==0 || magB==0) return 0;
  double cosine = dot/(magA*magB);
  double magRatio = (totalA>0 && totalB>0) ? std::min(totalA,totalB)/std::max(totalA,totalB) : 0;
  double blended = cosine*0.85 + magRatio*0.15;
  return (int)std::round(blended*100);
}

// Confidence from interaction COUNT (fix). Falls back to totalSignals only if
// the caller couldn't supply a real count.
double confidence(const Json& a, const Json& b) {
  double ca = a["interactionCount"].num(a["totalSignals"].num(0));
  double cb = b["interactionCount"].num(b["totalSignals"].num(0));
  double mn = std::min(ca, cb);
  if (mn >= CONF_STRONG) return 1.0;
  if (mn >= CONF_MODERATE) return 0.7;
  if (mn >= MIN_SIGNALS_FOR_MATCH) return 0.4;
  return 0.2;
}

double diversity(const Json& strands) {
  std::vector<double> vals; for (auto& v : VIBES) vals.push_back(strand(strands, v));
  return normalizedEntropy(vals);
}

} // namespace

std::string run_dna_match(const std::string& in) {
  Json input = Json::parse(in);
  std::string mode = input["mode"].str("compatibility");

  if (mode == "compatibility") {
    const Json& a = input["a"]; const Json& b = input["b"];
    const Json& sa = a["strands"]; const Json& sb = b["strands"];
    int mp = matchPercent(sa, sb);

    Json breakdown = Json::array();
    auto idf = vibeIdf();
    std::string closest, furthest; double minDiff=1e9, maxDiff=-1;
    for (auto& v : VIBES) {
      double diff = std::fabs(strand(sa,v) - strand(sb,v));
      Json item = Json::object();
      item["vibe"]=v; item["userA"]=strand(sa,v); item["userB"]=strand(sb,v);
      item["diff"]=diff; item["idfWeight"]=idf[v];
      breakdown.push_back(item);
      if (diff < minDiff) { minDiff=diff; closest=v; }
      if (diff > maxDiff) { maxDiff=diff; furthest=v; }
    }

    double conf = confidence(a, b);
    bool mutualVibe = a["dominantVibe"].str() == b["dominantVibe"].str() && !a["dominantVibe"].str().empty();
    int adjusted = std::min(100, mp + (mutualVibe ? 3 : 0));
    double divBonus = (diversity(sa)+diversity(sb))/2 * DIVERSITY_BONUS * 100;
    int finalMatch = std::min(100, (int)std::round(adjusted + divBonus));

    std::string label = "Low Match";
    if (finalMatch >= TWIN_THRESHOLD) label = "DNA Twins";
    else if (finalMatch >= HIGH_MATCH_THRESHOLD) label = "High Match";
    else if (finalMatch >= 50) label = "Good Match";

    Json out = Json::object();
    out["matchPercent"]=finalMatch; out["label"]=label; out["breakdown"]=breakdown;
    out["commonGround"]=closest; out["biggestDiff"]=furthest;
    out["isTwin"]=(finalMatch>=TWIN_THRESHOLD); out["confidence"]=conf;
    out["mutualVibe"]= mutualVibe ? Json(a["dominantVibe"].str()) : Json(nullptr);
    return out.dump();
  }

  // batch: score candidates against the user, return sorted matches >= 50.
  const Json& user = input["user"];
  const Json& su = user["strands"];
  int limit = (int)input["limit"].num(20);
  const Json& candidates = input["candidates"];

  struct M { Json entry; double sortScore; };
  std::vector<M> matches;
  if (candidates.isArray()) {
    for (auto& cand : candidates.arr()) {
      int mp = matchPercent(su, cand["strands"]);
      if (mp < 50) continue;
      double conf = confidence(user, cand);
      bool mutualVibe = user["dominantVibe"].str() == cand["dominantVibe"].str() && !user["dominantVibe"].str().empty();
      int finalPct = std::min(100, mp + (mutualVibe ? 3 : 0));
      Json e = Json::object();
      e["user"] = cand["user"];
      e["strands"] = cand["strands"];
      e["dominantVibe"] = cand["dominantVibe"];
      e["matchPercent"] = finalPct;
      e["confidence"] = conf;
      e["mutualVibe"] = mutualVibe;
      matches.push_back({e, finalPct * (0.7 + conf*0.3)});
    }
  }
  std::sort(matches.begin(), matches.end(), [](const M&a,const M&b){return a.sortScore>b.sortScore;});

  Json out = Json::array();
  for (int i=0;i<(int)matches.size() && i<limit;++i) out.push_back(matches[i].entry);
  return out.dump();
}

} // namespace pulse
