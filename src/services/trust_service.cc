// trust_service.cc — implementation. Ports src/services/trustService.js.
#include "pulse/services/trust_service.hpp"
#include "pulse/cache.hpp"
#include "pulse/config.hpp"

#include <regex>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <unordered_set>

namespace pulse {

namespace {

// ── Engagement-bait patterns (down-rank, don't remove) ──
// Verbatim from trustService.js BAIT_PATTERNS, in order. ECMAScript flavor +
// case-insensitive, matching the JS /.../i regex literals.
const std::vector<std::regex>& baitPatterns() {
  static const std::vector<std::regex> patterns = [] {
    auto f = std::regex::ECMAScript | std::regex::icase;
    std::vector<std::regex> v;
    v.emplace_back(R"(\blike (this |and )?(if|when|to)\b)", f);
    v.emplace_back(R"(\bcomment (below| "?\w+"?|your)\b)", f);
    v.emplace_back(R"(\bfollow (for follow|me back|4 follow|f4f)\b)", f);
    v.emplace_back(R"(\btag (a friend|someone|\d+ (friends|people))\b)", f);
    v.emplace_back(R"(\bshare (this |to )?(if|and|for)\b)", f);
    v.emplace_back(R"(\b(like|share|follow) (and|to) (win|enter|get)\b)", f);
    v.emplace_back(R"(\bdouble tap if\b)", f);
    v.emplace_back(R"(\bswipe up\b)", f);
    v.emplace_back(R"(\bcheck (my |the )?(bio|link in bio)\b)", f);
    v.emplace_back(R"(\b(repost|retweet) (if|to|for)\b)", f);
    return v;
  }();
  return patterns;
}

// Mirror `(author?._id || author)?.toString()`. Accepts an author object with
// an `_id` (string, or {$oid}/{_id} shaped), or a bare string id. Returns "" if
// nothing usable (JS: undefined id -> falls back to the 0.5 default upstream).
std::string resolveId(const Json::Value& v) {
  if (v.isString()) return v.asString();
  if (v.isObject()) {
    const Json::Value& id = v["_id"];
    if (id.isString()) return id.asString();
    if (id.isObject()) {
      // Tolerate { $oid: "..." } or nested { _id: "..." } shapes.
      if (id["$oid"].isString()) return id["$oid"].asString();
      if (id["_id"].isString()) return id["_id"].asString();
    }
    // No _id present: JS `author?._id || author` would yield the author object,
    // whose .toString() is "[object Object]" — not a usable id. Treat as empty.
  }
  return "";
}

// Numeric stat read with JS `stats.x || 0` semantics (missing/non-number -> 0).
double statNum(const Json::Value& stats, const char* key) {
  if (!stats.isObject()) return 0;
  const Json::Value& v = stats[key];
  if (v.isNumeric()) return v.asDouble();
  return 0;
}

// Parse a Date-like value to epoch milliseconds. The JS did
// `new Date(author.createdAt).getTime()`. Inputs here are JSON: an ISO-8601
// string (how bson_json renders dates) or a numeric epoch (ms). Returns nullopt
// when not parseable (JS NaN path is avoided upstream by the createdAt guard).
bool parseToMillis(const Json::Value& v, long long& outMs) {
  if (v.isNumeric()) { outMs = static_cast<long long>(v.asDouble()); return true; }
  if (!v.isString()) return false;
  std::string s = v.asString();
  if (s.empty()) return false;
  std::tm tm{};
  int year = 0, mon = 0, day = 0, hour = 0, min = 0;
  double sec = 0;
  // ISO-8601: YYYY-MM-DDTHH:MM:SS(.fff)?Z — the format bson_json emits.
  if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%lf",
                  &year, &mon, &day, &hour, &min, &sec) >= 3) {
    tm.tm_year = year - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = static_cast<int>(sec);
#if defined(_WIN32)
    time_t t = _mkgmtime(&tm);
#else
    time_t t = timegm(&tm);
#endif
    if (t == static_cast<time_t>(-1)) return false;
    outMs = static_cast<long long>(t) * 1000 +
            static_cast<long long>((sec - std::floor(sec)) * 1000);
    return true;
  }
  return false;
}

long long nowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

} // namespace

TrustService::TrustService() {
  // TRUST_TTL = parseInt(process.env.TRUST_TTL_SEC, 10) || 600
  trustTtl_ = config().envInt("TRUST_TTL_SEC", 600);
  if (trustTtl_ <= 0) trustTtl_ = 600;
}

TrustService& TrustService::instance() {
  static TrustService s;
  return s;
}

double TrustService::baitPenalty(const Json::Value& post) const {
  // text = post?.content?.text || post?.caption || ''
  std::string text;
  if (post.isObject()) {
    const Json::Value& content = post["content"];
    if (content.isObject() && content["text"].isString())
      text = content["text"].asString();
    if (text.empty()) {
      const Json::Value& caption = post["caption"];
      if (caption.isString()) text = caption.asString();
    }
  }
  if (text.empty()) return 1;

  int hits = 0;
  for (const auto& re : baitPatterns()) {
    if (std::regex_search(text, re)) hits++;
  }
  if (hits == 0) return 1;
  // each pattern shaves 25%, floored so genuine content using one phrase isn't killed
  return std::max(0.3, 1.0 - hits * 0.25);
}

