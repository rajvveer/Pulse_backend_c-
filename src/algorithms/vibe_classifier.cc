// vibe_classifier.cc — C++ port of VibeClassifier v2.0.
//
// Faithful to the JS, WITH the confirmed HIGH fix: strong-keyword matching now
// uses word boundaries (single words) / anchored phrase negation instead of the
// old `text.includes(keyword)` substring test that misfired ('goat' inside
// 'scapegoat', 'zen' inside 'frozen'). Weak keywords match whole tokens too.
//
// Input  JSON: { "text": "...", "hashtags": ["..."], "mediaTypes": ["video"...] }
// Output JSON: { "vibe", "vibeScore":{chill,hype,sad,funny,creative},
//                "confidence", "secondaryVibe"|null, "secondaryConfidence" }
#include "common.hpp"
#include <map>
#include <set>

namespace pulse {

namespace {

struct VibePattern {
  std::vector<std::string> strong;
  std::vector<std::string> weak;
  std::vector<std::string> emojis;   // raw UTF-8 byte sequences
  std::vector<std::string> hashtags;
};

const std::vector<std::string> VIBES = {"chill", "hype", "sad", "funny", "creative"};

const std::set<std::string> NEGATION_WORDS = {
  "not", "n't", "no", "never", "neither", "nobody", "nothing", "nowhere", "nor",
  "hardly", "barely", "scarcely", "don't", "doesn't", "didn't", "isn't",
  "aren't", "wasn't", "weren't", "won't", "wouldn't", "shouldn't", "couldn't",
  "can't", "cannot"
};
const int NEGATION_WINDOW = 3;

const std::map<std::string, VibePattern>& patterns() {
  static const std::map<std::string, VibePattern> P = {
    {"chill", {
      {"peaceful", "serene", "meditation", "zen", "tranquil", "cozy vibes", "lazy sunday", "just chilling"},
      {"relax", "calm", "cozy", "vibes", "sunset", "sunrise", "coffee", "tea", "lazy", "sunday",
       "chill", "quiet", "nature", "beach", "waves", "rain", "sleep", "rest", "breeze", "cottage",
       "lo-fi", "lofi", "ambient", "soothing"},
      {"\xF0\x9F\x98\x8C", "\xE2\x98\x95", "\xF0\x9F\x8C\x85", "\xF0\x9F\x8C\x84", "\xF0\x9F\x8F\x96\xEF\xB8\x8F",
       "\xF0\x9F\x8C\x8A", "\xF0\x9F\x8C\xBF", "\xF0\x9F\x8D\x83", "\xF0\x9F\x92\xA4", "\xF0\x9F\xA7\x98",
       "\xF0\x9F\x95\xAF\xEF\xB8\x8F", "\xF0\x9F\xAB\x96"},
      {"chill", "vibes", "relax", "peaceful", "cozy", "mood", "aesthetic", "lofi", "zen"}
    }},
    {"hype", {
      {"insane", "legendary", "goat", "banger", "unreal", "lets go", "let's go", "lfg", "sickkk", "no cap"},
      {"amazing", "incredible", "crazy", "wild", "party", "excited", "hyped", "lit", "fire", "best",
       "epic", "winning", "dope", "slaps", "bussin", "peak", "massive", "electric", "charged", "pumped"},
      {"\xF0\x9F\x94\xA5", "\xF0\x9F\x9A\x80", "\xF0\x9F\x92\xAF", "\xE2\x9A\xA1", "\xF0\x9F\x8E\x89",
       "\xF0\x9F\x99\x8C", "\xF0\x9F\x92\xAA", "\xF0\x9F\x8F\x86", "\xE2\x9C\xA8", "\xF0\x9F\x92\xA5",
       "\xF0\x9F\xAB\xA1", "\xF0\x9F\xA4\xAF"},
      {"viral", "trending", "fire", "lit", "hype", "epic", "goat", "bussin", "peak"}
    }},
    {"sad", {
      {"heartbreak", "depressed", "sobbing", "devastated", "heartbroken", "crying myself", "falling apart", "can't stop crying"},
      {"sad", "miss", "crying", "alone", "lonely", "hurt", "feelings", "feels", "broken", "pain",
       "tears", "gone", "lost", "sorry", "regret", "memories", "goodbye", "numb", "empty", "heavy",
       "tired of", "drained", "struggling"},
      {"\xF0\x9F\x98\xA2", "\xF0\x9F\x98\xAD", "\xF0\x9F\x92\x94", "\xF0\x9F\xA5\xBA", "\xF0\x9F\x98\x94",
       "\xF0\x9F\x98\x9E", "\xF0\x9F\x96\xA4", "\xF0\x9F\x92\xA7", "\xF0\x9F\xA5\x80", "\xF0\x9F\x98\xBF"},
      {"feels", "deep", "sad", "mood", "relatable", "heartbreak", "vent", "overthinking"}
    }},
    {"funny", {
      {"hilarious", "lmfao", "i'm dead", "i'm dying", "rofl", "no way bruh", "comedy gold", "crying laughing"},
      {"lol", "lmao", "funny", "joke", "meme", "dead", "dying", "haha", "comedy", "laughing", "pranked",
       "clown", "bruh", "no way", "bruhhh", "ong", "nah fr", "satire", "roast", "sarcasm", "trolling"},
      {"\xF0\x9F\x98\x82", "\xF0\x9F\xA4\xA3", "\xF0\x9F\x92\x80", "\xF0\x9F\x98\x86", "\xF0\x9F\xA4\xA1",
       "\xF0\x9F\x98\xB9", "\xF0\x9F\xA4\xAA", "\xF0\x9F\x98\x9C", "\xF0\x9F\xAB\xA0", "\xE2\x98\xA0\xEF\xB8\x8F"},
      {"funny", "meme", "comedy", "lol", "humor", "jokes", "roast", "sarcasm"}
    }},
    {"creative", {
      {"masterpiece", "portfolio", "composition", "handmade", "original work", "my creation", "just finished painting"},
      {"art", "design", "created", "made", "painted", "drew", "music", "wrote", "built", "project",
       "craft", "diy", "photography", "film", "edit", "animation", "sketch", "illustration",
       "digital art", "sculpture", "collage", "remix"},
      {"\xF0\x9F\x8E\xA8", "\xE2\x9C\x8F\xEF\xB8\x8F", "\xF0\x9F\x8E\xB5", "\xF0\x9F\x8E\xAC", "\xF0\x9F\x93\xB8",
       "\xE2\x9C\xA8", "\xF0\x9F\x92\xA1", "\xF0\x9F\x96\x8C\xEF\xB8\x8F", "\xF0\x9F\x8E\xAD", "\xF0\x9F\x8E\xA7",
       "\xF0\x9F\x8E\xB9", "\xF0\x9F\x93\xB7"},
      {"art", "create", "design", "music", "diy", "photography", "creative", "digital", "illustration"}
    }}
  };
  return P;
}

const std::map<std::string, std::vector<std::string>>& ngrams() {
  static const std::map<std::string, std::vector<std::string>> N = {
    {"chill", {"lazy sunday", "cozy vibes", "peaceful morning", "good vibes only", "just vibing", "stay calm"}},
    {"hype", {"let's go", "lets go", "no cap", "on god", "for real", "goes hard", "so fire", "too lit"}},
    {"sad", {"falling apart", "can't sleep", "miss you", "all alone", "broke my heart", "feel like crying"}},
    {"funny", {"i'm dead", "i can't", "no way", "bro what", "nah fr", "comedy gold", "not the"}},
    {"creative", {"work in progress", "just finished", "original work", "behind the scenes", "sneak peek"}}
  };
  return N;
}

struct Weights { double strong = 2.5, weak = 1.0, ngram = 3.0, emoji = 1.5, emojiStack = 0.8, hashtag = 2.0; };
const Weights W;

struct Tok { std::string word; bool negated; };

std::vector<Tok> tokenizeWithNegation(const std::string& text) {
  std::vector<Tok> tokens;
  int countdown = 0;
  for (const std::string& w : tokenize(text)) {
    bool isNeg = NEGATION_WORDS.count(w) > 0 ||
                 (w.size() >= 3 && w.compare(w.size() - 3, 3, "n't") == 0);
    if (isNeg) { countdown = NEGATION_WINDOW; tokens.push_back({w, false}); }
    else { tokens.push_back({w, countdown > 0}); if (countdown > 0) countdown--; }
  }
  return tokens;
}

} // namespace

Json classifyVibe(const std::string& textRaw, const std::vector<std::string>& hashtagsRaw,
                  const std::vector<std::string>& mediaTypes) {
  std::map<std::string, double> scores = {{"chill",0},{"hype",0},{"sad",0},{"funny",0},{"creative",0}};
  std::string text = toLower(textRaw);
  std::vector<std::string> hashtags;
  for (auto& h : hashtagsRaw) {
    std::string t = toLower(h);
    if (!t.empty() && t[0] == '#') t = t.substr(1);
    hashtags.push_back(t);
  }

  // 1. N-gram phrases (highest priority)
  for (auto& kv : ngrams())
    for (auto& phrase : kv.second)
      if (containsSub(text, phrase)) scores[kv.first] += W.ngram;

  auto tokens = tokenizeWithNegation(text);

  for (auto& kv : patterns()) {
    const std::string& vibe = kv.first;
    const VibePattern& p = kv.second;

    // 2a. Strong keywords — FIX: word-boundary single words, anchored-phrase negation.
    for (const std::string& keyword : p.strong) {
      bool isPhrase = keyword.find(' ') != std::string::npos;
      bool matched = false, negated = false;
      if (!isPhrase) {
        for (auto& t : tokens) {
          if (cleanToken(t.word) == keyword) { matched = true; if (t.negated) negated = true; }
        }
      } else {
        if (containsSub(text, keyword)) {
          matched = true;
          std::string firstWord = keyword.substr(0, keyword.find(' '));
          for (auto& t : tokens) { if (cleanToken(t.word) == firstWord) { negated = t.negated; break; } }
        }
      }
      if (matched) scores[vibe] += negated ? -W.strong * 0.5 : W.strong;
    }

    // 2b. Weak keywords — whole-token match (or substring for multi-word).
    for (const std::string& keyword : p.weak) {
      if (keyword.find(' ') != std::string::npos) {
        if (containsSub(text, keyword)) scores[vibe] += W.weak;
        continue;
      }
      for (auto& t : tokens)
        if (cleanToken(t.word) == keyword)
          scores[vibe] += t.negated ? -W.weak * 0.3 : W.weak;
    }

    // 3. Emoji detection with stacking (count raw UTF-8 byte occurrences).
    for (const std::string& emoji : p.emojis) {
      int count = countOccurrences(text, emoji);
      if (count > 0) scores[vibe] += W.emoji + (count - 1) * W.emojiStack;
    }

    // 4. Hashtag matching.
    for (const std::string& tag : p.hashtags)
      if (std::find(hashtags.begin(), hashtags.end(), tag) != hashtags.end())
        scores[vibe] += W.hashtag;
  }

  // 5. Media-type influence.
  bool hasVideo = false, hasImage = false;
  for (auto& m : mediaTypes) { if (m == "video") hasVideo = true; if (m == "image") hasImage = true; }
  if (hasVideo) { scores["funny"] += 0.4; scores["hype"] += 0.4; }
  if (hasImage) { scores["creative"] += 0.3; scores["chill"] += 0.2; }

  // 6. Clamp negatives.
  for (auto& v : VIBES) scores[v] = std::max(0.0, scores[v]);

  // 7. Primary / secondary.
  std::vector<std::pair<std::string, double>> sorted(scores.begin(), scores.end());
  std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.second > b.second; });
  auto& top = sorted[0];
  auto& second = sorted[1];

  // 8. Entropy confidence.
  std::vector<double> vals; for (auto& v : VIBES) vals.push_back(scores[v]);
  double conf = 0;
  { double e = normalizedEntropy(vals); conf = std::round((1.0 - e) * 100) / 100.0; }

  std::string primary = top.second >= 1.0 ? top.first : "general";
  bool hasSecondary = second.second >= 0.5;
  double secConf = (top.second > 0 && second.second > 0)
    ? std::round((second.second / top.second) * 100) / 100.0 : 0;

  Json out = Json::object();
  out["vibe"] = primary;
  Json vs = Json::object();
  for (auto& v : VIBES) vs[v] = scores[v];
  out["vibeScore"] = vs;
  out["confidence"] = conf;
  out["secondaryVibe"] = hasSecondary ? Json(second.first) : Json(nullptr);
  out["secondaryConfidence"] = secConf;
  return out;
}

// Entry point: classify one post, or a batch.
std::string run_vibe_classify(const std::string& in) {
  Json input = Json::parse(in);

  auto extractMediaTypes = [](const Json& post) {
    std::vector<std::string> mt;
    const Json& media = post["content"]["media"];
    if (media.isArray()) for (auto& m : media.arr()) mt.push_back(m["type"].str());
    return mt;
  };
  auto extractHashtags = [](const Json& post) {
    std::vector<std::string> hs;
    const Json& h = post["content"]["hashtags"];
    if (h.isArray()) for (auto& t : h.arr()) hs.push_back(t.str());
    return hs;
  };

  if (input.isArray()) {
    Json arr = Json::array();
    for (auto& post : input.arr()) {
      Json r = classifyVibe(post["content"]["text"].str(), extractHashtags(post), extractMediaTypes(post));
      if (post.contains("_id")) r["postId"] = post["_id"];
      arr.push_back(r);
    }
    return arr.dump();
  }

  return classifyVibe(input["content"]["text"].str(), extractHashtags(input), extractMediaTypes(input)).dump();
}

} // namespace pulse
