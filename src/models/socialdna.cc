// socialdna.cc — implementation of the SocialDNA model port. See socialdna.hpp.
//
// Ground truth: src/models/SocialDNA.js.
#include "pulse/models/socialdna.hpp"

#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/oid.hpp>
#include <bsoncxx/types.hpp>

#include <mongocxx/collection.hpp>
#include <mongocxx/options/index.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/find_one_and_update.hpp>
#include <mongocxx/exception/exception.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace pulse::models::socialdna {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;
using bld::make_array;
using pulse::bsonjson::oid;
using pulse::bsonjson::nowIso8601;

namespace {

// Fixed ordering of the five vibes, matching the JS `vibes` array exactly. The
// order matters for the tie-breaking reduces in _recalcStrands.
const std::array<std::string, 5> kVibeList = {"chill", "hype", "sad", "funny", "creative"};

// Per-vibe emoji + color tables (mirroring the JS literals).
std::string vibeEmoji(const std::string& v) {
  if (v == "chill")    return "\xF0\x9F\x98\x8C"; // 😌
  if (v == "hype")     return "\xF0\x9F\x94\xA5"; // 🔥
  if (v == "sad")      return "\xF0\x9F\x98\xA2"; // 😢
  if (v == "funny")    return "\xF0\x9F\x98\x82"; // 😂
  if (v == "creative") return "\xE2\x9C\xA8";     // ✨
  return "";
}

std::string vibeColor(const std::string& v) {
  if (v == "chill")    return "#00D2FF";
  if (v == "hype")     return "#FF6B35";
  if (v == "sad")      return "#7B68EE";
  if (v == "funny")    return "#FFD700";
  if (v == "creative") return "#FF1493";
  return "";
}

// Numeric accessor tolerant of int/double/missing — JSON numbers can arrive as
// either after a BSON round-trip.
double numOr(const Json::Value& obj, const std::string& key, double fallback) {
  if (!obj.isObject() || !obj.isMember(key)) return fallback;
  const Json::Value& v = obj[key];
  if (v.isNumeric()) return v.asDouble();
  return fallback;
}

long long now7DaysAgoMillis() {
  using namespace std::chrono;
  auto t = system_clock::now() - hours(24 * 7);
  return duration_cast<milliseconds>(t.time_since_epoch()).count();
}

// ISO-8601 UTC string for a given millisecond epoch (format produced by
// bson_json::nowIso8601: YYYY-MM-DDTHH:MM:SS.mmmZ).
std::string isoFromMillis(long long millis) {
  std::time_t secs = static_cast<std::time_t>(millis / 1000);
  int ms = static_cast<int>(millis % 1000);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &secs);
#else
  gmtime_r(&secs, &tm);
#endif
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
  return std::string(buf);
}

// Build the BSON insert document for a brand-new DNA profile from a Json doc
// that has already had applyDefaults() run on it. `user` is stored as a real
// BSON ObjectId; the rest are simple scalars / empty arrays.
bsoncxx::document::value buildInsertDoc(const Json::Value& doc,
                                        const bsoncxx::oid& userOid) {
  bld::document out;
  out.append(kvp("user", userOid));

  // strands subdocument.
  bld::document strands;
  for (const auto& v : kVibeList) {
    strands.append(kvp(v, static_cast<int>(numOr(doc["strands"], v, 20))));
  }
  out.append(kvp("strands", strands.extract()));

  out.append(kvp("dominantVibe", doc["dominantVibe"].asString()));

  // rawSignals subdocument.
  bld::document raw;
  for (const auto& v : kVibeList) {
    raw.append(kvp(v, static_cast<int>(numOr(doc["rawSignals"], v, 0))));
  }
  out.append(kvp("rawSignals", raw.extract()));

  out.append(kvp("totalSignals", static_cast<int>(numOr(doc, "totalSignals", 0))));
  out.append(kvp("snapshots", make_array()));
  out.append(kvp("twins", make_array()));
  out.append(kvp("latestInsights", make_array()));
  out.append(kvp("streak", static_cast<int>(numOr(doc, "streak", 0))));
  out.append(kvp("totalWeeksTracked", static_cast<int>(numOr(doc, "totalWeeksTracked", 0))));
  out.append(kvp("cardShareCount", static_cast<int>(numOr(doc, "cardShareCount", 0))));
  out.append(kvp("lastComputedAt", doc["lastComputedAt"].asString()));
  out.append(kvp("createdAt", doc["createdAt"].asString()));
  out.append(kvp("updatedAt", doc["updatedAt"].asString()));
  out.append(kvp("__v", 0));
  return out.extract();
}

} // namespace

