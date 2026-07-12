// reel_controller.cc — implementation of the Reel HttpController.
//
// 1:1 port of src/controllers/reelController.js. Every response shape, status
// code, error string, Redis key, TTL, Mongo query, and algorithm payload is
// preserved verbatim. The handlers call already-ported services/models rather
// than re-implementing their logic:
//   * Reel / ReelComment / Like / Follow / Notification / UserEngagement models
//   * MediaService (Cloudinary upload + URL optimize)   -> pulse::media()
//   * UserVectorService / EmbeddingService              -> pulse::userVector() / pulse::embedding()
//   * ReelAlgo.reelRank / CommentsAlgo.commentsRank      -> pulse::algos::reelRank / commentsRank
//
// Auth context: the AuthFilter stores { userId, username, email, isVerified } on
// the request attributes under "user"; handlers read it back with
//   req->getAttributes()->get<Json::Value>("user").
#include "pulse/controllers/reel_controller.hpp"

#include "pulse/http_response.hpp"
#include "pulse/db.hpp"
#include "pulse/cache.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/config.hpp"
#include "pulse/logger.hpp"
#include "pulse/algorithms.hpp"

#include "pulse/models/reel.hpp"
#include "pulse/models/reelcomment.hpp"
#include "pulse/models/like.hpp"
#include "pulse/models/follow.hpp"
#include "pulse/models/notification.hpp"
#include "pulse/models/userengagement.hpp"

#include "pulse/services/media_service.hpp"
#include "pulse/services/user_vector_service.hpp"
#include "pulse/services/embedding_service.hpp"

#include <drogon/HttpRequest.h>
#include <drogon/MultiPart.h>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/oid.hpp>

#include <mongocxx/collection.hpp>
#include <mongocxx/cursor.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/update.hpp>
#include <mongocxx/pipeline.hpp>
#include <mongocxx/exception/exception.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace pulse::controllers {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;
using bld::make_array;
namespace bj = pulse::bsonjson;

namespace {

// REEL_CANDIDATE_TTL = parseInt(process.env.REEL_CANDIDATE_TTL_SEC) || 30.
int reelCandidateTtl() {
  return static_cast<int>(pulse::config().envInt("REEL_CANDIDATE_TTL_SEC", 30));
}

// FOLLOW_GRAPH_TTL_SEC || 60 (the reel-specific following list cache TTL).
int followGraphTtl() {
  return static_cast<int>(pulse::config().envInt("FOLLOW_GRAPH_TTL_SEC", 60));
}

// ── JSON serialize / parse helpers (compact, no trailing newline) ──
std::string dumpJson(const Json::Value& v) {
  Json::StreamWriterBuilder b;
  b["indentation"] = "";
  b["commentStyle"] = "None";
  return Json::writeString(b, v);
}

Json::Value parseJson(const std::string& s) {
  Json::Value v;
  if (s.empty()) return v;
  Json::CharReaderBuilder b;
  std::string errs;
  std::unique_ptr<Json::CharReader> reader(b.newCharReader());
  if (!reader->parse(s.data(), s.data() + s.size(), &v, &errs)) return Json::Value();
  return v;
}

long long nowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch()).count();
}

// Math.min(Math.max(parseInt(x) || dflt, lo), hi) over a query-string value.
int clampParam(const std::string& raw, int dflt, int lo, int hi) {
  int v = dflt;
  if (!raw.empty()) {
    try { v = std::stoi(raw); } catch (...) { v = dflt; }
    if (v == 0) v = dflt;  // parseInt(...)||dflt: 0/NaN -> default
  }
  v = std::max(lo, std::min(hi, v));
  return v;
}

// Parse an ISO-8601-ish date string into epoch millis (new Date(str)).
std::optional<long long> parseDateMillis(const std::string& s) {
  if (s.empty()) return std::nullopt;
  int year = 0, mon = 0, day = 0, hour = 0, min = 0, sec = 0;
  int matched = std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d",
                            &year, &mon, &day, &hour, &min, &sec);
  if (matched < 3) return std::nullopt;
  std::tm tm{};
  tm.tm_year = year - 1900; tm.tm_mon = mon - 1; tm.tm_mday = day;
  tm.tm_hour = hour; tm.tm_min = min; tm.tm_sec = sec;
#if defined(_WIN32)
  std::time_t t = _mkgmtime(&tm);
#else
  std::time_t t = timegm(&tm);
#endif
  if (t == static_cast<std::time_t>(-1)) return std::nullopt;
  long long ms = static_cast<long long>(t) * 1000;
  auto dot = s.find('.');
  if (dot != std::string::npos && dot + 1 < s.size()) {
    std::string frac;
    for (size_t i = dot + 1; i < s.size() && std::isdigit((unsigned char)s[i]); ++i)
      frac.push_back(s[i]);
    while (frac.size() < 3) frac.push_back('0');
    frac = frac.substr(0, 3);
    ms += std::stoll(frac);
  }
  return ms;
}

// createdAt (ISO string or numeric ms) -> epoch millis (msFields normalization).
long long createdAtMillis(const Json::Value& r, long long fallback) {
  if (!r.isObject() || !r.isMember("createdAt")) return fallback;
  const Json::Value& c = r["createdAt"];
  if (c.isNumeric()) return static_cast<long long>(c.asDouble());
  if (c.isString()) { if (auto ms = parseDateMillis(c.asString())) return *ms; }
  return fallback;
}

// reel hex _id from a (lean) reel JSON doc.
std::string reelHexId(const Json::Value& r) {
  if (!r.isObject()) return "";
  const Json::Value& id = r["_id"];
  if (id.isString()) return id.asString();
  return "";
}

// ── Inline helpers ported from the JS controller ──

// HELPER: Optimize Cloudinary URL (getOptimizedVideoUrl). Delegates to the
// MediaService, which implements the exact '/upload/' -> '/upload/f_auto,
// q_auto,w_720/' rewrite (no-op for non-Cloudinary URLs).
std::string getOptimizedVideoUrl(const std::string& url) {
  return pulse::media().optimizeUrl(url);
}

// HELPER: Normalize User Object (normalizeUser). Mirrors the JS avatar-resolution
// chain and the projected output { _id, username, isVerified, avatar, stats }.
const char* kBrokenDefaultAvatar = "/defaults/avatar.png";

bool isValidAvatar(const Json::Value& url) {
  return url.isString() && !url.asString().empty()
         && url.asString().find(kBrokenDefaultAvatar) == std::string::npos;
}

