// feedback_service.cc — implementation of the real-time engagement feedback
// loop. Ports src/services/feedbackService.js exactly.
//
// The JS file delegates vector construction to embeddingService
// (featureVector / reelVector / DIM). embeddingService is feature-mode by
// default (free, deterministic, no external calls) and its output is the same
// 34-dim L2-normalized concatenation of [topic|vibe|media|meta] blocks. The
// pieces feedbackService actually invokes are ported here in an anonymous
// namespace so this unit compiles against ONLY the provided pulse headers,
// while preserving the exact event-recording shape and Redis keys/TTLs.
#include "pulse/services/feedback_service.hpp"

#include "pulse/cache.hpp"
#include "pulse/config.hpp"

#include <sw/redis++/redis++.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace pulse {

// ───────────────────────── embedding feature-vector ─────────────────────────
// Mirrors embeddingService.js (feature mode) + InterestProfiler.extractTopics.
// Operates on Json::Value items (posts/reels) the way the JS service does.
namespace {

// 22 stable topic categories (InterestProfiler.TOPIC_PATTERNS, insertion order).
struct Topic { const char* name; std::vector<const char*> keywords; };

const std::vector<Topic>& topicPatterns() {
  static const std::vector<Topic> kTopics = {
    {"tech", {"tech","code","programming","developer","software","ai","startup",
              "algorithm","api","javascript","python","react","node","database",
              "cloud","devops","ml","machine learning","silicon valley","hackathon",
              "open source","github"}},
    {"gaming", {"game","gaming","ps5","xbox","nintendo","steam","esports","valorant",
                "fortnite","minecraft","gamer","twitch","streamer","rpg","fps",
                "mmorpg","gta","zelda","elden ring","playstation"}},
    {"crypto", {"crypto","bitcoin","ethereum","blockchain","nft","web3","defi","token",
                "mining","btc","eth","solana","altcoin","moon","hodl","wallet","doge",
                "memecoin"}},
    {"lifestyle", {"life","lifestyle","daily","routine","morning","wellness","selfcare",
                   "mindfulness","minimalist","productivity","habit","journal",
                   "gratitude","balance","hustle","grind"}},
    {"memes", {"meme","lol","funny","joke","humor","shitpost","dank","bruh","no cap",
               "sus","based","ratio","slay"}},
    {"news", {"breaking","news","update","announced","reported","politics","election",
              "government","economy","world","crisis","headline"}},
    {"sports", {"game","match","score","team","player","win","lost","championship",
                "league","nba","nfl","soccer","football","cricket","ipl","basketball",
                "tennis","f1","formula"}},
    {"music", {"music","song","album","concert","artist","band","spotify","playlist",
               "producer","beat","lyric","rap","hiphop","pop","indie","edm","vinyl",
               "guitar","piano"}},
    {"food", {"food","recipe","cooking","restaurant","meal","delicious","chef","baking",
              "cuisine","brunch","foodie","yummy","homemade","pasta","sushi","pizza",
              "vegan"}},
    {"travel", {"travel","trip","vacation","explore","adventure","wanderlust",
                "destination","flight","hotel","backpack","roadtrip","beach","mountain",
                "europe","asia","bali","paris","tokyo"}},
    {"fashion", {"fashion","style","outfit","ootd","clothing","designer","trend","drip",
                 "thrift","vintage","sneakers","streetwear","luxury","accessories",
                 "makeup","beauty"}},
    {"fitness", {"fitness","gym","workout","exercise","training","muscle","cardio",
                 "protein","bodybuilding","yoga","crossfit","run","marathon","gains",
                 "lift","health","diet","bulk","cut"}},
    {"pets", {"pet","dog","cat","puppy","kitten","doggo","pupper","animal","rescue",
              "adopt","furry","paw","cute","floof","birb","hamster","bunny"}},
    {"movies", {"movie","film","cinema","director","actor","oscar","netflix","disney",
                "marvel","dc","horror","thriller","documentary","screening","premiere",
                "review","rating"}},
    {"anime", {"anime","manga","otaku","waifu","naruto","one piece","attack on titan",
               "jjk","demon slayer","cosplay","sub","dub","weeb","kawaii","isekai",
               "shonen"}},
    {"education", {"learn","education","study","school","university","college","course",
                   "tutorial","lecture","homework","exam","degree","scholarship",
                   "graduate","phd","research"}},
    {"science", {"science","physics","chemistry","biology","space","nasa","quantum",
                 "experiment","discovery","research","atom","telescope","dinosaur",
                 "evolution","dna","lab"}},
    {"relationships", {"love","relationship","dating","crush","couple","breakup",
                       "marriage","wedding","boyfriend","girlfriend","single","toxic",
                       "red flag","green flag","situationship"}},
    {"cars", {"car","auto","vehicle","drive","horsepower","engine","supercar","tesla",
              "bmw","mercedes","toyota","jdm","turbo","drift","exhaust","modification",
              "tuning"}},
    {"photography", {"photo","photography","camera","lens","portrait","landscape",
                     "lightroom","editing","composition","exposure","bokeh",
                     "golden hour","street photography","macro","drone"}},
    {"cooking", {"cook","bake","recipe","kitchen","ingredient","homemade","meal prep",
                 "seasoning","oven","grill","stir fry","sourdough","ferment","sauce",
                 "sautee"}},
    {"politics", {"politics","election","vote","democrat","republican","congress",
                  "senate","president","policy","law","rights","protest","activism",
                  "campaign","liberal","conservative"}},
  };
  return kTopics;
}

constexpr std::array<const char*, 5> kVibes = {"chill","hype","sad","funny","creative"};
constexpr int kMediaDims = 3;  // image, video, gif
constexpr int kMetaDims  = 4;  // quality, recency, hasMedia, lengthBucket
const int kDim = static_cast<int>(topicPatterns().size())          // 22
                 + static_cast<int>(kVibes.size())                 // 5
                 + kMediaDims                                       // 3
                 + kMetaDims;                                       // 4 => 34

std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
  return s;
}