// -----------------------------------------------------------------------------
// Indexes
// -----------------------------------------------------------------------------
void ensureIndexes() {
  try {
    auto col = pulse::db::collection(kCollection);

    // field `user`: { unique: true, index: true } -> unique index { user: 1 }.
    {
      mongocxx::options::index opts{};
      opts.unique(true);
      col.create_index(make_document(kvp("user", 1)), opts);
    }

    // socialDNASchema.index({ dominantVibe: 1 })
    col.create_index(make_document(kvp("dominantVibe", 1)));

    // socialDNASchema.index({ 'twins.user': 1 })
    col.create_index(make_document(kvp("twins.user", 1)));

    // socialDNASchema.index({ totalSignals: 1 })
    col.create_index(make_document(kvp("totalSignals", 1)));

    pulse::log::info("SocialDNA indexes ensured on collection '{}'", kCollection);
  } catch (const std::exception& e) {
    pulse::log::error("SocialDNA ensureIndexes failed: {}", e.what());
    throw;
  }
}

// -----------------------------------------------------------------------------
// Defaults / serialization
// -----------------------------------------------------------------------------
Json::Value applyDefaults(Json::Value doc) {
  if (!doc.isObject()) doc = Json::Value(Json::objectValue);

  // strands.{...}: default 20 each.
  if (!doc.isMember("strands") || !doc["strands"].isObject())
    doc["strands"] = Json::Value(Json::objectValue);
  for (const auto& v : kVibeList) {
    if (!doc["strands"].isMember(v)) doc["strands"][v] = 20;
  }

  // dominantVibe: enum, default 'chill'.
  if (!doc.isMember("dominantVibe")) doc["dominantVibe"] = "chill";

  // rawSignals.{...}: default 0 each.
  if (!doc.isMember("rawSignals") || !doc["rawSignals"].isObject())
    doc["rawSignals"] = Json::Value(Json::objectValue);
  for (const auto& v : kVibeList) {
    if (!doc["rawSignals"].isMember(v)) doc["rawSignals"][v] = 0;
  }

  // totalSignals: default 0.
  if (!doc.isMember("totalSignals")) doc["totalSignals"] = 0;

  // snapshots / twins / latestInsights: default empty arrays.
  if (!doc.isMember("snapshots"))      doc["snapshots"]      = Json::Value(Json::arrayValue);
  if (!doc.isMember("twins"))          doc["twins"]          = Json::Value(Json::arrayValue);
  if (!doc.isMember("latestInsights")) doc["latestInsights"] = Json::Value(Json::arrayValue);

  // stats: default 0.
  if (!doc.isMember("streak"))            doc["streak"]            = 0;
  if (!doc.isMember("totalWeeksTracked")) doc["totalWeeksTracked"] = 0;
  if (!doc.isMember("cardShareCount"))    doc["cardShareCount"]    = 0;

  // lastComputedAt: default Date.now.
  const std::string now = nowIso8601();
  if (!doc.isMember("lastComputedAt")) doc["lastComputedAt"] = now;

  // timestamps: true -> createdAt / updatedAt.
  if (!doc.isMember("createdAt")) doc["createdAt"] = now;
  if (!doc.isMember("updatedAt")) doc["updatedAt"] = now;

  // Mongoose version key.
  if (!doc.isMember("__v")) doc["__v"] = 0;

  return doc;
}

Json::Value sanitizeForOutput(Json::Value doc) {
  if (!doc.isObject()) return doc;
  // No select:false fields and no custom toJSON transform in socialDNASchema;
  // default Mongoose JSON keeps every field except the internal version key.
  doc.removeMember("__v");
  return doc;
}

