// comments_algo.cc — C++ port of CommentsAlgo v2.0 (comment ranking).
//
// Pure. Fixes baked in:
//  - MAX_RANK cap so regex-heavy scoring never runs unbounded.
//  - Patterns compiled ONCE (static) — the JS hoisted the repeated-char regex;
//    here all std::regex are constructed once at first use.
//  - Wilson term decoupled from raw engagement magnitude (uses log of
//    engagement) so it acts as a confidence-adjusted rank, not a double-count
//    of raw likes (the PARTIAL finding).
//
// Input JSON: { comments:[ {content, likes:[..]|likeCount, replies:[...],
//                           author:{isVerified,pulseScore,stats}, createdAt(ms)} ],
//               mode:"best|top|new|controversial", opId, nowMs }
// Output JSON: [ comment with _score, replies recursively ranked ]
#include "common.hpp"
#include <regex>
#include <map>
#include <set>

namespace pulse {
namespace {

struct Cfg {
  double W_likes=1.0,W_replies=2.5,W_lengthBonus=0.01,W_karma=0.1,W_wilson=3.0,W_replyChain=1.5,W_substance=2.0;
  int MIN_QUALITY_LENGTH=10,OPTIMAL_LENGTH=280,MAX_LENGTH_BONUS=100;
  double HALF_LIFE_HOURS=6,VERIFIED_BOOST=1.5,OP_BOOST=2.0,CONTROVERSY_THRESHOLD=0.3;
  int MAX_RANK=100, MAX_REPLY_DEPTH=5;
  double TOX_SEVERE=-10,TOX_MODERATE=-5,TOX_MILD=-2;
  int SPAM_MAX_LINKS=2; double SPAM_MAX_CAPS=0.6; int SPAM_MIN_CAPS_LEN=10, SPAM_MAX_REPEAT=4;
};
const Cfg C;

// Reputation tiers (PulseScore -> multiplier).
double reputationBoost(double ps) {
  if (ps >= 800) return 1.4; if (ps >= 600) return 1.25; if (ps >= 400) return 1.15; if (ps >= 200) return 1.05; return 1.0;
}

// ── Compiled-once pattern sets ──
const std::vector<std::regex>& spamPatterns() {
  static const std::vector<std::regex> P = {
    std::regex(R"(follow\s*(me|back))", std::regex::icase),
    std::regex(R"(check\s*(out|my)\s*(profile|page|link|bio))", std::regex::icase),
    std::regex(R"(click\s*(here|link|this))", std::regex::icase),
    std::regex(R"(free\s*(money|gift|follow|likes|v-?bucks))", std::regex::icase),
    std::regex(R"(\b(dm|message)\s*me\b)", std::regex::icase),
    std::regex(R"(\bsub(scribe)?\s*(to|my)\b)", std::regex::icase),
    std::regex(R"(\bpromo\s*code\b)", std::regex::icase),
    std::regex(R"(\bwin\s*(a|free|big)\b)", std::regex::icase),
    std::regex(R"(buy\s*(now|this|here))", std::regex::icase),
    std::regex(R"(\b(whatsapp|telegram)\s*me\b)", std::regex::icase),
    std::regex(R"(make\s*\$?\d+\s*(a|per)\s*(day|hour|week))", std::regex::icase),
    std::regex(R"(\b(cashapp|venmo|paypal)\b)", std::regex::icase),
    std::regex(R"(special\s*offer)", std::regex::icase),
    std::regex(R"(limited\s*time)", std::regex::icase)
  };
  return P;
}
const std::regex& linkPattern() { static const std::regex R(R"(https?://[^\s]+|www\.[^\s]+|bit\.ly/[^\s]+|t\.co/[^\s]+)", std::regex::icase); return R; }
const std::regex& repeatedChar() { static const std::regex R(R"((.)\1{4,})"); return R; }

struct ToxSet { std::vector<std::regex> severe, moderate, mild; };
const ToxSet& toxicity() {
  static const ToxSet T = {
    { std::regex(R"(\b(kys|kill\s*yourself|die\s*already)\b)", std::regex::icase),
      std::regex(R"(\b(k+i+l+l+\s*y+o+u+r+s+e+l+f+)\b)", std::regex::icase) },
    { std::regex(R"(\b(shut\s*up|stfu|gtfo)\b)", std::regex::icase),
      std::regex(R"(\b(you'?re?\s*(trash|garbage|useless|worthless|pathetic))\b)", std::regex::icase),
      std::regex(R"(\b(nobody\s*(likes|cares|asked))\b)", std::regex::icase),
      std::regex(R"(\b(go\s*(away|cry|die))\b)", std::regex::icase),
      std::regex(R"(\b(u\s*suck)\b)", std::regex::icase) },
    { std::regex(R"(\b(cringe|ratio|L\s*take|bad\s*take|clown)\b)", std::regex::icase),
      std::regex(R"(\b(cope|seethe|mid)\b)", std::regex::icase),
      std::regex(R"(\b(touch\s*grass)\b)", std::regex::icase) }
  };
  return T;
}
const std::vector<std::regex>& substanceHigh() {
  static const std::vector<std::regex> P = {
    std::regex(R"(\b(because|since|therefore|however|although|moreover)\b)", std::regex::icase),
    std::regex(R"(\b(i\s*think|in\s*my\s*opinion|imo|i\s*believe)\b)", std::regex::icase),
    std::regex(R"(\b(for\s*example|such\s*as|similar\s*to|compared\s*to)\b)", std::regex::icase),
    std::regex(R"(\b(on\s*the\s*other\s*hand|alternatively|that\s*said)\b)", std::regex::icase),
    std::regex(R"(\?\s*$)")
  };
  return P;
}
const std::regex& substanceLow() { static const std::regex R(R"(^(lol|lmao|same|this|fr|real|facts|true|w|l|ratio|based|mid)+$)", std::regex::icase); return R; }

double wilsonScore(double pos, double total) {
  if (total == 0) return 0;
  double z = 1.96, p = pos / total;
  double denom = 1 + (z*z)/total;
  double inner = p*(1-p)/total + (z*z)/(4*total*total);
  return (p + (z*z)/(2*total) - z*std::sqrt(inner)) / denom;
}

int likeCountOf(const Json& c) {
  if (c["likes"].isArray()) return (int)c["likes"].size();
  return (int)c["likeCount"].num(0);
}
int replyCountOf(const Json& c) { return c["replies"].isArray() ? (int)c["replies"].size() : 0; }

double analyzeSubstance(const std::string& content) {
  if (content.empty()) return 0;
  std::string trimmed = content;
  // trim
  size_t a = trimmed.find_first_not_of(" \t\n\r");
  size_t b = trimmed.find_last_not_of(" \t\n\r");
  trimmed = (a==std::string::npos) ? "" : trimmed.substr(a, b-a+1);
  if (trimmed.size() <= 5) return 0;
  if (std::regex_search(trimmed, substanceLow())) return 0;
  double score = 0.5;
  for (auto& r : substanceHigh()) if (std::regex_search(content, r)) score += 0.4;
  int words = (int)tokenize(content).size();
  if (words >= 15) score += 0.3; if (words >= 30) score += 0.3; if (words >= 50) score += 0.2;
  // sentence count
  int sentences = 0; { std::string cur; for (char ch : content) { if (ch=='.'||ch=='!'||ch=='?') { if (cur.size()>5) sentences++; cur.clear(); } else cur+=ch; } }
  if (sentences >= 2) score += 0.2; if (sentences >= 3) score += 0.2;
  return std::min(2.0, score);
}

double analyzeReplyChainQuality(const Json& replies) {
  if (!replies.isArray() || replies.size()==0) return 0;
  double quality = std::min(1.0, replies.size() * 0.2);
  double totalSub = 0; size_t n = std::min((size_t)10, replies.size());
  for (size_t i=0;i<n;++i) totalSub += analyzeSubstance(replies.at(i)["content"].str());
  quality += (totalSub / n) * 0.5;
  std::set<std::string> authors;
  for (auto& r : replies.arr()) { std::string id = r["author"].isObject()?r["author"]["_id"].str():r["author"].str(); if(!id.empty()) authors.insert(id); }
  if (authors.size() >= 3) quality += 0.5; if (authors.size() >= 5) quality += 0.3;
  return std::min(3.0, quality);
}

double detectToxicity(const std::string& content) {
  if (content.empty()) return 0;
  std::string lower = toLower(content);
  double penalty = 0;
  for (auto& r : toxicity().severe) if (std::regex_search(lower, r)) penalty += C.TOX_SEVERE;
  for (auto& r : toxicity().moderate) if (std::regex_search(lower, r)) penalty += C.TOX_MODERATE;
  for (auto& r : toxicity().mild) if (std::regex_search(lower, r)) penalty += C.TOX_MILD;
  return penalty;
}

bool isSpammy(const Json& c) {
  std::string original = c["content"].str();
  std::string content = toLower(original);
  if (content.empty()) return false;
  int signals = 0;
  for (auto& r : spamPatterns()) if (std::regex_search(content, r)) signals += 2;
  // links
  int links = (int)std::distance(std::sregex_iterator(content.begin(), content.end(), linkPattern()), std::sregex_iterator());
  if (links > C.SPAM_MAX_LINKS) signals += 3; else if (links > 0 && content.size() < 50) signals += 1;
  // caps
  if (original.size() >= (size_t)C.SPAM_MIN_CAPS_LEN) {
    int caps=0; for(char ch:original) if(ch>='A'&&ch<='Z') caps++;
    if ((double)caps/original.size() > C.SPAM_MAX_CAPS) signals += 1;
  }
  if (std::regex_search(content, repeatedChar())) signals += 1;
  // repeated words
  auto words = tokenize(content);
  if (words.size() >= 6) {
    std::map<std::string,int> wc; for (auto& w : words) wc[w]++;
    int mx=0; for (auto& kv : wc) mx = std::max(mx, kv.second);
    if ((double)mx/words.size() > 0.5) signals += 2;
  }
  // hashtag stuffing
  int hashtags=0; for(char ch:content) if(ch=='#') hashtags++;
  if (hashtags > 5) signals += 2;
  return signals >= 3;
}

double commentQuality(const Json& c, bool isOP) {
  double score = 0;
  int likes = likeCountOf(c), replies = replyCountOf(c);
  int totalEng = likes + replies;

  // Wilson — decoupled from raw magnitude (FIX): scale by log(engagement) so
  // it expresses confidence, not size; raw engagement is added separately.
  if (totalEng > 0) {
    double wilson = wilsonScore(likes, totalEng + 5);
    score += wilson * C.W_wilson * (1.0 + log2p(totalEng));
  }
  score += likes * C.W_likes + replies * C.W_replies;

  std::string content = c["content"].str();
  if ((int)content.size() >= C.MIN_QUALITY_LENGTH) {
    double lb = std::min((double)content.size(), (double)C.OPTIMAL_LENGTH) * C.W_lengthBonus;
    score += std::min(lb, (double)C.MAX_LENGTH_BONUS);
  }
  score += analyzeSubstance(content) * C.W_substance;
  if (replies > 0) score += analyzeReplyChainQuality(c["replies"]) * C.W_replyChain;

  const Json& a = c["author"];
  if (a["isVerified"].boolean(false)) score *= C.VERIFIED_BOOST;
  if (isOP) score *= C.OP_BOOST;
  double ps = a["pulseScore"].num(a["stats"]["pulseScore"].num(0));
  score *= reputationBoost(ps);
  double karma = a["stats"]["karma"].num(a["stats"]["reputation"].num(0));
  score += log10p(karma) * C.W_karma;
  score += detectToxicity(content);
  return std::max(0.0, score);
}

double controversy(const Json& c) {
  int likes = likeCountOf(c), replies = replyCountOf(c);
  if (likes + replies < 3) return 0;
  double replyRatio = (double)replies / (likes + 1);
  int substantive = 0;
  if (c["replies"].isArray()) { size_t n=std::min((size_t)10,c["replies"].size()); for(size_t i=0;i<n;++i) if(analyzeSubstance(c["replies"].at(i)["content"].str())>0.5) substantive++; }
  double debateQuality = substantive / (double)std::max(1, std::min(replies,10));
  if (replyRatio >= C.CONTROVERSY_THRESHOLD) return (likes+replies)*replyRatio*(0.5+debateQuality*0.5);
  return 0;
}

Json rankComments(const Json& commentsIn, const std::string& mode, const std::string& opId, double now, int depth) {
  Json out = Json::array();
  if (!commentsIn.isArray() || commentsIn.size()==0 || depth > C.MAX_REPLY_DEPTH) return out;

  // Cap the number scored per pass.
  size_t limit = std::min((size_t)C.MAX_RANK, commentsIn.size());

  struct SC { Json comment; double score; };
  std::vector<SC> scored;
  for (size_t i=0;i<limit;++i) {
    const Json& c = commentsIn.at(i);
    std::string aid = c["author"].isObject()?c["author"]["_id"].str():c["author"].str();
    bool isOP = !opId.empty() && aid == opId;
    double score;
    if (mode == "new") score = c["createdAt"].num(0);
    else if (mode == "controversial") score = controversy(c);
    else if (mode == "top") score = commentQuality(c, isOP);
    else { score = commentQuality(c, isOP); score = timeDecay(score, c["createdAt"].num(now), now, C.HALF_LIFE_HOURS); }
    if (isSpammy(c)) score *= 0.1;
    scored.push_back({c, score});
  }

  std::sort(scored.begin(), scored.end(), [](const SC& a, const SC& b){ return a.score > b.score; });

  for (auto& sc : scored) {
    Json c = sc.comment;
    c["_score"] = sc.score;
    if (c["replies"].isArray() && c["replies"].size() > 0)
      c["replies"] = rankComments(c["replies"], mode, opId, now, depth + 1);
    out.push_back(c);
  }
  return out;
}

} // namespace

std::string run_comments_rank(const std::string& in) {
  Json input = Json::parse(in);
  std::string mode = input["mode"].str("best");
  std::string opId = input["opId"].str();
  double now = input["nowMs"].num(0);
  return rankComments(input["comments"], mode, opId, now, 0).dump();
}

} // namespace pulse
