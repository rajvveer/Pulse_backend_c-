// mood_detector.cc — C++ port of MoodDetector v2.0.
//
// Stateless: momentum/EMA across calls (which needed instance history) is left
// to the JS wrapper, which keeps the small per-instance history and only calls
// C++ for the per-post scoring. This port covers the deterministic scoring,
// WITH the confirmed fix: the post's own content.text/hashtags are scored
// (weighted 3x) so a clearly-toned post isn't overridden by time/day biases.
//
// Input JSON: { text, hashtags:[...], nowMs, dayOfWeek, hourOfDay }
// Output JSON: { primaryMood, moodScores:{...}, confidence }
#include "common.hpp"
#include <map>

namespace pulse {
namespace {

struct MoodKw { std::vector<std::string> strong; std::vector<std::string> weak; double weight; };

const std::map<std::string, MoodKw>& moodKeywords() {
  static const std::map<std::string, MoodKw> M = {
    {"chill", {{"meditation","zen","serene","tranquil","peaceful morning"},
               {"relaxing","peaceful","calm","cozy","vibes","sunset","coffee","lazy","sunday",
                "lo-fi","lofi","ambient","soothing","breeze"}, 1.0}},
    {"hype", {{"insane","legendary","banger","lfg","sickkk","goat"},
              {"lit","amazing","incredible","crazy","wild","party","lets go","excited","hyped",
               "fire","epic","pumped"}, 1.2}},
    {"sad", {{"heartbroken","devastated","sobbing","depressed"},
             {"sad","miss","crying","alone","lonely","heartbreak","feelings","feels","broken","numb","empty"}, 1.0}},
    {"funny", {{"hilarious","lmfao","comedy gold","crying laughing"},
               {"lol","lmao","funny","joke","meme","dead","dying","haha","bruh","roast"}, 1.0}},
    {"creative", {{"masterpiece","portfolio","composition","original work"},
                  {"art","design","created","made","painted","drew","music","wrote","built","project","craft","diy"}, 1.0}}
  };
  return M;
}

// Time-of-day and day-of-week ambient nudges (small; must not override text).
const std::map<std::string, std::map<std::string, double>>& timeMoods() {
  static const std::map<std::string, std::map<std::string, double>> T = {
    {"morning",   {{"chill",0.3},{"hype",0.1},{"creative",0.2}}},
    {"afternoon", {{"hype",0.2},{"creative",0.2},{"funny",0.1}}},
    {"evening",   {{"chill",0.2},{"sad",0.1},{"creative",0.15}}},
    {"night",     {{"sad",0.2},{"chill",0.2},{"funny",0.15}}}
  };
  return T;
}
const std::map<int, std::map<std::string, double>>& dayMoods() {
  static const std::map<int, std::map<std::string, double>> D = {
    {0,{{"chill",0.3},{"sad",0.1}}}, {1,{{"sad",0.15},{"chill",0.1}}},
    {2,{{"creative",0.1}}}, {3,{{"creative",0.15},{"hype",0.05}}},
    {4,{{"hype",0.1},{"funny",0.1}}}, {5,{{"hype",0.3},{"funny",0.2}}},
    {6,{{"chill",0.15},{"hype",0.15},{"funny",0.1}}}
  };
  return D;
}

void extractMoodFromText(std::map<std::string,double>& scores, const std::string& textRaw, double multiplier) {
  std::string lower = toLower(textRaw);
  for (auto& kv : moodKeywords()) {
    const std::string& mood = kv.first;
    const MoodKw& cfg = kv.second;
    for (auto& k : cfg.strong) if (containsSub(lower, k)) scores[mood] += cfg.weight * 2.0 * multiplier;
    for (auto& k : cfg.weak)   if (containsSub(lower, k)) scores[mood] += cfg.weight * multiplier;
  }
}

double calcConfidence(std::vector<std::pair<std::string,double>>& sorted) {
  if (sorted.size() < 2) return 0;
  double top = sorted[0].second, second = sorted[1].second;
  double third = sorted.size() > 2 ? sorted[2].second : 0;
  if (top == 0) return 0;
  double primaryGap = (top - second) / top;
  double secondaryGap = second > 0 ? (second - third) / second : 0;
  double c = primaryGap * 0.7 + secondaryGap * 0.3;
  return std::round(clampd(c, 0, 1) * 100) / 100.0;
}

} // namespace

std::string run_mood_detect(const std::string& in) {
  Json input = Json::parse(in);
  std::map<std::string,double> scores = {{"chill",0},{"hype",0},{"sad",0},{"funny",0},{"creative",0}};

  // 0. Post's own text/hashtags — strongest signal (FIX).
  if (input.contains("text")) extractMoodFromText(scores, input["text"].str(), 3.0);
  const Json& tags = input["hashtags"];
  if (tags.isArray()) for (auto& t : tags.arr()) extractMoodFromText(scores, t.str(), 3.0);

  // Time-of-day.
  int hour = (int)input["hourOfDay"].num(-1);
  if (hour >= 0) {
    std::string period = (hour >= 5 && hour < 11) ? "morning"
                       : (hour >= 11 && hour < 17) ? "afternoon"
                       : (hour >= 17 && hour < 21) ? "evening" : "night";
    auto it = timeMoods().find(period);
    if (it != timeMoods().end()) for (auto& m : it->second) scores[m.first] += m.second;
  }

  // Day-of-week.
  int day = (int)input["dayOfWeek"].num(-1);
  if (day >= 0 && day <= 6) {
    auto it = dayMoods().find(day);
    if (it != dayMoods().end()) for (auto& m : it->second) scores[m.first] += m.second;
  }

  std::vector<std::pair<std::string,double>> sorted(scores.begin(), scores.end());
  std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b){ return a.second > b.second; });

  Json out = Json::object();
  out["primaryMood"] = sorted[0].first;
  Json ms = Json::object();
  for (auto& kv : scores) ms[kv.first] = kv.second;
  out["moodScores"] = ms;
  out["confidence"] = calcConfidence(sorted);
  return out.dump();
}

} // namespace pulse