// -----------------------------------------------------------------------------
// Instance methods (operating on a Json::Value document)
// -----------------------------------------------------------------------------
Json::Value& recalcStrands(Json::Value& doc) {
  // const total = this.totalSignals || 1; // avoid /0
  double totalSignals = numOr(doc, "totalSignals", 0);
  double total = (totalSignals != 0) ? totalSignals : 1.0;

  if (!doc.isMember("strands") || !doc["strands"].isObject())
    doc["strands"] = Json::Value(Json::objectValue);
  if (!doc.isMember("rawSignals") || !doc["rawSignals"].isObject())
    doc["rawSignals"] = Json::Value(Json::objectValue);

  // vibes.forEach(v => strands[v] = Math.round((rawSignals[v] / total) * 100));
  // JS Math.round rounds half up (toward +Infinity).
  for (const auto& v : kVibeList) {
    double raw = numOr(doc["rawSignals"], v, 0);
    int pct = static_cast<int>(std::floor((raw / total) * 100.0 + 0.5));
    doc["strands"][v] = pct;
  }

  // Fix rounding so it sums to 100.
  long long sum = 0;
  for (const auto& v : kVibeList) sum += doc["strands"][v].asInt();
  if (sum != 100 && total > 0) {
    long long diff = 100 - sum;
    // dominant = vibes.reduce((a, b) => strands[a] > strands[b] ? a : b)
    // Strict '>' keeps the earlier vibe on ties.
    std::string dominant = kVibeList[0];
    for (size_t i = 1; i < kVibeList.size(); ++i) {
      const std::string& b = kVibeList[i];
      if (!(doc["strands"][dominant].asInt() > doc["strands"][b].asInt())) {
        dominant = b;
      }
    }
    doc["strands"][dominant] = doc["strands"][dominant].asInt() + static_cast<int>(diff);
  }

  // dominantVibe = vibes.reduce((a, b) => strands[a] >= strands[b] ? a : b)
  // '>=' keeps the later vibe on ties.
  {
    std::string dominant = kVibeList[0];
    for (size_t i = 1; i < kVibeList.size(); ++i) {
      const std::string& b = kVibeList[i];
      if (!(doc["strands"][dominant].asInt() >= doc["strands"][b].asInt())) {
        dominant = b;
      }
    }
    doc["dominantVibe"] = dominant;
  }

  return doc;
}

Json::Value generateInsights(const Json::Value& doc) {
  Json::Value insights(Json::arrayValue);

  const Json::Value& snapshots =
      doc.isMember("snapshots") ? doc["snapshots"] : Json::Value(Json::arrayValue);
  bool hasPrev = snapshots.isArray() && !snapshots.empty();
  Json::Value prev = hasPrev ? snapshots[snapshots.size() - 1] : Json::Value(Json::nullValue);

  const Json::Value& strands =
      doc.isMember("strands") ? doc["strands"] : Json::Value(Json::objectValue);
  std::string dominantVibe = doc.isMember("dominantVibe") ? doc["dominantVibe"].asString() : "";

  if (!hasPrev) {
    Json::Value w(Json::objectValue);
    w["type"]   = "welcome";
    w["message"] = "\xF0\x9F\xA7\xAC Your Social DNA has been activated! Keep engaging to evolve it.";
    w["metric"] = "activation";
    w["value"]  = 1;
    insights.append(w);
    return insights;
  }

  const Json::Value& prevStrands =
      prev.isObject() && prev.isMember("strands") ? prev["strands"] : Json::Value(Json::objectValue);

  // Check for big shifts.
  for (const auto& v : kVibeList) {
    int cur  = static_cast<int>(numOr(strands, v, 0));
    int prevV = static_cast<int>(numOr(prevStrands, v, 0));
    int diff = cur - prevV;
    if (diff >= 10) {
      Json::Value ins(Json::objectValue);
      ins["type"]    = "spike";
      ins["message"] = vibeEmoji(v) + " Your " + v + " side surged +" +
                       std::to_string(diff) + "% this week!";
      ins["metric"]  = v;
      ins["value"]   = diff;
      insights.append(ins);
    } else if (diff <= -10) {
      Json::Value ins(Json::objectValue);
      ins["type"]    = "shift";
      ins["message"] = vibeEmoji(v) + " Your " + v + " energy dropped " +
                       std::to_string(std::abs(diff)) + "% \xE2\x80\x94 new phase?";
      ins["metric"]  = v;
      ins["value"]   = diff;
      insights.append(ins);
    }
  }

  // Check for dominant change.
  std::string prevDominant =
      prev.isObject() && prev.isMember("dominantVibe") ? prev["dominantVibe"].asString() : "";
  if (prevDominant != dominantVibe) {
    Json::Value ins(Json::objectValue);
    ins["type"]    = "shift";
    ins["message"] = "\xF0\x9F\x94\x84 Personality shift! You went from " +
                     vibeEmoji(prevDominant) + " " + prevDominant + " to " +
                     vibeEmoji(dominantVibe) + " " + dominantVibe;
    ins["metric"]  = "dominant_change";
    ins["value"]   = 1;
    insights.append(ins);
  }

  // Milestone checks.
  int totalWeeksTracked = static_cast<int>(numOr(doc, "totalWeeksTracked", 0));
  if (totalWeeksTracked == 4) {
    Json::Value ins(Json::objectValue);
    ins["type"]    = "milestone";
    ins["message"] = "\xF0\x9F\x8E\x89 1 month of Social DNA! Your fingerprint is getting more accurate.";
    ins["metric"]  = "weeks";
    ins["value"]   = 4;
    insights.append(ins);
  }

  if (insights.empty()) {
    int domPct = static_cast<int>(numOr(strands, dominantVibe, 0));
    Json::Value ins(Json::objectValue);
    ins["type"]    = "stable";
    ins["message"] = "\xF0\x9F\xA7\xAC Consistent vibes! Your DNA is " +
                     std::to_string(domPct) + "% " + dominantVibe + " " +
                     vibeEmoji(dominantVibe);
    ins["metric"]  = "stability";
    ins["value"]   = domPct;
    insights.append(ins);
  }

  return insights;
}

