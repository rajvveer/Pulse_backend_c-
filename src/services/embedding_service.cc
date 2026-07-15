// embedding_service.cc — implementation of embedding_service.hpp.
//
// 1:1 port of src/services/embeddingService.js (and the slice of
// src/Algorithms/_fallback/InterestProfiler.js it depends on: TOPIC_PATTERNS +
// extractTopics). Preserves DIM=34, the block layout, L2 normalization, the
// OpenAI semantic-embed call shape, and every field name the JS read.
#include "pulse/services/embedding_service.hpp"
#include "pulse/services/http_client.hpp"

#include "pulse/config.hpp"
#include "pulse/logger.hpp"
#include "pulse/bson_json.hpp"

#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <unordered_set>

namespace pulse {

namespace {

// ── TOPIC_PATTERNS — 22 categories, in declaration order (matches the JS
//    Object.keys(InterestProfiler.TOPIC_PATTERNS) order exactly). embeddingService
//    only needs the topic key + its keyword list; popularity is unused here. ──
struct TopicPattern {
  const char* topic;
  std::vector<std::string> keywords;
};

const std::vector<TopicPattern>& topicPatterns() {
  static const std::vector<TopicPattern> kPatterns = {
    {"tech", {"tech", "code", "programming", "developer", "software", "ai",
              "startup", "algorithm", "api", "javascript", "python", "react",
              "node", "database", "cloud", "devops", "ml", "machine learning",
              "silicon valley", "hackathon", "open source", "github"}},
    {"gaming", {"game", "gaming", "ps5", "xbox", "nintendo", "steam", "esports",
                "valorant", "fortnite", "minecraft", "gamer", "twitch", "streamer",
                "rpg", "fps", "mmorpg", "gta", "zelda", "elden ring", "playstation"}},
    {"crypto", {"crypto", "bitcoin", "ethereum", "blockchain", "nft", "web3",
                "defi", "token", "mining", "btc", "eth", "solana", "altcoin",
                "moon", "hodl", "wallet", "doge", "memecoin"}},
    {"lifestyle", {"life", "lifestyle", "daily", "routine", "morning", "wellness",
                   "selfcare", "mindfulness", "minimalist", "productivity", "habit",
                   "journal", "gratitude", "balance", "hustle", "grind"}},
    {"memes", {"meme", "lol", "funny", "joke", "humor", "shitpost", "dank",
               "bruh", "no cap", "sus", "based", "ratio", "slay"}},
    {"news", {"breaking", "news", "update", "announced", "reported", "politics",
              "election", "government", "economy", "world", "crisis", "headline"}},
    {"sports", {"game", "match", "score", "team", "player", "win", "lost",
                "championship", "league", "nba", "nfl", "soccer", "football",
                "cricket", "ipl", "basketball", "tennis", "f1", "formula"}},
    {"music", {"music", "song", "album", "concert", "artist", "band", "spotify",
               "playlist", "producer", "beat", "lyric", "rap", "hiphop", "pop",
               "indie", "edm", "vinyl", "guitar", "piano"}},
    {"food", {"food", "recipe", "cooking", "restaurant", "meal", "delicious",
              "chef", "baking", "cuisine", "brunch", "foodie", "yummy",
              "homemade", "pasta", "sushi", "pizza", "vegan"}},
    {"travel", {"travel", "trip", "vacation", "explore", "adventure", "wanderlust",
                "destination", "flight", "hotel", "backpack", "roadtrip", "beach",
                "mountain", "europe", "asia", "bali", "paris", "tokyo"}},
    {"fashion", {"fashion", "style", "outfit", "ootd", "clothing", "designer",
                 "trend", "drip", "thrift", "vintage", "sneakers", "streetwear",
                 "luxury", "accessories", "makeup", "beauty"}},
    {"fitness", {"fitness", "gym", "workout", "exercise", "training", "muscle",
                 "cardio", "protein", "bodybuilding", "yoga", "crossfit", "run",
                 "marathon", "gains", "lift", "health", "diet", "bulk", "cut"}},
    {"pets", {"pet", "dog", "cat", "puppy", "kitten", "doggo", "pupper",
              "animal", "rescue", "adopt", "furry", "paw", "cute", "floof",
              "birb", "hamster", "bunny"}},
    {"movies", {"movie", "film", "cinema", "director", "actor", "oscar",
                "netflix", "disney", "marvel", "dc", "horror", "thriller",
                "documentary", "screening", "premiere", "review", "rating"}},
    {"anime", {"anime", "manga", "otaku", "waifu", "naruto", "one piece",
               "attack on titan", "jjk", "demon slayer", "cosplay",
               "sub", "dub", "weeb", "kawaii", "isekai", "shonen"}},
    {"education", {"learn", "education", "study", "school", "university", "college",
                   "course", "tutorial", "lecture", "homework", "exam", "degree",
                   "scholarship", "graduate", "phd", "research"}},
    {"science", {"science", "physics", "chemistry", "biology", "space", "nasa",
                 "quantum", "experiment", "discovery", "research", "atom",
                 "telescope", "dinosaur", "evolution", "dna", "lab"}},
    {"relationships", {"love", "relationship", "dating", "crush", "couple", "breakup",
                       "marriage", "wedding", "boyfriend", "girlfriend", "single",
                       "toxic", "red flag", "green flag", "situationship"}},
    {"cars", {"car", "auto", "vehicle", "drive", "horsepower", "engine",
              "supercar", "tesla", "bmw", "mercedes", "toyota", "jdm",
              "turbo", "drift", "exhaust", "modification", "tuning"}},
    {"photography", {"photo", "photography", "camera", "lens", "portrait", "landscape",
                     "lightroom", "editing", "composition", "exposure", "bokeh",
                     "golden hour", "street photography", "macro", "drone"}},
    {"cooking", {"cook", "bake", "recipe", "kitchen", "ingredient", "homemade",
                 "meal prep", "seasoning", "oven", "grill", "stir fry",
                 "sourdough", "ferment", "sauce", "sautee"}},
    {"politics", {"politics", "election", "vote", "democrat", "republican", "congress",
                  "senate", "president", "policy", "law", "rights", "protest",
                  "activism", "campaign", "liberal", "conservative"}},
  };
  return kPatterns;
}

std::string toLower(const std::string& s) {
  std::string out(s.size(), '\0');
  std::transform(s.begin(), s.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// JS String.prototype.includes (substring containment).
inline bool strIncludes(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) return true;  // matches JS ''.includes behaviour (always true)
  return haystack.find(needle) != std::string::npos;
}

// String.prototype.replace('#', '') — removes only the FIRST '#' (JS replace with
// a string pattern replaces the first occurrence only).
std::string stripFirstHash(const std::string& s) {
  auto pos = s.find('#');
  if (pos == std::string::npos) return s;
  return s.substr(0, pos) + s.substr(pos + 1);
}

// Parse an ISO-8601 / Date-parseable timestamp into epoch milliseconds, matching
// `new Date(post.createdAt).getTime()`. Accepts a JSON number (already millis),
// or a string. Returns NaN-equivalent via the bool out-param on failure.
double parseDateMillis(const Json::Value& createdAt, bool& ok) {
  ok = true;
  if (createdAt.isNumeric()) {
    return createdAt.asDouble();
  }
  if (createdAt.isString()) {
    const std::string s = createdAt.asString();
    if (s.empty()) { ok = false; return 0.0; }
    // Parse "YYYY-MM-DDTHH:MM:SS(.fff)?(Z|+hh:mm)?" — the ISO form Mongoose/JSON
    // emits. We compute UTC epoch millis via timegm-equivalent.
    std::tm tm{};
    int year = 0, mon = 0, day = 0, hour = 0, min = 0; double sec = 0.0;
    int n = std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%lf", &year, &mon, &day, &hour, &min, &sec);
    if (n < 3) {
      // Try plain date "YYYY-MM-DD".
      n = std::sscanf(s.c_str(), "%d-%d-%d", &year, &mon, &day);
      if (n < 3) { ok = false; return 0.0; }
    }
    tm.tm_year = year - 1900;
    tm.tm_mon  = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min  = min;
    tm.tm_sec  = static_cast<int>(sec);
#if defined(_WIN32)
    std::time_t t = _mkgmtime(&tm);
#else
    std::time_t t = timegm(&tm);
#endif
    if (t == static_cast<std::time_t>(-1)) { ok = false; return 0.0; }
    double ms = static_cast<double>(t) * 1000.0 + (sec - static_cast<int>(sec)) * 1000.0;
    return ms;
  }
  ok = false;
  return 0.0;
}

double numOr(const Json::Value& v, const char* key, double def) {
  if (v.isObject() && v.isMember(key) && v[key].isNumeric()) return v[key].asDouble();
  return def;
}

}  // namespace

EmbeddingService& EmbeddingService::instance() {
  static EmbeddingService inst;
  return inst;
}

const std::vector<std::string>& EmbeddingService::topicKeys() {
  static const std::vector<std::string> kKeys = [] {
    std::vector<std::string> keys;
    keys.reserve(topicPatterns().size());
    for (const auto& tp : topicPatterns()) keys.emplace_back(tp.topic);
    return keys;
  }();
  return kKeys;
}

const std::vector<std::string>& EmbeddingService::vibes() {
  static const std::vector<std::string> kVibes = {"chill", "hype", "sad", "funny", "creative"};
  return kVibes;
}

EmbeddingService::EmbeddingService() {
  // provider = (process.env.EMBED_PROVIDER || 'feature').toLowerCase()
  std::string p = config().env("EMBED_PROVIDER", "feature");
  if (p.empty()) p = "feature";
  provider_ = toLower(p);
}

// ── l2normalize ─────────────────────────────────────────────────────────────
std::vector<double> EmbeddingService::l2normalize(const std::vector<double>& v) const {
  double s = 0.0;
  for (double x : v) s += x * x;
  const double n = std::sqrt(s);
  if (n == 0.0) return v;
  std::vector<double> out;
  out.reserve(v.size());
  for (double x : v) out.push_back(x / n);
  return out;
}

// ── cosine ──────────────────────────────────────────────────────────────────
double EmbeddingService::cosine(const std::vector<double>& a, const std::vector<double>& b) const {
  if (a.empty() || b.empty() || a.size() != b.size()) return 0.0;
  double dot = 0.0;
  for (size_t i = 0; i < a.size(); ++i) dot += a[i] * b[i];
  return dot;  // both are unit vectors
}

// ── extractTopics (mirror of InterestProfiler computeTopics) ────────────────
std::vector<std::string> EmbeddingService::extractTopics(const Json::Value& post) const {
  // The embeddingService only consults membership of the 22 TOPIC_KEYS, but we
  // reproduce the full set semantics (hashtags, text, mentions, default) for
  // exact behavioural parity.
  std::vector<std::string> ordered;             // preserves first-insertion order
  std::unordered_set<std::string> seen;
  auto add = [&](const std::string& t) {
    if (seen.insert(t).second) ordered.push_back(t);
  };

  const Json::Value content = post.isObject() && post.isMember("content") ? post["content"] : Json::Value();

  // ── From hashtags ──
  if (content.isObject() && content.isMember("hashtags") && content["hashtags"].isArray()) {
    for (const auto& tagV : content["hashtags"]) {
      if (!tagV.isString()) continue;
      const std::string clean = stripFirstHash(toLower(tagV.asString()));
      add(clean);
      for (const auto& tp : topicPatterns()) {
        bool matched = false;
        for (const auto& kw : tp.keywords) {
          if (strIncludes(clean, kw) || strIncludes(kw, clean)) { matched = true; break; }
        }
        if (matched) add(tp.topic);
      }
    }
  }

  // ── From text content ──
  std::string text;
  if (content.isObject() && content.isMember("text") && content["text"].isString())
    text = content["text"].asString();
  const std::string lower = toLower(text);
  for (const auto& tp : topicPatterns()) {
    int matchCount = 0;
    for (const auto& kw : tp.keywords) {
      if (strIncludes(lower, kw)) matchCount++;
    }
    if (matchCount >= 1) add(tp.topic);
  }

  // ── From mentions ──
  if (content.isObject() && content.isMember("mentions") && content["mentions"].isArray()
      && !content["mentions"].empty()) {
    add("social");
  }

  if (ordered.empty()) return {"general"};  // CONFIG.DEFAULT_TOPICS
  return ordered;
}

// ── topicBlock ──────────────────────────────────────────────────────────────
std::vector<double> EmbeddingService::topicBlock(const Json::Value& post) const {
  const auto& keys = topicKeys();
  std::vector<double> block(keys.size(), 0.0);
  std::unordered_set<std::string> topics;
  // try { ... } catch { topics = new Set(); } — extractTopics here never throws,
  // but we keep the empty-set fallback semantics implicitly.
  for (const auto& t : extractTopics(post)) topics.insert(t);
  for (size_t i = 0; i < keys.size(); ++i) {
    if (topics.count(keys[i])) block[i] = 1.0;
  }
  return block;
}

// ── vibeBlock ───────────────────────────────────────────────────────────────
std::vector<double> EmbeddingService::vibeBlock(const Json::Value& post) const {
  const auto& VIBES = vibes();
  std::vector<double> block(VIBES.size(), 0.0);

  // const vs = post.vibeScore || (post.vibe ? { [post.vibe]: 1 } : null);
  // We model `vs` as: present?, and a lookup of vibe -> score.
  bool hasVs = false;
  Json::Value vsObj(Json::objectValue);  // map vibe -> number
  const std::string vibeStr =
      (post.isObject() && post.isMember("vibe") && post["vibe"].isString()) ? post["vibe"].asString() : "";

  // JS: const vs = post.vibeScore || (post.vibe ? { [post.vibe]: 1 } : null).
  // Any present object (even {}) is truthy in JS, so it wins over the vibe
  // fallback. We treat a present, non-null vibeScore object as truthy.
  if (post.isObject() && post.isMember("vibeScore") && post["vibeScore"].isObject()) {
    hasVs = true;
    vsObj = post["vibeScore"];
  } else if (!vibeStr.empty()) {
    hasVs = true;
    vsObj[vibeStr] = 1;
  }

  if (hasVs) {
    double maxV = 0.0;
    for (const auto& v : VIBES) {
      const double val = numOr(vsObj, v.c_str(), 0.0);
      maxV = std::max(maxV, val);
    }
    for (size_t i = 0; i < VIBES.size(); ++i) {
      const double val = numOr(vsObj, VIBES[i].c_str(), 0.0);
      block[i] = maxV > 0.0 ? val / maxV : 0.0;
    }
  } else if (!vibeStr.empty()) {
    // Unreachable when vibeStr is non-empty (the `||` above produced an object),
    // but ported faithfully for the case where vibe is falsy yet truthy-checked.
    auto it = std::find(VIBES.begin(), VIBES.end(), vibeStr);
    if (it != VIBES.end()) block[std::distance(VIBES.begin(), it)] = 1.0;
  }
  return block;
}

// ── mediaBlock ──────────────────────────────────────────────────────────────
std::vector<double> EmbeddingService::mediaBlock(const Json::Value& post) const {
  std::vector<double> block(kMediaDims, 0.0);  // image, video, gif
  const Json::Value content = post.isObject() && post.isMember("content") ? post["content"] : Json::Value();
  if (content.isObject() && content.isMember("media") && content["media"].isArray()) {
    for (const auto& m : content["media"]) {
      if (!m.isObject() || !m.isMember("type") || !m["type"].isString()) continue;
      const std::string type = m["type"].asString();
      if (type == "image") block[0] = 1.0;
      if (type == "video") block[1] = 1.0;
      if (type == "gif")   block[2] = 1.0;
    }
  }
  return block;
}

// ── metaBlock ───────────────────────────────────────────────────────────────
std::vector<double> EmbeddingService::metaBlock(const Json::Value& post) const {
  const Json::Value stats = post.isObject() && post.isMember("stats") ? post["stats"] : Json::Value();
  // Quality proxy: log-scaled engagement (caps runaway virality influence).
  const double likes    = stats.isObject() ? numOr(stats, "likes", 0.0) : 0.0;
  const double comments = stats.isObject() ? numOr(stats, "comments", 0.0) : 0.0;
  const double shares   = stats.isObject() ? numOr(stats, "shares", 0.0) : 0.0;
  const double eng = likes + comments * 2.0 + shares * 3.0;
  const double quality = std::min(1.0, std::log10(eng + 1.0) / 4.0);

  // Recency: 1 now -> 0 at ~7d. ageH = (Date.now() - createdAt) / 3600000.
  double ageH = 0.0;
  if (post.isObject() && post.isMember("createdAt") && !post["createdAt"].isNull()) {
    bool ok = false;
    const double createdMs = parseDateMillis(post["createdAt"], ok);
    if (ok) {
      const double nowMs = static_cast<double>(bsonjson::nowMillis());
      ageH = (nowMs - createdMs) / 3600000.0;
    }
  }
  const double recency = std::max(0.0, 1.0 - ageH / 168.0);

  const Json::Value content = post.isObject() && post.isMember("content") ? post["content"] : Json::Value();
  int mediaLen = 0;
  if (content.isObject() && content.isMember("media") && content["media"].isArray())
    mediaLen = static_cast<int>(content["media"].size());
  const double hasMedia = mediaLen > 0 ? 1.0 : 0.0;

  int len = 0;
  if (content.isObject() && content.isMember("text") && content["text"].isString())
    len = static_cast<int>(content["text"].asString().size());
  const double lengthBucket = std::min(1.0, static_cast<double>(len) / 500.0);

  return {quality, recency, hasMedia, lengthBucket};
}

// ── featureVector ───────────────────────────────────────────────────────────
std::vector<double> EmbeddingService::featureVector(const Json::Value& post) const {
  std::vector<double> v;
  v.reserve(kDim);
  for (double x : topicBlock(post)) v.push_back(x);
  for (double x : vibeBlock(post))  v.push_back(x);
  for (double x : mediaBlock(post)) v.push_back(x);
  for (double x : metaBlock(post))  v.push_back(x);
  return l2normalize(v);
}

// ── reelToEmbeddable ────────────────────────────────────────────────────────
Json::Value EmbeddingService::reelToEmbeddable(const Json::Value& reel) const {
  Json::Value out(Json::objectValue);
  Json::Value content(Json::objectValue);

  content["text"] = (reel.isObject() && reel.isMember("caption") && reel["caption"].isString())
                        ? reel["caption"] : Json::Value("");
  Json::Value hashtags(Json::arrayValue);
  if (reel.isObject() && reel.isMember("hashtags") && reel["hashtags"].isArray())
    hashtags = reel["hashtags"];
  content["hashtags"] = hashtags;

  Json::Value media(Json::arrayValue);
  Json::Value videoEntry(Json::objectValue);
  videoEntry["type"] = "video";
  media.append(videoEntry);
  content["media"] = media;

  out["content"] = content;
  out["vibe"]      = (reel.isObject() && reel.isMember("vibe")) ? reel["vibe"] : Json::Value();
  out["vibeScore"] = (reel.isObject() && reel.isMember("vibeScore")) ? reel["vibeScore"] : Json::Value();
  out["stats"]     = (reel.isObject() && reel.isMember("stats") && reel["stats"].isObject())
                         ? reel["stats"] : Json::Value(Json::objectValue);
  out["createdAt"] = (reel.isObject() && reel.isMember("createdAt")) ? reel["createdAt"] : Json::Value();
  return out;
}

// ── reelVector ──────────────────────────────────────────────────────────────
std::vector<double> EmbeddingService::reelVector(const Json::Value& reel) const {
  return featureVector(reelToEmbeddable(reel));
}

// ── userVector ──────────────────────────────────────────────────────────────
std::vector<double> EmbeddingService::userVector(const Json::Value& args) const {
  const auto& keys = topicKeys();
  const auto& VIBES = vibes();
  std::vector<double> acc(kDim, 0.0);

  // for (const p of engagedPosts) acc += featureVector(p)
  if (args.isObject() && args.isMember("engagedPosts") && args["engagedPosts"].isArray()) {
    for (const auto& p : args["engagedPosts"]) {
      const std::vector<double> fv = featureVector(p);
      for (int i = 0; i < kDim; ++i) acc[i] += fv[i];
    }
  }

  // Fold in explicit topic affinities (object) onto the topic block.
  if (args.isObject() && args.isMember("topicAffinities") && args["topicAffinities"].isObject()) {
    const Json::Value& ta = args["topicAffinities"];
    for (size_t i = 0; i < keys.size(); ++i) {
      const double a = (ta.isMember(keys[i]) && ta[keys[i]].isNumeric()) ? ta[keys[i]].asDouble() : 0.0;
      acc[i] += a;
    }
  }

  // Fold in vibe strands (SocialDNA) onto the vibe block.
  if (args.isObject() && args.isMember("vibeStrands") && args["vibeStrands"].isObject()) {
    const Json::Value& vs = args["vibeStrands"];
    double maxV = 0.0;
    for (const auto& v : VIBES) {
      const double val = (vs.isMember(v) && vs[v].isNumeric()) ? vs[v].asDouble() : 0.0;
      maxV = std::max(maxV, val);
    }
    for (size_t i = 0; i < VIBES.size(); ++i) {
      const double val = (vs.isMember(VIBES[i]) && vs[VIBES[i]].isNumeric()) ? vs[VIBES[i]].asDouble() : 0.0;
      acc[keys.size() + i] += maxV > 0.0 ? val / maxV : 0.0;
    }
  }

  return l2normalize(acc);
}

// ── embedPost ───────────────────────────────────────────────────────────────
std::vector<double> EmbeddingService::embedPost(const Json::Value& post) const {
  const std::vector<double> feat = featureVector(post);
  if (provider_ == "feature") return feat;

  // Else try semantic embedding; on success or failure the feature vector
  // remains the index-compatible one that is returned.
  try {
    std::string text;
    if (post.isObject() && post.isMember("content") && post["content"].isObject()
        && post["content"].isMember("text") && post["content"]["text"].isString())
      text = post["content"]["text"].asString();
    const auto semantic = semanticEmbed(text);
    (void)semantic;  // kept on a side channel in JS; not used for the index
    return feat;
  } catch (...) {
    return feat;
  }
}

// ── semanticEmbed ───────────────────────────────────────────────────────────
std::optional<std::vector<double>> EmbeddingService::semanticEmbed(const std::string& text) const {
  if (text.empty() || provider_ == "feature") return std::nullopt;

  if (provider_ == "openai") {
    const std::string apiKey = config().env("OPENAI_API_KEY", "");
    if (apiKey.empty()) return std::nullopt;

    try {
      auto client = drogon::HttpClient::newHttpClient("https://api.openai.com");

      Json::Value body(Json::objectValue);
      // model: process.env.EMBED_MODEL || 'text-embedding-3-small'
      std::string model = config().env("EMBED_MODEL", "text-embedding-3-small");
      if (model.empty()) model = "text-embedding-3-small";
      body["model"] = model;
      // input: text.slice(0, 8000)
      body["input"] = text.size() > 8000 ? text.substr(0, 8000) : text;

      auto req = drogon::HttpRequest::newHttpJsonRequest(body);
      req->setMethod(drogon::Post);
      req->setPath("/v1/embeddings");
      req->addHeader("Authorization", "Bearer " + apiKey);
      req->setContentTypeCode(drogon::CT_APPLICATION_JSON);

      auto [result, resp] = client->sendRequest(
          req, pulse::services::outboundHttpTimeoutSeconds());
      if (result != drogon::ReqResult::Ok || !resp) return std::nullopt;

      const auto json = resp->getJsonObject();
      if (!json) return std::nullopt;

      // json?.data?.[0]?.embedding || null
      const Json::Value& root = *json;
      if (!root.isObject() || !root.isMember("data") || !root["data"].isArray()
          || root["data"].empty())
        return std::nullopt;
      const Json::Value& first = root["data"][0u];
      if (!first.isObject() || !first.isMember("embedding") || !first["embedding"].isArray())
        return std::nullopt;

      std::vector<double> embedding;
      embedding.reserve(first["embedding"].size());
      for (const auto& x : first["embedding"]) {
        if (x.isNumeric()) embedding.push_back(x.asDouble());
      }
      if (embedding.empty()) return std::nullopt;
      return embedding;
    } catch (const std::exception& e) {
      log::warn("embeddingService.semanticEmbed failed: {}", e.what());
      return std::nullopt;
    }
  }

  // Else (no matching provider / no key): return null.
  return std::nullopt;
}

}  // namespace pulse

// ── Module-export adapter (mirrors `require('./embeddingService').featureVector`) ──
//
// vector_retrieval_service.cc calls `pulse::services::embedding::featureVector(p)`
// expecting a Json::Value array (its local cosine() takes Json::Value args). The
// class method returns std::vector<double>; this free function bridges the two by
// delegating to the singleton and serializing the DIM-length vector to a JSON
// array. Definition lives here so the embeddingService port owns it.
namespace pulse::services::embedding {

Json::Value featureVector(const Json::Value& post) {
  const std::vector<double> v = pulse::embedding().featureVector(post);
  Json::Value out(Json::arrayValue);
  for (double d : v) out.append(d);
  return out;
}

} // namespace pulse::services::embedding