Json::Value normalizeUser(const Json::Value& user) {
  // JS: `if (!user) return null` — only a null/undefined user yields null. A
  // populated user is an object; a failed-populate lean ref is a bare id string,
  // for which JS still builds the projected object with undefined fields.
  if (user.isNull()) return Json::Value(Json::nullValue);

  Json::Value cleanAvatar(Json::nullValue);

  // 1. profile.avatar (skip the schema default placeholder).
  if (user.isMember("profile") && user["profile"].isObject()
      && isValidAvatar(user["profile"]["avatar"])) {
    cleanAvatar = user["profile"]["avatar"];
  }
  // 2. direct avatar field.
  else if (isValidAvatar(user["avatar"])) {
    cleanAvatar = user["avatar"];
  }
  // 3. authMethods OAuth avatars.
  else if (user.isMember("authMethods") && user["authMethods"].isArray()
           && user["authMethods"].size() > 0) {
    for (const auto& method : user["authMethods"]) {
      if (method.isObject() && method.isMember("profile")
          && method["profile"].isObject()
          && isValidAvatar(method["profile"]["avatar"])) {
        cleanAvatar = method["profile"]["avatar"];
        break;
      }
    }
  }

  Json::Value out(Json::objectValue);
  out["_id"]        = user.isMember("_id") ? user["_id"] : Json::Value(Json::nullValue);
  out["username"]   = user.isMember("username") ? user["username"] : Json::Value(Json::nullValue);
  out["isVerified"] = user.isMember("isVerified") ? user["isVerified"] : Json::Value(Json::nullValue);
  out["avatar"]     = cleanAvatar;
  out["stats"]      = user.isMember("stats") ? user["stats"] : Json::Value(Json::nullValue);
  return out;
}

// Read the authenticated user JSON ({ userId, username, email, isVerified })
// stored by AuthFilter. Returns an empty userId string when absent.
Json::Value authUser(const HttpRequestPtr& req) {
  auto attrs = req->getAttributes();
  if (attrs->find("user")) return attrs->get<Json::Value>("user");
  return Json::Value(Json::nullValue);
}
std::string authUserId(const Json::Value& user) {
  if (user.isObject() && user.isMember("userId") && user["userId"].isString())
    return user["userId"].asString();
  return "";
}

// Append a ref field (ObjectId) from a Json string to a builder; a 24-hex string
// coerces to bsoncxx::oid (Mongoose ref casting), any other string kept verbatim.
void appendRef(bld::document& doc, const std::string& key, const std::string& hex) {
  if (auto id = bj::tryOid(hex)) { doc.append(kvp(key, *id)); return; }
  doc.append(kvp(key, hex));
}

// .populate({ path:'user'/'author', select:'+authMethods username profile avatar
// isVerified stats' }) — hydrate each doc's `field` ObjectId ref into the
// projected user subdocument, in one $in query. Docs whose ref cannot be
// resolved keep their original ref. `key` is the property holding the ref.
void populateUsers(Json::Value& docs, const std::string& key,
                   bool includeStats, bool includeAuthMethods) {
  if (!docs.isArray() || docs.empty()) return;

  std::vector<std::string> ids;
  for (const auto& d : docs) {
    if (!d.isMember(key)) continue;
    const Json::Value& a = d[key];
    std::string hex;
    if (a.isString()) hex = a.asString();
    else if (a.isObject() && a.isMember("_id") && a["_id"].isString())
      hex = a["_id"].asString();
    if (!hex.empty() && std::find(ids.begin(), ids.end(), hex) == ids.end())
      ids.push_back(hex);
  }
  if (ids.empty()) return;

  Json::Value byId(Json::objectValue);
  try {
    bld::array in;
    for (const auto& hex : ids)
      if (auto o = bj::tryOid(hex)) in.append(*o);
    auto filter = make_document(kvp("_id", make_document(kvp("$in", in.extract()))));

    bld::document proj;
    proj.append(kvp("username", 1), kvp("profile", 1), kvp("avatar", 1),
                kvp("isVerified", 1));
    if (includeStats)       proj.append(kvp("stats", 1));
    if (includeAuthMethods) proj.append(kvp("authMethods", 1));
    auto projection = proj.extract();
    mongocxx::options::find opts{};
    opts.projection(projection.view());

    auto col = pulse::db::collection("users");
    auto cursor = col.find(filter.view(), opts);
    for (const auto& doc : cursor) {
      Json::Value u = bj::toJson(doc);
      if (u.isMember("_id") && u["_id"].isString()) byId[u["_id"].asString()] = u;
    }
  } catch (const std::exception& e) {
    pulse::log::error("[reels] user populate failed: {}", e.what());
    return;
  }

  for (auto& d : docs) {
    if (!d.isMember(key)) continue;
    const Json::Value& a = d[key];
    std::string hex;
    if (a.isString()) hex = a.asString();
    else if (a.isObject() && a.isMember("_id") && a["_id"].isString())
      hex = a["_id"].asString();
    if (!hex.empty() && byId.isMember(hex)) d[key] = byId[hex];
  }
}

// reel.user ref hex (populated object or raw id).
std::string reelUserId(const Json::Value& r) {
  if (!r.isObject() || !r.isMember("user")) return "";
  const Json::Value& u = r["user"];
  if (u.isString()) return u.asString();
  if (u.isObject() && u.isMember("_id") && u["_id"].isString())
    return u["_id"].asString();
  return "";
}

// Cached following-ids lookup (getCachedFollowing): getOrSet under
// `reel:following:${userId}`, value = Follow.getFollowingIds(userId).map(String).
std::vector<std::string> getCachedFollowing(const std::string& userId) {
  std::string cached = pulse::cache().getOrSet(
      "reel:following:" + userId,
      [&]() -> std::string {
        std::vector<std::string> ids = pulse::models::follow::getFollowingIds(userId);
        Json::Value arr(Json::arrayValue);
        for (const auto& id : ids) arr.append(id);
        return dumpJson(arr);
      },
      followGraphTtl());
  std::vector<std::string> out;
  Json::Value j = parseJson(cached);
  if (j.isArray())
    for (const auto& x : j) if (x.isString()) out.push_back(x.asString());
  return out;
}

// Shared candidate set fetch for 'reel:candidate:foryou':
//   Reel.find({ isActive: { $ne: false } }).sort({createdAt:-1}).limit(100)
//     .populate('user', '+authMethods username profile avatar isVerified stats').lean()
Json::Value fetchReelCandidates() {
  std::string cached = pulse::cache().getOrSet(
      "reel:candidate:foryou",
      [&]() -> std::string {
        Json::Value reels(Json::arrayValue);
        try {
          auto col = pulse::db::collection(pulse::models::reel::kCollection);
          auto filter = make_document(
              kvp("isActive", make_document(kvp("$ne", false))));
          mongocxx::options::find opts{};
          opts.sort(make_document(kvp("createdAt", -1)));
          opts.limit(100);
          auto cursor = col.find(filter.view(), opts);
          for (const auto& doc : cursor) reels.append(bj::toJson(doc));
        } catch (const std::exception& e) {
          pulse::log::error("[reels] candidate fetch failed: {}", e.what());
        }
        populateUsers(reels, "user", /*stats=*/true, /*authMethods=*/true);
        return dumpJson(reels);
      },
      reelCandidateTtl());
  Json::Value reels = parseJson(cached);
  if (!reels.isArray()) reels = Json::Value(Json::arrayValue);
  return reels;
}