Json::Value takeSnapshot(Json::Value& doc) {
  // weekStart = now - 7d; weekEnd = now.
  const std::string weekEnd = nowIso8601();
  const std::string weekStart = isoFromMillis(now7DaysAgoMillis());

  // Build the snapshot. insights are generated BEFORE pushing (matching JS,
  // which calls _generateInsights() while snapshots still holds only the prior
  // entries, so prev = last existing snapshot).
  Json::Value snapshot(Json::objectValue);
  snapshot["weekStart"] = weekStart;
  snapshot["weekEnd"]   = weekEnd;
  snapshot["strands"] = doc.isMember("strands") ? doc["strands"] : Json::Value(Json::objectValue);
  snapshot["dominantVibe"] = doc.isMember("dominantVibe") ? doc["dominantVibe"] : Json::Value("");
  snapshot["insights"] = generateInsights(doc);
  snapshot["totalInteractions"] =
      static_cast<int>(numOr(doc, "totalSignals", 0));

  if (!doc.isMember("snapshots") || !doc["snapshots"].isArray())
    doc["snapshots"] = Json::Value(Json::arrayValue);
  doc["snapshots"].append(snapshot);

  // Keep only last 52 weeks.
  if (doc["snapshots"].size() > 52) {
    Json::Value trimmed(Json::arrayValue);
    Json::ArrayIndex sz = doc["snapshots"].size();
    Json::ArrayIndex start = sz - 52;
    for (Json::ArrayIndex i = start; i < sz; ++i) trimmed.append(doc["snapshots"][i]);
    doc["snapshots"] = trimmed;
  }

  // this.totalWeeksTracked++; this.streak++;
  doc["totalWeeksTracked"] = static_cast<int>(numOr(doc, "totalWeeksTracked", 0)) + 1;
  doc["streak"]            = static_cast<int>(numOr(doc, "streak", 0)) + 1;

  return snapshot;
}

int matchWith(const Json::Value& doc, const Json::Value& otherDNA) {
  const Json::Value& a = doc.isMember("strands") ? doc["strands"] : Json::Value(Json::objectValue);
  const Json::Value& b =
      otherDNA.isMember("strands") ? otherDNA["strands"] : Json::Value(Json::objectValue);

  double dotProduct = 0, magA = 0, magB = 0;
  for (const auto& v : kVibeList) {
    double av = numOr(a, v, 0);
    double bv = numOr(b, v, 0);
    dotProduct += av * bv;
    magA += av * av;
    magB += bv * bv;
  }

  magA = std::sqrt(magA);
  magB = std::sqrt(magB);

  if (magA == 0 || magB == 0) return 0;

  double similarity = dotProduct / (magA * magB);
  return static_cast<int>(std::floor(similarity * 100.0 + 0.5));
}

