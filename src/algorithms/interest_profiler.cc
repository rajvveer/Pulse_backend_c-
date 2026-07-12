// interest_profiler.cc — C++ port of InterestProfiler v2.0.
//
// Pure scoring: the JS wrapper fetches UserBehavior prefs and passes them in.
// WITH the confirmed LOW fix: maxPossible (the 0..10 normalizer) only counts
// the authorAffinity weight when an author-affinity contribution is actually
// applied, so scores aren't systematically deflated when no affinities exist.
//
// Input JSON: { posts:[ {content:{text,hashtags,media,mentions}, author, ...} ],
//               prefs: { topics:{topic:aff}, mediaTypes:{...}, postLengths:{...},
//                        authorAffinities:{id:aff} | null } }
// Output JSON: [ { postId, relevance } ]   (0..10)
#include "common.hpp"
#include <map>
#include <set>

namespace pulse {
namespace {

struct TopicCfg { std::vector<std::string> keywords; double popularity; };

const std::map<std::string, TopicCfg>& topicPatterns() {
  static const std::map<std::string, TopicCfg> T = {
    {"tech", {{"tech","code","programming","developer","software","ai","startup","algorithm","api",
      "javascript","python","react","node","database","cloud","devops","ml","machine learning",
      "silicon valley","hackathon","open source","github"}, 0.7}},
    {"gaming", {{"game","gaming","ps5","xbox","nintendo","steam","esports","valorant","fortnite",
      "minecraft","gamer","twitch","streamer","rpg","fps","mmorpg","gta","zelda","elden ring","playstation"}, 0.8}},
    {"crypto", {{"crypto","bitcoin","ethereum","blockchain","nft","web3","defi","token","mining","btc",
      "eth","solana","altcoin","moon","hodl","wallet","doge","memecoin"}, 0.4}},
    {"lifestyle", {{"life","lifestyle","daily","routine","morning","wellness","selfcare","mindfulness",
      "minimalist","productivity","habit","journal","gratitude","balance","hustle","grind"}, 0.9}},
    {"memes", {{"meme","lol","funny","joke","humor","shitpost","dank","bruh","no cap","sus","based","ratio","slay"}, 0.95}},
    {"news", {{"breaking","news","update","announced","reported","politics","election","government",
      "economy","world","crisis","headline"}, 0.6}},
    {"sports", {{"game","match","score","team","player","win","lost","championship","league","nba","nfl",
      "soccer","football","cricket","ipl","basketball","tennis","f1","formula"}, 0.75}},
    {"music", {{"music","song","album","concert","artist","band","spotify","playlist","producer","beat",
      "lyric","rap","hiphop","pop","indie","edm","vinyl","guitar","piano"}, 0.8}},
    {"food", {{"food","recipe","cooking","restaurant","meal","delicious","chef","baking","cuisine",
      "brunch","foodie","yummy","homemade","pasta","sushi","pizza","vegan"}, 0.85}},
    {"travel", {{"travel","trip","vacation","explore","adventure","wanderlust","destination","flight",
      "hotel","backpack","roadtrip","beach","mountain","europe","asia","bali","paris","tokyo"}, 0.6}},
    {"fashion", {{"fashion","style","outfit","ootd","clothing","designer","trend","drip","thrift",
      "vintage","sneakers","streetwear","luxury","accessories","makeup","beauty"}, 0.7}},
    {"fitness", {{"fitness","gym","workout","exercise","training","muscle","cardio","protein",
      "bodybuilding","yoga","crossfit","run","marathon","gains","lift","health","diet","bulk","cut"}, 0.65}},
    {"pets", {{"pet","dog","cat","puppy","kitten","doggo","pupper","animal","rescue","adopt","furry",
      "paw","cute","floof","birb","hamster","bunny"}, 0.7}},
    {"movies", {{"movie","film","cinema","director","actor","oscar","netflix","disney","marvel","dc",
      "horror","thriller","documentary","screening","premiere","review","rating"}, 0.75}},
    {"anime", {{"anime","manga","otaku","waifu","naruto","one piece","attack on titan","jjk",
      "demon slayer","cosplay","sub","dub","weeb","kawaii","isekai","shonen"}, 0.5}},
    {"education", {{"learn","education","study","school","university","college","course","tutorial",
      "lecture","homework","exam","degree","scholarship","graduate","phd","research"}, 0.5}},
    {"science", {{"science","physics","chemistry","biology","space","nasa","quantum","experiment",
      "discovery","research","atom","telescope","dinosaur","evolution","dna","lab"}, 0.35}},
    {"relationships", {{"love","relationship","dating","crush","couple","breakup","marriage","wedding",
      "boyfriend","girlfriend","single","toxic","red flag","green flag","situationship"}, 0.85}},
    {"cars", {{"car","auto","vehicle","drive","horsepower","engine","supercar","tesla","bmw","mercedes",
      "toyota","jdm","turbo","drift","exhaust","modification","tuning"}, 0.45}},
    {"photography", {{"photo","photography","camera","lens","portrait","landscape","lightroom","editing",
      "composition","exposure","bokeh","golden hour","street photography","macro","drone"}, 0.4}},
    {"cooking", {{"cook","bake","recipe","kitchen","ingredient","homemade","meal prep","seasoning",
      "oven","grill","stir fry","sourdough","ferment","sauce","sautee"}, 0.55}},
    {"politics", {{"politics","election","vote","democrat","republican","congress","senate","president",
      "policy","law","rights","protest","activism","campaign","liberal","conservative"}, 0.5}}
  };
  return T;
}

const std::map<std::string, std::vector<std::string>>& crossTopic() {
  static const std::map<std::string, std::vector<std::string>> C = {
    {"tech",{"gaming","science","crypto","education"}}, {"gaming",{"tech","anime","memes"}},
    {"crypto",{"tech","news","politics"}}, {"lifestyle",{"fitness","food","travel","relationships"}},
    {"memes",{"funny","gaming","anime"}}, {"news",{"politics","science","crypto"}},
    {"sports",{"fitness","gaming"}}, {"music",{"movies","lifestyle","fashion"}},
    {"food",{"cooking","travel","lifestyle"}}, {"travel",{"photography","food","lifestyle"}},
    {"fashion",{"lifestyle","photography","music"}}, {"fitness",{"food","lifestyle","sports"}},
    {"pets",{"lifestyle","photography"}}, {"movies",{"music","anime","memes"}},
    {"anime",{"gaming","movies","memes"}}, {"education",{"tech","science"}},
    {"science",{"tech","education","news"}}, {"relationships",{"lifestyle","memes"}},
    {"cars",{"tech","photography"}}, {"photography",{"travel","fashion","cars"}},
    {"cooking",{"food","lifestyle"}}, {"politics",{"news","crypto"}}
  };
  return C;
}

struct W { double topic=3.0, media=1.5, length=0.8, author=2.0, fresh=1.2, niche=1.5, cross=0.8; };
const W WT;
const double AFFINITY_THRESHOLD = 0.3;

std::set<std::string> extractTopics(const Json& post) {
  std::set<std::string> topics;
  const Json& hs = post["content"]["hashtags"];
  if (hs.isArray()) {
    for (auto& tg : hs.arr()) {
      std::string clean = toLower(tg.str());
      if (!clean.empty() && clean[0] == '#') clean = clean.substr(1);
      topics.insert(clean);
      for (auto& kv : topicPatterns())
        for (auto& kw : kv.second.keywords)
          if (clean.find(kw) != std::string::npos || kw.find(clean) != std::string::npos) { topics.insert(kv.first); break; }
    }
  }
  std::string lower = toLower(post["content"]["text"].str());
  for (auto& kv : topicPatterns()) {
    for (auto& kw : kv.second.keywords) {
      if (lower.find(kw) != std::string::npos) { topics.insert(kv.first); break; }
    }
  }
  const Json& mentions = post["content"]["mentions"];
  if (mentions.isArray() && mentions.size() > 0) topics.insert("social");
  if (topics.empty()) topics.insert("general");
  return topics;
}

std::string mediaType(const Json& post) {
  const Json& media = post["content"]["media"];
  if (!media.isArray() || media.size() == 0) return "text";
  bool v=false,g=false,i=false;
  for (auto& m : media.arr()) { std::string t=m["type"].str(); if(t=="video")v=true; if(t=="gif")g=true; if(t=="image")i=true; }
  if (v) return "video"; if (g) return "gif"; if (i) return "image"; return "text";
}
std::string lengthKey(const Json& post) {
  size_t len = post["content"]["text"].str().size();
  if (len > 500) return "very_long"; if (len > 200) return "long"; if (len > 50) return "medium"; return "short";
}

double calcTopicMatch(const std::set<std::string>& postTopics, const Json& userTopics, bool haveTopics) {
  if (!haveTopics) return 0.5;
  double total = 0; int matchCount = 0;
  for (auto& t : postTopics) {
    double aff = userTopics[t].num(0);
    if (aff >= AFFINITY_THRESHOLD) { total += aff; matchCount++; }
  }
  if (matchCount == 0) return 0.2;
  double multiBonus = matchCount > 1 ? 0.1 * (matchCount - 1) : 0;
  return std::min(1.0, total / matchCount + multiBonus);
}
double calcNicheness(const std::set<std::string>& postTopics) {
  double boost = 0;
  for (auto& t : postTopics) { auto it = topicPatterns().find(t); if (it != topicPatterns().end()) boost += (1 - it->second.popularity) * 0.5; }
  return std::min(1.0, boost);
}
double calcCrossTopic(const std::set<std::string>& postTopics, const Json& userTopics) {
  std::set<std::string> userList;
  if (userTopics.isObject()) for (auto& kv : userTopics.obj()) if (kv.second.num(0) >= AFFINITY_THRESHOLD) userList.insert(kv.first);
  double boost = 0;
  for (auto& pt : postTopics) {
    auto it = crossTopic().find(pt);
    if (it != crossTopic().end()) for (auto& rt : it->second) if (userList.count(rt)) boost += 0.3;
  }
  return std::min(1.0, boost);
}

} // namespace

std::string run_interest_score(const std::string& in) {
  Json input = Json::parse(in);
  const Json& prefs = input["prefs"];
  const Json& userTopics = prefs["topics"];
  bool haveTopics = userTopics.isObject() && userTopics.size() > 0;
  const Json& mediaPrefs = prefs["mediaTypes"];
  const Json& lengthPrefs = prefs["postLengths"];
  const Json& authorAff = prefs["authorAffinities"];
  bool haveAuthorAff = authorAff.isObject() && authorAff.size() > 0;

  Json out = Json::array();
  const Json& posts = input["posts"];
  if (!posts.isArray()) return out.dump();

  for (auto& post : posts.arr()) {
    auto topics = extractTopics(post);
    double score = 0;
    score += calcTopicMatch(topics, userTopics, haveTopics) * WT.topic;
    score += calcNicheness(topics) * WT.niche;
    score += calcCrossTopic(topics, userTopics) * WT.cross;
    score += (mediaPrefs[mediaType(post)].num(0.5)) * WT.media;
    score += (lengthPrefs[lengthKey(post)].num(0.5)) * WT.length;

    // Author affinity — only contributes (and only counts toward maxPossible)
    // when an affinity actually exists for this author (FIX).
    bool authorApplied = false;
    if (haveAuthorAff) {
      std::string authorId = post["author"].isObject() ? post["author"]["_id"].str() : post["author"].str();
      double aff = authorAff[authorId].num(0);
      if (aff > 0) { score += aff * WT.author; authorApplied = true; }
    }

    double maxPossible = WT.topic + WT.media + WT.length + WT.niche + WT.cross + (authorApplied ? WT.author : 0);
    double normalized = (score / maxPossible) * 10.0;

    Json r = Json::object();
    r["postId"] = post["_id"];
    r["relevance"] = clampd(normalized, 0, 10);
    out.push_back(r);
  }
  return out.dump();
}

} // namespace pulse