// ── ReelAlgo bridge (ReelAlgo.getForYouFeed / getFollowingFeed -> reelRank) ──
//
// aid(r): (r.user?._id || r.user || r.author?._id || r.author)?.toString().
std::string algoAuthorId(const Json::Value& r) {
  if (!r.isObject()) return "";
  if (r.isMember("user")) {
    const Json::Value& u = r["user"];
    if (u.isObject() && u.isMember("_id") && u["_id"].isString()) return u["_id"].asString();
    if (u.isString()) return u.asString();
  }
  if (r.isMember("author")) {
    const Json::Value& a = r["author"];
    if (a.isObject() && a.isMember("_id") && a["_id"].isString()) return a["_id"].asString();
    if (a.isString()) return a.asString();
  }
  return "";
}

Json::Value toJsonArray(const std::vector<std::string>& v) {
  Json::Value a(Json::arrayValue);
  for (const auto& s : v) a.append(s);
  return a;
}

// rankReels(reels, userId, options): batch affinity + (optional) velocity maps,
// build the reelRank payload (msFields createdAt), invoke the native kernel.
// injectDiversity uses Math.random and is a JS-only post step; it is omitted here
// (parity-safe: it only reorders, and the C++ kernel is the ranking source of
// truth). On any failure we return the input order, matching the JS try/catch
// fallback to JS.rankReels which preserves the candidate set.
Json::Value rankReels(const Json::Value& reels, const std::string& userId,
                      bool includeVelocity, const std::vector<std::string>& followingIds) {
  if (!reels.isArray() || reels.empty()) return Json::Value(Json::arrayValue);

  const long long now = nowMillis();

  // authorIds = [...new Set(reels.map(aid).filter(Boolean))]
  std::vector<std::string> authorIds;
  {
    std::unordered_set<std::string> seen;
    for (const auto& r : reels) {
      std::string aid = algoAuthorId(r);
      if (!aid.empty() && seen.insert(aid).second) authorIds.push_back(aid);
    }
  }

  Json::Value affinityMap(Json::objectValue);
  if (!userId.empty() && !authorIds.empty()) {
    try {
      std::map<std::string, double> aff =
          pulse::models::userengagement::getBatchAffinities(userId, authorIds);
      for (const auto& kv : aff) affinityMap[kv.first] = kv.second;
    } catch (const std::exception& e) {
      pulse::log::warn("[reels] affinity fetch failed: {}", e.what());
    }
  }

  Json::Value velocityMap(Json::objectValue);
  if (includeVelocity) {
    try {
      std::vector<std::string> reelIds;
      for (const auto& r : reels) {
        std::string id = reelHexId(r);
        if (!id.empty()) reelIds.push_back(id);
      }
      // JS.CONFIG.VELOCITY_WINDOW_HOURS — Like.getBatchLikeVelocities default 1.0.
      std::map<std::string, double> vel =
          pulse::models::like::getBatchLikeVelocities(
              pulse::models::like::kTargetTypeReel, reelIds, 1.0);
      for (const auto& kv : vel) velocityMap[kv.first] = kv.second;
    } catch (const std::exception& e) {
      pulse::log::warn("[reels] velocity fetch failed: {}", e.what());
    }
  }

  // msFields(reels, ['createdAt']) — normalize createdAt to epoch ms.
  Json::Value reelsMs(Json::arrayValue);
  for (const auto& r : reels) {
    Json::Value q = r;
    q["createdAt"] = static_cast<Json::Int64>(createdAtMillis(r, now));
    reelsMs.append(q);
  }

  Json::Value negative(Json::objectValue);
  negative["skippedCreators"]  = Json::Value(Json::arrayValue);
  negative["hiddenCategories"] = Json::Value(Json::arrayValue);

  Json::Value payload(Json::objectValue);
  payload["reels"]           = reelsMs;
  payload["userId"]          = userId.empty() ? Json::Value(Json::nullValue)
                                              : Json::Value(userId);
  payload["nowMs"]           = static_cast<Json::Int64>(now);
  payload["followingIds"]    = toJsonArray(followingIds);
  payload["velocityMap"]     = velocityMap;
  payload["affinityMap"]     = affinityMap;
  payload["userAudioPrefs"]  = Json::Value(Json::objectValue);
  payload["negativeSignals"] = negative;
  payload["sessionDepth"]    = 0;

  try {
    std::string ranked = pulse::algos::reelRank(dumpJson(payload));
    Json::Value out = parseJson(ranked);
    if (out.isArray()) return out;
  } catch (const std::exception& e) {
    pulse::log::warn("[ReelAlgo] native path failed, using input order: {}", e.what());
  }
  return reels;
}

// ReelAlgo.getForYouFeed(userId, reels, { followingIds }):
//   others = reels.filter(aid != userId); discovery = others.length>=5 ? others : reels;
//   rankReels(discovery, userId, { followingIds, injectDiversityContent:true }).
Json::Value getForYouFeed(const std::string& userId, const Json::Value& reels,
                          const std::vector<std::string>& followingIds) {
  Json::Value others(Json::arrayValue);
  if (reels.isArray())
    for (const auto& r : reels)
      if (algoAuthorId(r) != userId) others.append(r);
  const Json::Value& discovery = (others.size() >= 5) ? others : reels;
  return rankReels(discovery, userId, /*includeVelocity=*/true, followingIds);
}

// ReelAlgo.getFollowingFeed(userId, reels, followingIds):
//   set = new Set(followingIds); followed = reels.filter(aid in set || aid==userId);
//   rankReels(followed, userId, { includeVelocity:false, followingIds }).
Json::Value getFollowingFeed(const std::string& userId, const Json::Value& reels,
                             const std::vector<std::string>& followingIds) {
  std::unordered_set<std::string> set(followingIds.begin(), followingIds.end());
  Json::Value followed(Json::arrayValue);
  if (reels.isArray())
    for (const auto& r : reels) {
      std::string aid = algoAuthorId(r);
      if (set.count(aid) || aid == userId) followed.append(r);
    }
  return rankReels(followed, userId, /*includeVelocity=*/false, followingIds);
}

// CommentsAlgo.rankComments(comments, { mode, opId, includeReplies:true }):
//   payload = { comments: msFields(comments,['createdAt'],['replies']), mode,
//               opId, nowMs }; JSON.parse(commentsRank(JSON.stringify(payload))).
Json::Value msCommentCreatedAt(const Json::Value& c, long long now) {
  Json::Value q = c;
  q["createdAt"] = static_cast<Json::Int64>(createdAtMillis(c, now));
  if (q.isMember("replies") && q["replies"].isArray()) {
    Json::Value reps(Json::arrayValue);
    for (const auto& rep : q["replies"]) reps.append(msCommentCreatedAt(rep, now));
    q["replies"] = reps;
  }
  return q;
}