double TrustService::computeAuthorTrust(const Json::Value& author) const {
  // if (!author) return 0.5
  // A bare id string is JS-truthy but carries no stats/createdAt/isVerified, so
  // it flows through to the neutral 0.5 computation below.
  if (author.isNull() || (!author.isObject() && !author.isString())) return 0.5;

  // if (author.isVerified) return 1.0
  if (author.isObject() && author["isVerified"].isBool() &&
      author["isVerified"].asBool())
    return 1.0;

  const Json::Value stats = author.isObject() ? author["stats"] : Json::Value();
  double followers = statNum(stats, "followers");
  double following = statNum(stats, "following");
  double posts = statNum(stats, "posts");

  double ageDays = 0;
  if (author.isObject() && author.isMember("createdAt")) {
    long long createdMs = 0;
    if (parseToMillis(author["createdAt"], createdMs)) {
      ageDays = (static_cast<double>(nowMs()) - static_cast<double>(createdMs)) / 86400000.0;
    }
  }

  double trust = 0.5;  // neutral prior

  // Account maturity (older = more trustworthy, saturating).
  trust += std::min(0.2, ageDays / 90.0 * 0.2);

  // Real audience: having followers that engage. Log-scaled.
  if (followers > 0)
    trust += std::min(0.2, std::log10(followers + 1) / 5.0 * 0.2);

  // Follow-ring / spam signal: following WAY more than followers with little
  // content is classic bot/farm behavior.
  if (following > 50 && followers > 0) {
    double ratio = following / (followers + 1);
    if (ratio > 10) trust -= 0.25;
    else if (ratio > 5) trust -= 0.12;
  }
  // Following thousands while posting nothing → almost certainly a bot.
  if (following > 500 && posts < 3) trust -= 0.25;

  // Brand-new account with an enormous follower count = bought followers.
  if (ageDays < 7 && followers > 5000) trust -= 0.2;

  return std::max(0.0, std::min(1.0, trust));
}

double TrustService::getAuthorTrust(const Json::Value& author) const {
  std::string id = resolveId(author);
  if (id.empty()) return 0.5;

  const std::string key = "trust:" + id;
  // try { const cached = await cacheService.get(key);
  //       if (cached !== null && typeof cached === 'number') return cached; }
  try {
    auto cached = cache().get(key);
    if (cached && !cached->empty()) {
      // The value was stored as a JSON number string; parse strictly. A
      // non-numeric payload mirrors `typeof cached !== 'number'` -> fall through.
      try {
        size_t consumed = 0;
        double n = std::stod(*cached, &consumed);
        if (consumed == cached->size()) return n;
      } catch (...) { /* fall through to compute */ }
    }
  } catch (...) { /* fall through */ }

  double t = computeAuthorTrust(author);
  // try { await cacheService.set(key, t, TRUST_TTL); } catch { /* best effort */ }
  try {
    Json::Value num(t);
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    std::string serialized = Json::writeString(w, num);
    cache().set(key, serialized, trustTtl_);
  } catch (...) { /* best effort */ }
  return t;
}

TrustService::Signals TrustService::buildSignals(const Json::Value& candidates) const {
  Signals out;
  out.trustMap = Json::objectValue;
  out.baitMap = Json::objectValue;

  if (!candidates.isArray()) return out;

  // Dedup authors so we compute/cache trust once per author per request. Insertion
  // order preserved to mirror the JS Map iteration order.
  std::vector<std::pair<std::string, Json::Value>> authorOrder;
  std::unordered_set<std::string> seen;

  for (const auto& c : candidates) {
    // const a = c.author || c.user || {}
    Json::Value a;
    if (c.isObject() && c.isMember("author") && !c["author"].isNull())
      a = c["author"];
    else if (c.isObject() && c.isMember("user") && !c["user"].isNull())
      a = c["user"];
    else
      a = Json::objectValue;

    // const aid = (a._id || a)?.toString()
    std::string aid = resolveId(a);
    if (!aid.empty() && seen.insert(aid).second) {
      authorOrder.emplace_back(aid, a);
    }

    // baitMap[(c._id || c).toString()] = baitPenalty(c)
    std::string pid = resolveId(c);
    if (!pid.empty()) out.baitMap[pid] = baitPenalty(c);
  }

  // await Promise.all([...authorById.entries()].map(...trustMap[aid] = getAuthorTrust(a)))
  for (const auto& [aid, a] : authorOrder) {
    out.trustMap[aid] = getAuthorTrust(a);
  }

  return out;
}

void TrustService::invalidate(const Json::Value& authorId) const {
  invalidate(resolveId(authorId));
}

void TrustService::invalidate(const std::string& authorId) const {
  if (authorId.empty()) return;
  // return cacheService.del(`trust:${authorId}`).catch(() => {})
  try { cache().del("trust:" + authorId); } catch (...) { /* swallow */ }
}

} // namespace pulse