Json::Value getShareCardData(const Json::Value& doc) {
  const Json::Value& strands =
      doc.isMember("strands") ? doc["strands"] : Json::Value(Json::objectValue);

  // Object.entries(strands).sort((a, b) => b[1] - a[1]) — descending by value.
  // V8 iterates the strands subdocument in schema-declared order
  // (chill, hype, sad, funny, creative); we replicate that fixed order here so
  // ties resolve identically (JsonCpp's getMemberNames() would order keys
  // alphabetically and break parity on equal percentages). Then stable-sort
  // descending so ties keep their original (insertion) order.
  struct Entry { std::string vibe; int pct; };
  std::vector<Entry> entries;
  for (const auto& key : kVibeList) {
    entries.push_back({key, static_cast<int>(numOr(strands, key, 0))});
  }
  std::stable_sort(entries.begin(), entries.end(),
                   [](const Entry& x, const Entry& y) { return x.pct > y.pct; });

  Json::Value out(Json::objectValue);
  Json::Value strandArr(Json::arrayValue);
  for (const auto& e : entries) {
    Json::Value s(Json::objectValue);
    s["vibe"]       = e.vibe;
    s["percentage"] = e.pct;
    s["emoji"]      = vibeEmoji(e.vibe);
    s["color"]      = vibeColor(e.vibe);
    strandArr.append(s);
  }
  out["strands"] = strandArr;

  std::string dominantVibe = doc.isMember("dominantVibe") ? doc["dominantVibe"].asString() : "";
  out["dominantVibe"]  = dominantVibe;
  out["dominantEmoji"] = vibeEmoji(dominantVibe);
  out["dominantColor"] = vibeColor(dominantVibe);
  out["streak"]        = static_cast<int>(numOr(doc, "streak", 0));
  out["weeksTracked"]  = static_cast<int>(numOr(doc, "totalWeeksTracked", 0));
  return out;
}

// -----------------------------------------------------------------------------
// Statics
// -----------------------------------------------------------------------------
Json::Value getOrCreate(const std::string& userId) {
  try {
    auto col = pulse::db::collection(kCollection);
    const bsoncxx::oid userOid = oid(userId);

    // let dna = findOne({ user: userId });
    auto existing = col.find_one(make_document(kvp("user", userOid)));
    if (existing) {
      return pulse::bsonjson::toJson(existing->view());
    }

    // if (!dna) { dna = new this({ user: userId }); await dna.save(); }
    Json::Value doc(Json::objectValue);
    doc["user"] = userId;
    doc = applyDefaults(doc);

    col.insert_one(buildInsertDoc(doc, userOid).view());

    // Read back so the returned document matches what was persisted (including
    // the generated _id), mirroring the saved Mongoose document.
    auto created = col.find_one(make_document(kvp("user", userOid)));
    if (created) return pulse::bsonjson::toJson(created->view());
    return doc;
  } catch (const std::exception& e) {
    pulse::log::error("SocialDNA getOrCreate failed: {}", e.what());
    throw;
  }
}