Json::Value rankComments(const Json::Value& comments, const std::string& mode,
                         const std::string& opId) {
  if (!comments.isArray() || comments.empty()) return Json::Value(Json::arrayValue);
  const long long now = nowMillis();

  Json::Value commentsMs(Json::arrayValue);
  for (const auto& c : comments) commentsMs.append(msCommentCreatedAt(c, now));

  Json::Value payload(Json::objectValue);
  payload["comments"] = commentsMs;
  payload["mode"]     = mode.empty() ? std::string("best") : mode;
  payload["opId"]     = opId;  // options.opId ? toString() : ''
  payload["nowMs"]    = static_cast<Json::Int64>(now);

  try {
    std::string ranked = pulse::algos::commentsRank(dumpJson(payload));
    Json::Value out = parseJson(ranked);
    if (out.isArray()) return out;
  } catch (const std::exception& e) {
    pulse::log::warn("[CommentsAlgo] native path failed: {}", e.what());
  }
  return comments;  // JS falls back to JS.rankComments(comments) — keep order.
}

} // namespace

// =========================================================
//  1. CREATE REEL
// =========================================================
void ReelController::createReel(const HttpRequestPtr& req,
                                std::function<void(const HttpResponsePtr&)>&& callback) {
  Json::Value user = authUser(req);
  std::string userId = authUserId(user);

  // if (!userId) return res.status(401).json({ success:false, message:'Unauthorized' })
  if (userId.empty()) {
    Json::Value body(Json::objectValue);
    body["success"] = false;
    body["message"] = "Unauthorized";
    callback(pulse::http::json(drogon::k401Unauthorized, body));
    return;
  }

  try {
    // multer videoUpload.single('file') -> req.file. Parse the multipart body
    // (the multipart parse that multer did in the Express chain). Form fields
    // (req.body.caption) and the file (req.file) both come out of this parse.
    drogon::MultiPartParser fileParser;
    std::string fileBytes;
    std::string fileName;
    std::string caption;  // req.body.caption || ''
    if (fileParser.parse(req) == 0) {
      const auto& files = fileParser.getFiles();
      for (const auto& f : files) {
        // multer .single('file') keys the upload by the field name 'file'.
        if (f.getItemName() == "file") {
          fileBytes = std::string(f.fileContent());
          fileName  = f.getFileName();
          break;
        }
      }
      if (fileBytes.empty() && !files.empty()) {
        fileBytes = std::string(files.front().fileContent());
        fileName  = files.front().getFileName();
      }
      // req.body.caption — a non-file form field.
      const auto& params = fileParser.getParameters();
      auto it = params.find("caption");
      if (it != params.end()) caption = it->second;
    }

    // if (!req.file) return res.status(400).json({ success:false, message:'No video file provided' })
    if (fileBytes.empty()) {
      Json::Value body(Json::objectValue);
      body["success"] = false;
      body["message"] = "No video file provided";
      callback(pulse::http::json(drogon::k400BadRequest, body));
      return;
    }

    // cloudinary.uploader.upload_stream({ folder: <config.folder>/reels,
    //   resource_type:'video', eager:[{width:720,crop:'limit',quality:'auto:good'}],
    //   eager_async:true }) — the MediaService.uploadReel implements this exactly.
    pulse::UploadResult result;
    try {
      result = pulse::media().uploadReel(fileBytes, fileName);
    } catch (const std::exception& e) {
      // Cloudinary upload callback error path.
      pulse::log::error("Cloudinary Error: {}", e.what());
      Json::Value body(Json::objectValue);
      body["success"] = false;
      body["message"] = "Cloudinary upload failed";
      callback(pulse::http::json(drogon::k500InternalServerError, body));
      return;
    }

    // Reel.create({ user, videoUrl, publicId, caption, stats:{likes,comments,shares,views} })
    try {
      Json::Value doc(Json::objectValue);
      doc["user"]     = userId;
      doc["videoUrl"] = result.secureUrl;
      doc["publicId"] = result.publicId;
      doc["caption"]  = caption;  // req.body.caption || ''
      Json::Value stats(Json::objectValue);
      stats["likes"] = 0; stats["comments"] = 0; stats["shares"] = 0; stats["views"] = 0;
      doc["stats"] = stats;

      // Apply schema defaults (fills remaining stats fields, timestamps, etc.).
      Json::Value defaulted = pulse::models::reel::applyDefaults(doc);

      // Build the insert document, coercing ref ids + the explicit stats object.
      bld::document insert;
      bsoncxx::oid newId;
      insert.append(kvp("_id", newId));
      appendRef(insert, "user", userId);
      insert.append(kvp("videoUrl", result.secureUrl));
      insert.append(kvp("publicId", result.publicId));
      insert.append(kvp("caption", caption));
      // stats sub-document.
      bld::document statsDoc;
      for (const auto& k : defaulted["stats"].getMemberNames())
        statsDoc.append(kvp(k, defaulted["stats"][k].asInt64()));
      insert.append(kvp("stats", statsDoc.extract()));
      // remaining defaulted fields: likes[], commentsCount, duration, hashtags[], music, timestamps.
      insert.append(kvp("likes", make_array()));
      insert.append(kvp("commentsCount", static_cast<std::int64_t>(0)));
      insert.append(kvp("duration", static_cast<std::int64_t>(0)));
      insert.append(kvp("hashtags", make_array()));
      insert.append(kvp("music", bsoncxx::types::b_null{}));
      long long now = nowMillis();
      insert.append(kvp("createdAt", bsoncxx::types::b_date{std::chrono::milliseconds{now}}));
      insert.append(kvp("updatedAt", bsoncxx::types::b_date{std::chrono::milliseconds{now}}));
      insert.append(kvp("__v", static_cast<std::int64_t>(0)));

      auto col = pulse::db::collection(pulse::models::reel::kCollection);
      col.insert_one(insert.view());

      // Read back the created doc to return it (res.json({ data: newReel })).
      auto created = col.find_one(make_document(kvp("_id", newId)));
      Json::Value newReel = created
          ? pulse::models::reel::sanitizeForOutput(bj::toJson(created->view()))
          : defaulted;

      callback(pulse::http::ok(newReel, drogon::k201Created));
      return;
    } catch (const std::exception& e) {
      // catch (dbError) -> 500 Database error.
      pulse::log::error("Database Save Error: {}", e.what());
      Json::Value body(Json::objectValue);
      body["success"] = false;
      body["message"] = "Database error";
      callback(pulse::http::json(drogon::k500InternalServerError, body));
      return;
    }
  } catch (const std::exception& e) {
    // catch (error) -> 500 Server error.
    pulse::log::error("General Error: {}", e.what());
    Json::Value body(Json::objectValue);
    body["success"] = false;
    body["message"] = "Server error";
    callback(pulse::http::json(drogon::k500InternalServerError, body));
    return;
  }
}