bool contains(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

// InterestProfiler.computeTopics — returns the set of matched topic *keys*.
// (feedbackService only needs the topic-key membership for the topic block.)
std::set<std::string> extractTopics(const Json::Value& post) {
  std::set<std::string> topics;
  const Json::Value& content = post["content"];

  // ── From hashtags ──
  const Json::Value& hashtags = content["hashtags"];
  if (hashtags.isArray()) {
    for (const auto& tagV : hashtags) {
      std::string clean = toLower(tagV.asString());
      // remove first '#' (JS String.replace replaces only the first occurrence)
      auto h = clean.find('#');
      if (h != std::string::npos) clean.erase(h, 1);
      topics.insert(clean);
      for (const auto& t : topicPatterns()) {
        for (const char* kw : t.keywords) {
          std::string k = kw;
          if (contains(clean, k) || contains(k, clean)) { topics.insert(t.name); break; }
        }
      }
    }
  }

  // ── From text content ──
  std::string lower = toLower(content["text"].isString() ? content["text"].asString() : "");
  for (const auto& t : topicPatterns()) {
    int matchCount = 0;
    for (const char* kw : t.keywords) {
      if (contains(lower, kw)) ++matchCount;
    }
    if (matchCount >= 1) topics.insert(t.name);
  }

  // ── From mentions ──
  const Json::Value& mentions = content["mentions"];
  if (mentions.isArray() && !mentions.empty()) topics.insert("social");

  // computeTopics returns DEFAULT_TOPICS (['general']) when empty; that key is
  // not in TOPIC_KEYS so it contributes nothing to the topic block either way.
  if (topics.empty()) topics.insert("general");
  return topics;
}

std::vector<double> topicBlock(const Json::Value& post) {
  const auto& topics = topicPatterns();
  std::vector<double> block(topics.size(), 0.0);
  std::set<std::string> set = extractTopics(post);
  for (size_t i = 0; i < topics.size(); ++i) {
    if (set.count(topics[i].name)) block[i] = 1.0;
  }
  return block;
}

std::vector<double> vibeBlock(const Json::Value& post) {
  std::vector<double> block(kVibes.size(), 0.0);
  // vs = post.vibeScore || (post.vibe ? { [post.vibe]: 1 } : null)
  const Json::Value& vibeScore = post["vibeScore"];
  std::string vibe = post["vibe"].isString() ? post["vibe"].asString() : "";

  if (vibeScore.isObject() && !vibeScore.empty()) {
    double max = 0.0;
    for (const char* v : kVibes) {
      double val = vibeScore.isMember(v) ? vibeScore[v].asDouble() : 0.0;
      max = std::max(max, val);
    }
    for (size_t i = 0; i < kVibes.size(); ++i) {
      double val = vibeScore.isMember(kVibes[i]) ? vibeScore[kVibes[i]].asDouble() : 0.0;
      block[i] = max > 0 ? val / max : 0.0;
    }
  } else if (!vibe.empty()) {
    // single-vibe fallback: vs = { [vibe]: 1 }, so max = 1 over that vibe.
    for (size_t i = 0; i < kVibes.size(); ++i) {
      block[i] = (vibe == kVibes[i]) ? 1.0 : 0.0;
    }
  }
  return block;
}

std::vector<double> mediaBlock(const Json::Value& post) {
  std::vector<double> block(kMediaDims, 0.0);  // image, video, gif
  const Json::Value& media = post["content"]["media"];
  if (media.isArray()) {
    for (const auto& m : media) {
      std::string type = m["type"].isString() ? m["type"].asString() : "";
      if (type == "image") block[0] = 1.0;
      if (type == "video") block[1] = 1.0;
      if (type == "gif")   block[2] = 1.0;
    }
  }
  return block;
}

long long nowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Parse an ISO-8601 / epoch-millis createdAt into epoch millis. Returns 0 when
// absent (JS: ageH defaults to 0 when createdAt is falsy).
long long createdAtMillis(const Json::Value& createdAt) {
  if (createdAt.isNull()) return 0;
  if (createdAt.isNumeric()) return static_cast<long long>(createdAt.asDouble());
  if (createdAt.isString()) {
    const std::string s = createdAt.asString();
    if (s.empty()) return 0;
    std::tm tm{};
    int y=0, mo=0, d=0, h=0, mi=0, se=0;
    // Accept "YYYY-MM-DDTHH:MM:SS" (Z / fractional seconds tolerated).
    if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y,&mo,&d,&h,&mi,&se) >= 3) {
      tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
      tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = se;
#if defined(_WIN32)
      std::time_t t = _mkgmtime(&tm);
#else
      std::time_t t = timegm(&tm);
#endif
      if (t != static_cast<std::time_t>(-1)) return static_cast<long long>(t) * 1000;
    }
  }
  return 0;
}