std::optional<Json::Value> recordSignal(const std::string& userId,
                                        const std::string& vibe,
                                        int weight) {
  try {
    // const validVibes = [...]; if (!validVibes.includes(vibe)) return null;
    bool valid = false;
    for (const auto& v : kVibeList) {
      if (v == vibe) { valid = true; break; }
    }
    if (!valid) return std::nullopt;

    auto col = pulse::db::collection(kCollection);
    const bsoncxx::oid userOid = oid(userId);

    // const dna = await this.getOrCreate(userId);
    Json::Value dna = getOrCreate(userId);

    // dna.rawSignals[vibe] += weight; dna.totalSignals += weight;
    if (!dna.isMember("rawSignals") || !dna["rawSignals"].isObject())
      dna["rawSignals"] = Json::Value(Json::objectValue);
    int newRaw = static_cast<int>(numOr(dna["rawSignals"], vibe, 0)) + weight;
    dna["rawSignals"][vibe] = newRaw;
    int newTotal = static_cast<int>(numOr(dna, "totalSignals", 0)) + weight;
    dna["totalSignals"] = newTotal;

    // dna._recalcStrands();
    recalcStrands(dna);

    // dna.lastComputedAt = new Date();
    const std::string now = nowIso8601();
    dna["lastComputedAt"] = now;

    // await dna.save(); — persist the mutated fields.
    bld::document setDoc;
    setDoc.append(kvp("rawSignals." + vibe, newRaw));
    setDoc.append(kvp("totalSignals", newTotal));
    for (const auto& v : kVibeList) {
      setDoc.append(kvp("strands." + v, dna["strands"][v].asInt()));
    }
    setDoc.append(kvp("dominantVibe", dna["dominantVibe"].asString()));
    setDoc.append(kvp("lastComputedAt", now));
    setDoc.append(kvp("updatedAt", now));

    mongocxx::options::find_one_and_update opts{};
    opts.return_document(mongocxx::options::return_document::k_after);

    auto updated = col.find_one_and_update(
        make_document(kvp("user", userOid)),
        make_document(kvp("$set", setDoc.extract())),
        opts);

    if (updated) return pulse::bsonjson::toJson(updated->view());
    return dna;
  } catch (const std::exception& e) {
    pulse::log::error("SocialDNA recordSignal failed: {}", e.what());
    throw;
  }
}

// runWeeklyComputation — ports DNAMatchAlgo.runWeeklyComputation (keyset cursor).
// For every DNA doc with totalSignals >= 1: takeSnapshot(), set latestInsights
// to the new snapshot's insights, and persist (snapshots + latestInsights +
// streak + totalWeeksTracked). Batches of WEEKLY_BATCH_SIZE by _id ascending.
long long runWeeklyComputation() {
  constexpr int kWeeklyBatchSize = 500; // DNAMatchAlgo CONFIG.WEEKLY_BATCH_SIZE
  pulse::log::info("[SocialDNA] Starting weekly DNA computation (keyset)...");

  long long processed = 0;
  std::optional<bsoncxx::oid> lastId;

  try {
    auto col = pulse::db::collection(kCollection);
    for (;;) {
      // Filter: totalSignals >= 1, with _id > lastId for the keyset cursor.
      bld::document filter;
      filter.append(kvp("totalSignals", make_document(kvp("$gte", 1))));
      if (lastId) filter.append(kvp("_id", make_document(kvp("$gt", *lastId))));

      mongocxx::options::find findOpts{};
      findOpts.sort(make_document(kvp("_id", 1)));
      findOpts.limit(kWeeklyBatchSize);

      auto cursor = col.find(filter.extract(), findOpts);
      int batchCount = 0;
      bsoncxx::oid batchLast;

      for (const auto& view : cursor) {
        ++batchCount;
        batchLast = view["_id"].get_oid().value;
        try {
          Json::Value dna = pulse::bsonjson::toJson(view);
          // takeSnapshot mutates dna (appends snapshot, bumps streak/weeks).
          Json::Value snapshot = takeSnapshot(dna);
          dna["latestInsights"] = snapshot.isMember("insights")
              ? snapshot["insights"] : Json::Value(Json::arrayValue);

          // Persist the mutated snapshots / latestInsights / streak / weeks.
          // Build a $set whose value is a JSON object, converted to BSON once.
          Json::Value setJson(Json::objectValue);
          setJson["snapshots"]         = dna["snapshots"];
          setJson["latestInsights"]    = dna["latestInsights"];
          if (dna.isMember("streak"))            setJson["streak"] = dna["streak"];
          if (dna.isMember("totalWeeksTracked")) setJson["totalWeeksTracked"] = dna["totalWeeksTracked"];

          col.update_one(make_document(kvp("_id", batchLast)),
                         make_document(kvp("$set", pulse::bsonjson::fromJson(setJson))));
          ++processed;
        } catch (const std::exception& e) {
          // Promise.allSettled semantics — one failure doesn't abort the batch.
          pulse::log::warn("[SocialDNA] weekly snapshot failed for one user: {}", e.what());
        }
      }

      if (batchCount == 0) break;
      lastId = batchLast;
    }
  } catch (const std::exception& e) {
    pulse::log::error("[SocialDNA] runWeeklyComputation error: {}", e.what());
  }

  pulse::log::info("[SocialDNA] Weekly computation complete. Processed {} users.", processed);
  return processed;
}

} // namespace pulse::models::socialdna