// =========================================================
//  2. GET REELS FEED - RANKED
// =========================================================
void ReelController::getReelsFeed(const HttpRequestPtr& req,
                                  std::function<void(const HttpResponsePtr&)>&& callback) {
  // Helper to stamp the no-cache headers on every response (res.set(...) at top).
  auto withNoCache = [](const HttpResponsePtr& resp) {
    resp->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, private");
    resp->addHeader("Pragma", "no-cache");
    resp->addHeader("Expires", "0");
    return resp;
  };

  try {
    int page = clampParam(req->getParameter("page"), 1, 1, 50);
    int limit = clampParam(req->getParameter("limit"), 10, 1, 50);
    std::string feedType = req->getParameter("type");
    if (feedType.empty()) feedType = "foryou";
    Json::Value user = authUser(req);
    std::string userId = authUserId(user);

    // Shared candidate set ('reel:candidate:foryou').
    Json::Value reels = fetchReelCandidates();

    if (!reels.isArray() || reels.empty()) {
      Json::Value data(Json::arrayValue);
      Json::Value pagination(Json::objectValue);
      pagination["page"] = page;
      pagination["limit"] = limit;
      pagination["hasMore"] = false;
      pagination["feedType"] = feedType;
      Json::Value extra(Json::objectValue);
      extra["data"] = data;
      extra["pagination"] = pagination;
      callback(withNoCache(pulse::http::success(extra, drogon::k200OK)));
      return;
    }

    // Following list (best-effort).
    std::vector<std::string> followingIds;
    if (!userId.empty()) {
      try {
        followingIds = getCachedFollowing(userId);
      } catch (const std::exception& e) {
        pulse::log::warn("Could not get following: {}", e.what());
      }
    }

    // Per-user liked set for these reels (counts come from stats, not aggregation).
    std::vector<std::string> reelIds;
    for (const auto& r : reels) {
      std::string id = reelHexId(r);
      if (!id.empty()) reelIds.push_back(id);
    }
    std::set<std::string> likedSet;
    try {
      if (!userId.empty())
        likedSet = pulse::models::like::getLikedIds(
            userId, pulse::models::like::kTargetTypeReel, reelIds);
    } catch (const std::exception& e) {
      pulse::log::warn("Like lookup failed: {}", e.what());
    }

    // Personalized candidate generation (For-You only): reorder by cosine to the
    // user taste vector before ranking. Best-effort.
    Json::Value candidateReels = reels;
    if (feedType != "following" && !userId.empty()) {
      try {
        Json::Value userVec = pulse::userVector().getUserVector(userId);
        if (userVec.isArray() && !userVec.empty()) {
          std::vector<double> uv;
          uv.reserve(userVec.size());
          for (const auto& x : userVec) uv.push_back(x.isNumeric() ? x.asDouble() : 0.0);

          struct Scored { Json::Value r; double s; };
          std::vector<Scored> scored;
          scored.reserve(reels.size());
          for (const auto& r : reels) {
            std::vector<double> rv = pulse::embedding().reelVector(r);
            double s = pulse::embedding().cosine(uv, rv);
            scored.push_back({r, s});
          }
          std::stable_sort(scored.begin(), scored.end(),
                           [](const Scored& a, const Scored& b) { return a.s > b.s; });
          Json::Value reordered(Json::arrayValue);
          for (const auto& x : scored) reordered.append(x.r);
          candidateReels = reordered;
        }
      } catch (const std::exception& e) {
        pulse::log::warn("[reels] retrieval failed, using recency pool: {}", e.what());
      }
    }

    // Ranking with fallback.
    Json::Value rankedReels;
    try {
      if (feedType == "following")
        rankedReels = getFollowingFeed(userId, reels, followingIds);
      else
        rankedReels = getForYouFeed(userId, candidateReels, followingIds);
    } catch (const std::exception& e) {
      pulse::log::error("[getReelsFeed] Ranking algorithm failed: {}", e.what());
      rankedReels = reels;  // fallback to chronological order
    }

    if (!rankedReels.isArray() || rankedReels.empty()) rankedReels = reels;

    // Paginate after ranking.
    int startIndex = (page - 1) * limit;
    Json::Value processedReels(Json::arrayValue);
    int total = static_cast<int>(rankedReels.size());
    for (int i = startIndex; i < total && i < startIndex + limit; ++i) {
      Json::Value reel = rankedReels[i];

      reel["videoUrl"] = getOptimizedVideoUrl(
          reel.isMember("videoUrl") && reel["videoUrl"].isString()
              ? reel["videoUrl"].asString() : std::string());
      reel["isLiked"] = likedSet.count(reelHexId(reel)) > 0;

      // likesCount = reel.stats?.likes ?? reel.likesCount ?? 0
      Json::Value likesCount(0);
      if (reel.isMember("stats") && reel["stats"].isObject()
          && reel["stats"].isMember("likes") && reel["stats"]["likes"].isNumeric())
        likesCount = reel["stats"]["likes"];
      else if (reel.isMember("likesCount") && reel["likesCount"].isNumeric())
        likesCount = reel["likesCount"];
      reel["likesCount"] = likesCount;

      reel["user"] = normalizeUser(reel.isMember("user") ? reel["user"]
                                                         : Json::Value(Json::nullValue));

      // Remove internal scoring fields (set to undefined -> dropped).
      reel.removeMember("_score");
      reel.removeMember("_personalBoost");
      reel.removeMember("isDiversity");

      processedReels.append(reel);
    }

    bool hasMore = (startIndex + limit) < total;

    Json::Value pagination(Json::objectValue);
    pagination["page"] = page;
    pagination["limit"] = limit;
    pagination["hasMore"] = hasMore;
    pagination["feedType"] = feedType;

    Json::Value extra(Json::objectValue);
    extra["data"] = processedReels;
    extra["pagination"] = pagination;
    callback(withNoCache(pulse::http::success(extra, drogon::k200OK)));
    return;
  } catch (const std::exception& e) {
    pulse::log::error("Get Reels Error: {}", e.what());
    Json::Value body(Json::objectValue);
    body["success"] = false;
    body["message"] = "Failed to fetch reels";
    callback(withNoCache(pulse::http::json(drogon::k500InternalServerError, body)));
    return;
  }
}