std::vector<double> metaBlock(const Json::Value& post) {
  const Json::Value& stats = post["stats"];
  double likes    = stats["likes"].isNumeric()    ? stats["likes"].asDouble()    : 0.0;
  double comments = stats["comments"].isNumeric() ? stats["comments"].asDouble() : 0.0;
  double shares   = stats["shares"].isNumeric()   ? stats["shares"].asDouble()   : 0.0;
  double eng = likes + comments * 2.0 + shares * 3.0;
  double quality = std::min(1.0, std::log10(eng + 1.0) / 4.0);

  long long created = createdAtMillis(post["createdAt"]);
  double ageH = created ? (static_cast<double>(nowMillis() - created)) / 3600000.0 : 0.0;
  double recency = std::max(0.0, 1.0 - ageH / 168.0);

  const Json::Value& media = post["content"]["media"];
  double hasMedia = (media.isArray() && !media.empty()) ? 1.0 : 0.0;

  const Json::Value& text = post["content"]["text"];
  double len = text.isString() ? static_cast<double>(text.asString().size()) : 0.0;
  double lengthBucket = std::min(1.0, len / 500.0);

  return {quality, recency, hasMedia, lengthBucket};
}

std::vector<double> l2normalize(std::vector<double> v) {
  double s = 0.0;
  for (double x : v) s += x * x;
  double n = std::sqrt(s);
  if (n == 0.0) return v;
  for (double& x : v) x /= n;
  return v;
}