// =========================================================
//  3. TOGGLE REEL LIKE
// =========================================================
void ReelController::toggleLike(const HttpRequestPtr& req,
                                std::function<void(const HttpResponsePtr&)>&& callback,
                                std::string reelId) {
  try {
    Json::Value user = authUser(req);
    std::string userId = authUserId(user);

    // Reel.findById(reelId)
    Json::Value reel(Json::nullValue);
    {
      auto col = pulse::db::collection(pulse::models::reel::kCollection);
      bld::document f;
      if (auto o = bj::tryOid(reelId)) f.append(kvp("_id", *o));
      else                            f.append(kvp("_id", reelId));
      auto found = col.find_one(f.view());
      if (found) reel = bj::toJson(found->view());
    }
    if (!reel.isObject()) {
      Json::Value body(Json::objectValue);
      body["success"] = false;
      body["message"] = "Reel not found";
      callback(pulse::http::json(drogon::k404NotFound, body));
      return;
    }

    // Atomic Like collection toggle.
    auto result = pulse::models::like::toggleLike(
        userId, pulse::models::like::kTargetTypeReel, reelId);

    // Track engagement + notify the author (best-effort) on a like.
    std::string authorId = reelUserId(reel);
    if (result.liked && !authorId.empty()) {
      pulse::models::userengagement::recordSignal(userId, authorId, "likes", 1);

      Json::Value notif(Json::objectValue);
      notif["recipient"] = authorId;
      notif["sender"]    = userId;
      notif["type"]      = "reel_like";
      notif["reel"]      = reelId;
      notif["message"]   = "liked your reel";
      try {
        pulse::models::notification::createNotification(notif);
      } catch (const std::exception& e) {
        pulse::log::error("Notification error: {}", e.what());
      }
    }

    Json::Value data(Json::objectValue);
    data["isLiked"]    = result.liked;
    data["likesCount"] = static_cast<Json::Int64>(result.likeCount);
    callback(pulse::http::ok(data, drogon::k200OK));
    return;
  } catch (const std::exception& e) {
    pulse::log::error("Like Reel Error: {}", e.what());
    Json::Value body(Json::objectValue);
    body["success"] = false;
    body["message"] = "Server error";
    callback(pulse::http::json(drogon::k500InternalServerError, body));
    return;
  }
}

// =========================================================
//  4. TRACK VIEW / WATCH TIME
// =========================================================
void ReelController::trackView(const HttpRequestPtr& req,
                               std::function<void(const HttpResponsePtr&)>&& callback,
                               std::string reelId) {
  try {
    auto bodyPtr = req->getJsonObject();
    Json::Value body = (bodyPtr && bodyPtr->isObject()) ? *bodyPtr : Json::Value(Json::objectValue);
    Json::Value user = authUser(req);
    std::string userId = authUserId(user);

    // Reel.findById(reelId).select('user')
    Json::Value reel(Json::nullValue);
    std::string reelAuthorId;
    {
      auto col = pulse::db::collection(pulse::models::reel::kCollection);
      bld::document f;
      if (auto o = bj::tryOid(reelId)) f.append(kvp("_id", *o));
      else                            f.append(kvp("_id", reelId));
      mongocxx::options::find opts{};
      opts.projection(make_document(kvp("user", 1)));
      auto found = col.find_one(f.view(), opts);
      if (found) {
        reel = bj::toJson(found->view());
        reelAuthorId = reelUserId(reel);
      }
    }
    if (!reel.isObject()) {
      Json::Value b(Json::objectValue);
      b["success"] = false;
      b["message"] = "Reel not found";
      callback(pulse::http::json(drogon::k404NotFound, b));
      return;
    }

    // Normalize watchPercentage to 0..1 (accept 0..1 or 0..100).
    std::optional<double> frac;
    if (body.isMember("watchPercentage") && !body["watchPercentage"].isNull()) {
      const Json::Value& wp = body["watchPercentage"];
      double n = std::numeric_limits<double>::quiet_NaN();
      if (wp.isNumeric()) n = wp.asDouble();
      else if (wp.isString()) { try { n = std::stod(wp.asString()); } catch (...) {} }
      if (!std::isnan(n)) {
        double v = (n > 1.0) ? n / 100.0 : n;
        frac = std::max(0.0, std::min(1.0, v));
      }
    }

    auto col = pulse::db::collection(pulse::models::reel::kCollection);
    bld::document filter;
    if (auto o = bj::tryOid(reelId)) filter.append(kvp("_id", *o));
    else                            filter.append(kvp("_id", reelId));
    auto filterDoc = filter.extract();

    if (frac.has_value()) {
      // Aggregation-pipeline update (Reel.updateOne(filter, [ { $set: {...} } ])):
      // bump views AND maintain a running average watch completion.
      //   newAvg = (a*v + frac)/(v+1).
      double f = *frac;
      mongocxx::pipeline pipe{};
      pipe.append_stage(make_document(kvp("$set", make_document(
          kvp("stats.avgWatchPercentage", make_document(kvp("$let", make_document(
              kvp("vars", make_document(
                  kvp("v", make_document(kvp("$ifNull", make_array("$stats.views", 0)))),
                  kvp("a", make_document(kvp("$ifNull", make_array("$stats.avgWatchPercentage", 0)))))),
              kvp("in", make_document(kvp("$divide", make_array(
                  make_document(kvp("$add", make_array(
                      make_document(kvp("$multiply", make_array("$$a", "$$v"))), f))),
                  make_document(kvp("$add", make_array("$$v", 1))))))))))),
          kvp("stats.views", make_document(kvp("$add", make_array(
              make_document(kvp("$ifNull", make_array("$stats.views", 0))), 1))))))));
      col.update_one(filterDoc.view(), pipe);
    } else {
      // $inc: { 'stats.views': 1 }
      col.update_one(filterDoc.view(),
                     make_document(kvp("$inc", make_document(kvp("stats.views", 1)))));
    }

    // Track engagement for personalization.
    if (!userId.empty() && !reelAuthorId.empty()) {
      pulse::models::userengagement::recordSignal(userId, reelAuthorId, "views", 1);

      if (body.isMember("watchTimeSeconds") && body["watchTimeSeconds"].isNumeric()
          && body["watchTimeSeconds"].asDouble() != 0.0) {
        pulse::models::userengagement::recordSignal(
            userId, reelAuthorId, "totalWatchTimeSeconds",
            body["watchTimeSeconds"].asDouble());
      }
    }

    callback(pulse::http::success(Json::Value(Json::objectValue), drogon::k200OK));
    return;
  } catch (const std::exception& e) {
    pulse::log::error("Track View Error: {}", e.what());
    Json::Value body(Json::objectValue);
    body["success"] = false;
    body["message"] = "Server error";
    callback(pulse::http::json(drogon::k500InternalServerError, body));
    return;
  }
}