std::vector<double> featureVector(const Json::Value& post) {
  std::vector<double> v;
  v.reserve(kDim);
  for (double x : topicBlock(post)) v.push_back(x);
  for (double x : vibeBlock(post))  v.push_back(x);
  for (double x : mediaBlock(post)) v.push_back(x);
  for (double x : metaBlock(post))  v.push_back(x);
  return l2normalize(std::move(v));
}

// embeddingService.reelToEmbeddable + featureVector.
std::vector<double> reelVector(const Json::Value& reel) {
  Json::Value embeddable(Json::objectValue);
  Json::Value content(Json::objectValue);
  content["text"] = reel["caption"].isString() ? reel["caption"].asString() : "";
  content["hashtags"] = reel["hashtags"].isArray() ? reel["hashtags"] : Json::Value(Json::arrayValue);
  Json::Value media(Json::arrayValue);
  Json::Value vid(Json::objectValue); vid["type"] = "video"; media.append(vid);
  content["media"] = media;
  embeddable["content"] = content;
  embeddable["vibe"] = reel["vibe"];
  embeddable["vibeScore"] = reel["vibeScore"];
  embeddable["stats"] = reel["stats"].isObject() ? reel["stats"] : Json::Value(Json::objectValue);
  embeddable["createdAt"] = reel["createdAt"];
  return featureVector(embeddable);
}

// feedbackService.l2 — identical to embeddingService.l2normalize.
std::vector<double> l2(std::vector<double> v) { return l2normalize(std::move(v)); }

// Serialize a vector to the compact JSON-array string the JS service cached
// (cacheService.set did JSON.stringify on the array).
std::string vecToJson(const std::vector<double>& v) {
  Json::Value arr(Json::arrayValue);
  for (double x : v) arr.append(x);
  Json::StreamWriterBuilder w;
  w["indentation"] = "";
  return Json::writeString(w, arr);
}

// Parse a cached vector; returns empty when not a numeric array.
std::vector<double> jsonToVec(const std::string& json) {
  std::vector<double> out;
  Json::CharReaderBuilder rb;
  Json::Value root;
  std::string errs;
  const std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
  if (!reader->parse(json.data(), json.data() + json.size(), &root, &errs)) return out;
  if (!root.isArray()) return out;
  out.reserve(root.size());
  for (const auto& x : root) out.push_back(x.isNumeric() ? x.asDouble() : 0.0);
  return out;
}

// (item._id || item).toString() — prefer the {_id} field, else the raw id.
// bsonjson renders ObjectId _id as its 24-char hex string, so this is normally
// already a string; numeric ids convert via asString().
std::string itemIdString(const Json::Value& item) {
  if (item.isObject()) {
    const Json::Value& id = item["_id"];
    if (id.isString())  return id.asString();
    if (id.isNumeric()) return id.asString();
    return "";
  }
  if (item.isString())  return item.asString();
  if (item.isNumeric()) return item.asString();
  return "";
}

} // namespace

// ──────────────────────────── service surface ───────────────────────────────

FeedbackService::FeedbackService() {
  auto& cfg = config();
  userVecTtl_  = cfg.envInt("USER_VECTOR_TTL_SEC", 300);   // 5 min
  velocityTtl_ = cfg.envInt("VELOCITY_TTL_SEC", 3600);     // 1 h window
  // ALPHA = parseFloat(process.env.FEEDBACK_ALPHA) || 0.15
  std::string a = cfg.env("FEEDBACK_ALPHA", "");
  alpha_ = 0.15;
  if (!a.empty()) {
    try { double parsed = std::stod(a); if (parsed != 0.0) alpha_ = parsed; } catch (...) {}
  }
}

FeedbackService& FeedbackService::instance() {
  static FeedbackService s;
  return s;
}

std::string FeedbackService::userVecKey(const std::string& userId) const {
  return "uservec:" + userId;
}