// =========================================================
//  5. ADD COMMENT
// =========================================================
void ReelController::addComment(const HttpRequestPtr& req,
                                std::function<void(const HttpResponsePtr&)>&& callback,
                                std::string reelId) {
  try {
    auto bodyPtr = req->getJsonObject();
    Json::Value body = (bodyPtr && bodyPtr->isObject()) ? *bodyPtr : Json::Value(Json::objectValue);
    Json::Value user = authUser(req);
    std::string userId = authUserId(user);

    std::string content = (body.isMember("content") && body["content"].isString())
                              ? body["content"].asString() : "";
    // if (!content) -> 400 Content required.  (Mirrors JS falsy check on body.content.)
    bool contentFalsy = !body.isMember("content") || body["content"].isNull()
                        || (body["content"].isString() && body["content"].asString().empty())
                        || (body["content"].isBool() && !body["content"].asBool())
                        || (body["content"].isNumeric() && body["content"].asDouble() == 0.0);
    if (contentFalsy) {
      Json::Value b(Json::objectValue);
      b["success"] = false;
      b["message"] = "Content required";
      callback(pulse::http::json(drogon::k400BadRequest, b));
      return;
    }

    // Reel.findById(reelId)
    Json::Value reel(Json::nullValue);
    {
      auto col = pulse::db::collection(pulse::models::reel::kCollection);
      bld::document f;
      if (auto o = bj::tryOid(reelId)) f.append(kvp("_id", *o));
      else                            f.append(kvp("_id", reelId));
      auto found = col.find_one(f.view());
      if (found) reel = bj::toJson(found->view());
    }
    if (!reel.isObject()) {
      Json::Value b(Json::objectValue);
      b["success"] = false;
      b["message"] = "Reel not found";
      callback(pulse::http::json(drogon::k404NotFound, b));
      return;
    }

    // ReelComment.create({ reel, author, content, type:type||'text',
    //                      parentComment: parentCommentId||null })
    std::string type = (body.isMember("type") && body["type"].isString()
                        && !body["type"].asString().empty())
                           ? body["type"].asString() : "text";
    std::string parentCommentId;
    if (body.isMember("parentCommentId") && body["parentCommentId"].isString())
      parentCommentId = body["parentCommentId"].asString();

    Json::Value doc(Json::objectValue);
    doc["reel"]          = reelId;
    doc["author"]        = userId;
    doc["content"]       = content;
    doc["type"]          = type;
    doc["parentComment"] = parentCommentId.empty() ? Json::Value(Json::nullValue)
                                                   : Json::Value(parentCommentId);
    Json::Value defaulted = pulse::models::reelcomment::applyDefaults(doc);

    bsoncxx::oid newId;
    {
      bld::document insert;
      insert.append(kvp("_id", newId));
      appendRef(insert, "reel", reelId);
      appendRef(insert, "author", userId);
      insert.append(kvp("content", defaulted["content"].asString()));
      insert.append(kvp("type", type));
      if (parentCommentId.empty()) insert.append(kvp("parentComment", bsoncxx::types::b_null{}));
      else                         appendRef(insert, "parentComment", parentCommentId);
      insert.append(kvp("likes", make_array()));
      long long now = nowMillis();
      insert.append(kvp("createdAt", bsoncxx::types::b_date{std::chrono::milliseconds{now}}));
      insert.append(kvp("updatedAt", bsoncxx::types::b_date{std::chrono::milliseconds{now}}));
      insert.append(kvp("__v", static_cast<std::int64_t>(0)));

      auto col = pulse::db::collection(pulse::models::reelcomment::kCollection);
      col.insert_one(insert.view());
    }

    // Read back the created comment, then populate author + normalize.
    Json::Value responseData;
    {
      auto col = pulse::db::collection(pulse::models::reelcomment::kCollection);
      auto created = col.find_one(make_document(kvp("_id", newId)));
      responseData = created ? bj::toJson(created->view()) : defaulted;
    }
    // newComment.populate('author','+authMethods username profile avatar isVerified')
    {
      Json::Value arr(Json::arrayValue);
      arr.append(responseData);
      populateUsers(arr, "author", /*stats=*/false, /*authMethods=*/true);
      responseData = arr[0u];
    }
    // responseData.author = normalizeUser(newComment.author)
    responseData["author"] = normalizeUser(
        responseData.isMember("author") ? responseData["author"] : Json::Value(Json::nullValue));

    // Reel.findByIdAndUpdate(reelId, { $inc: { commentsCount: 1 } })
    {
      auto col = pulse::db::collection(pulse::models::reel::kCollection);
      bld::document f;
      if (auto o = bj::tryOid(reelId)) f.append(kvp("_id", *o));
      else                            f.append(kvp("_id", reelId));
      col.update_one(f.view(),
                     make_document(kvp("$inc", make_document(kvp("commentsCount", 1)))));
    }

    // Track engagement for personalization.
    std::string reelAuthorId = reelUserId(reel);
    if (!reelAuthorId.empty())
      pulse::models::userengagement::recordSignal(userId, reelAuthorId, "comments", 1);

    callback(pulse::http::ok(responseData, drogon::k201Created));
    return;
  } catch (const std::exception& e) {
    pulse::log::error("Add Comment Error: {}", e.what());
    Json::Value body(Json::objectValue);
    body["success"] = false;
    body["message"] = "Server error";
    callback(pulse::http::json(drogon::k500InternalServerError, body));
    return;
  }
}