std::string FeedbackService::velocityKey(const std::string& type, const std::string& id) const {
  return "vel:" + type + ":" + id;
}

void FeedbackService::reinforceUserVector(const std::string& userId,
                                          const Json::Value& itemEmbedding,
                                          double weight) {
  try {
    // 1. Ensure we have a vector. If not an array, treat the value as an item
    //    object and compute its feature vector (embeddingService.featureVector).
    std::vector<double> emb;
    if (itemEmbedding.isArray()) {
      emb.reserve(itemEmbedding.size());
      for (const auto& x : itemEmbedding) emb.push_back(x.isNumeric() ? x.asDouble() : 0.0);
    } else {
      emb = featureVector(itemEmbedding);
    }
    // 2. Validate length === DIM; bail otherwise.
    if (emb.empty() || static_cast<int>(emb.size()) != kDim) return;

    // 3. Current cached taste vector (or zero vector of DIM).
    std::vector<double> base;
    auto cur = cache().get(userVecKey(userId));
    if (cur && !cur->empty()) base = jsonToVec(*cur);
    if (static_cast<int>(base.size()) != kDim) base.assign(kDim, 0.0);

    // 4. Adaptive learning rate, then EMA update toward the engaged item.
    double aRate = std::min(0.5, alpha_ * weight);
    std::vector<double> next(kDim);
    for (int i = 0; i < kDim; ++i) next[i] = base[i] * (1.0 - aRate) + emb[i] * aRate;

    // 5. L2-normalize and cache with TTL.
    cache().set(userVecKey(userId), vecToJson(l2(std::move(next))), userVecTtl_);
  } catch (...) {
    // best-effort — periodic rebuild will catch up
  }
}

void FeedbackService::recordEngagement(const std::string& userId,
                                       const Json::Value& item,
                                       const std::string& contentType,
                                       const std::string& action) {
  // 1. Signal weight by action (comment 1.5, share 2, view 0.4, default 1).
  double weight = action == "comment" ? 1.5
                : action == "share"   ? 2.0
                : action == "view"    ? 0.4
                                      : 1.0;

  // 2. Item embedding: prefer item.embedding, else compute (reel vs post).
  //    JS uses `item?.embedding` whenever truthy; an array (even empty) counts
  //    as present and is passed through (reinforceUserVector then DIM-checks it).
  Json::Value emb;
  const Json::Value& stored = item["embedding"];
  if (stored.isArray()) {
    emb = stored;
  } else {
    std::vector<double> computed =
        (contentType == "reel") ? reelVector(item) : featureVector(item);
    emb = Json::Value(Json::arrayValue);
    for (double x : computed) emb.append(x);
  }
  reinforceUserVector(userId, emb, weight);

  // 3. Bump per-item recent velocity (atomic INCR + expire). Best-effort.
  try {
    cache().incrementRateLimit(velocityKey(contentType, itemIdString(item)), velocityTtl_);
  } catch (...) {
    // ignore
  }
}

std::unordered_map<std::string, long long>
FeedbackService::getVelocities(const std::string& contentType,
                               const std::vector<std::string>& itemIds) {
  std::unordered_map<std::string, long long> out;
  if (itemIds.empty()) return out;
  try {
    auto redis = cache().raw();
    if (!redis) return out;

    std::vector<std::string> keys;
    keys.reserve(itemIds.size());
    for (const auto& id : itemIds) keys.push_back(velocityKey(contentType, id));

    std::vector<sw::redis::OptionalString> vals;
    vals.reserve(keys.size());
    redis->mget(keys.begin(), keys.end(), std::back_inserter(vals));

    for (size_t i = 0; i < itemIds.size(); ++i) {
      long long v = 0;
      if (i < vals.size() && vals[i]) {
        try { v = std::stoll(*vals[i]); } catch (...) { v = 0; }
      }
      out[itemIds[i]] = v;  // parseInt(...) || 0
    }
  } catch (...) {
    return {};  // degrade to empty (zero velocities)
  }
  return out;
}

} // namespace pulse