// =========================================================
//  6. GET COMMENTS - RANKED
// =========================================================
void ReelController::getComments(const HttpRequestPtr& req,
                                 std::function<void(const HttpResponsePtr&)>&& callback,
                                 std::string reelId) {
  try {
    std::string sortMode = req->getParameter("sort");
    if (sortMode.empty()) sortMode = "best";
    int page = clampParam(req->getParameter("page"), 1, 1, 100);
    int limit = clampParam(req->getParameter("limit"), 30, 1, 50);

    // ReelComment.find({ reel:reelId, parentComment:null }).sort({createdAt:-1})
    //   .skip((page-1)*limit).limit(limit)
    //   .populate('author','username profile avatar isVerified')
    //   .populate('replies', { sort:{createdAt:1}, limit:30 }, author...).lean({virtuals})
    Json::Value comments(Json::arrayValue);
    {
      auto col = pulse::db::collection(pulse::models::reelcomment::kCollection);
      bld::document f;
      if (auto o = bj::tryOid(reelId)) f.append(kvp("reel", *o));
      else                            f.append(kvp("reel", reelId));
      f.append(kvp("parentComment", bsoncxx::types::b_null{}));
      mongocxx::options::find opts{};
      opts.sort(make_document(kvp("createdAt", -1)));
      opts.skip(static_cast<std::int64_t>((page - 1) * limit));
      opts.limit(static_cast<std::int64_t>(limit));
      auto cursor = col.find(f.view(), opts);
      for (const auto& d : cursor) comments.append(bj::toJson(d));
    }

    // replies virtual: ReelComment where parentComment == this._id, sorted
    // createdAt:1, limited 30, author populated.
    {
      auto col = pulse::db::collection(pulse::models::reelcomment::kCollection);
      for (auto& c : comments) {
        Json::Value replies(Json::arrayValue);
        std::string cid = reelHexId(c);
        bld::document rf;
        if (auto o = bj::tryOid(cid)) rf.append(kvp("parentComment", *o));
        else                          rf.append(kvp("parentComment", cid));
        mongocxx::options::find ropts{};
        ropts.sort(make_document(kvp("createdAt", 1)));
        ropts.limit(30);
        auto rcursor = col.find(rf.view(), ropts);
        for (const auto& rd : rcursor) replies.append(bj::toJson(rd));
        populateUsers(replies, "author", /*stats=*/false, /*authMethods=*/false);
        c["replies"] = replies;
      }
    }
    populateUsers(comments, "author", /*stats=*/false, /*authMethods=*/false);

    // Reel.findById(reelId).select('user').lean() — OP boost id.
    std::string opId;
    {
      auto col = pulse::db::collection(pulse::models::reel::kCollection);
      bld::document f;
      if (auto o = bj::tryOid(reelId)) f.append(kvp("_id", *o));
      else                            f.append(kvp("_id", reelId));
      mongocxx::options::find opts{};
      opts.projection(make_document(kvp("user", 1)));
      auto found = col.find_one(f.view(), opts);
      if (found) {
        Json::Value reel = bj::toJson(found->view());
        opId = reelUserId(reel);
      }
    }

    // CommentsAlgo.rankComments(comments, { mode:sortMode, opId, includeReplies:true })
    Json::Value ranked = rankComments(comments, sortMode, opId);

    // Process for response.
    Json::Value processed(Json::arrayValue);
    for (const auto& src : ranked) {
      Json::Value comment = src;
      comment["author"] = normalizeUser(
          comment.isMember("author") ? comment["author"] : Json::Value(Json::nullValue));

      Json::Value repliesOut(Json::arrayValue);
      if (comment.isMember("replies") && comment["replies"].isArray()) {
        for (const auto& rsrc : comment["replies"]) {
          Json::Value reply = rsrc;
          reply["author"] = normalizeUser(
              reply.isMember("author") ? reply["author"] : Json::Value(Json::nullValue));
          reply.removeMember("_score");
          repliesOut.append(reply);
        }
      }
      comment["replies"] = repliesOut;
      comment.removeMember("_score");
      processed.append(comment);
    }

    callback(pulse::http::ok(processed, drogon::k200OK));
    return;
  } catch (const std::exception& e) {
    pulse::log::error("Get Comments Error: {}", e.what());
    Json::Value body(Json::objectValue);
    body["success"] = false;
    body["message"] = "Server error";
    callback(pulse::http::json(drogon::k500InternalServerError, body));
    return;
  }
}

// =========================================================
//  7. TOGGLE COMMENT LIKE
// =========================================================
void ReelController::toggleCommentLike(const HttpRequestPtr& req,
                                       std::function<void(const HttpResponsePtr&)>&& callback,
                                       std::string reelId, std::string commentId) {
  (void)reelId;  // :reelId is part of the path but unused by the JS handler.
  try {
    Json::Value user = authUser(req);
    std::string userId = authUserId(user);

    // ReelComment.findById(commentId).select('_id').lean()
    bool exists = false;
    {
      auto col = pulse::db::collection(pulse::models::reelcomment::kCollection);
      bld::document f;
      if (auto o = bj::tryOid(commentId)) f.append(kvp("_id", *o));
      else                                f.append(kvp("_id", commentId));
      mongocxx::options::find opts{};
      opts.projection(make_document(kvp("_id", 1)));
      auto found = col.find_one(f.view(), opts);
      exists = static_cast<bool>(found);
    }
    if (!exists) {
      Json::Value b(Json::objectValue);
      b["success"] = false;
      b["message"] = "Comment not found";
      callback(pulse::http::json(drogon::k404NotFound, b));
      return;
    }

    // Atomic toggle via the Like collection (targetType 'comment').
    auto result = pulse::models::like::toggleLike(
        userId, pulse::models::like::kTargetTypeComment, commentId);

    Json::Value data(Json::objectValue);
    data["isLiked"]    = result.liked;
    data["likesCount"] = static_cast<Json::Int64>(result.likeCount);
    callback(pulse::http::ok(data, drogon::k200OK));
    return;
  } catch (const std::exception& e) {
    pulse::log::error("Toggle Comment Like Error: {}", e.what());
    Json::Value body(Json::objectValue);
    body["success"] = false;
    body["message"] = "Server error";
    callback(pulse::http::json(drogon::k500InternalServerError, body));
    return;
  }
}

// =========================================================
//  8. SHARE REEL
// =========================================================
void ReelController::shareReel(const HttpRequestPtr& req,
                               std::function<void(const HttpResponsePtr&)>&& callback,
                               std::string reelId) {
  try {
    Json::Value user = authUser(req);
    std::string userId = authUserId(user);

    // Reel.findById(reelId)
    Json::Value reel(Json::nullValue);
    {
      auto col = pulse::db::collection(pulse::models::reel::kCollection);
      bld::document f;
      if (auto o = bj::tryOid(reelId)) f.append(kvp("_id", *o));
      else                            f.append(kvp("_id", reelId));
      auto found = col.find_one(f.view());
      if (found) reel = bj::toJson(found->view());
    }
    if (!reel.isObject()) {
      Json::Value b(Json::objectValue);
      b["success"] = false;
      b["message"] = "Reel not found";
      callback(pulse::http::json(drogon::k404NotFound, b));
      return;
    }

    // Reel.findByIdAndUpdate(reelId, { $inc: { 'stats.shares': 1 } })
    {
      auto col = pulse::db::collection(pulse::models::reel::kCollection);
      bld::document f;
      if (auto o = bj::tryOid(reelId)) f.append(kvp("_id", *o));
      else                            f.append(kvp("_id", reelId));
      col.update_one(f.view(),
                     make_document(kvp("$inc", make_document(kvp("stats.shares", 1)))));
    }

    // Track engagement.
    std::string reelAuthorId = reelUserId(reel);
    if (!userId.empty() && !reelAuthorId.empty())
      pulse::models::userengagement::recordSignal(userId, reelAuthorId, "shares", 1);

    callback(pulse::http::success(Json::Value(Json::objectValue), drogon::k200OK));
    return;
  } catch (const std::exception& e) {
    pulse::log::error("Share Reel Error: {}", e.what());
    Json::Value body(Json::objectValue);
    body["success"] = false;
    body["message"] = "Server error";
    callback(pulse::http::json(drogon::k500InternalServerError, body));
    return;
  }
}

} // namespace pulse::controllers
