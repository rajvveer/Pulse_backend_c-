// post_controller.cc — C++ port of src/controllers/postController.js.
// See include/pulse/controllers/post_controller.hpp for the route table.
//
// 1:1 functional parity with the Express controller. Every handler mirrors the
// corresponding exports.* function: same validation, same Mongo queries, same
// response JSON shapes / status codes / messages. The JS error responses use the
// shape { success:false, message:'...' } (NOT the {error,code} contract), so
// those are built directly via pulse::http::json to match byte-for-byte.
//
// Business logic delegates to the already-ported models/services and is NOT
// re-implemented:
//   * Like.toggleLike / isLikedBy / getLikeCount / getLikedIds /
//     getBatchLikeCounts        -> pulse::models::like::*
//   * Follow.isFollowing         -> pulse::models::follow::isFollowing
//   * Notification.createNotification -> pulse::models::notification::createNotification
//   * UserBehavior.recordLike    -> pulse::models::userbehavior::recordLike
//   * UserEngagement.recordSignal-> pulse::models::userengagement::recordSignal
//   * PulseScore.getOrCreate/recordAction -> pulse::models::pulsescore::*
//   * feedbackService.recordEngagement    -> pulse::feedbackService().recordEngagement
//   * cacheService.getOrSet      -> pulse::cache().getOrSet (trending hashtags)
//   * model defaults/sanitize    -> pulse::models::post/comment::applyDefaults/...
//
// Direct CRUD (Post.findById/updateOne, User.findByIdAndUpdate, Comment.find,
// $text search, author/replies "populate") is done with mongocxx here — these
// have no dedicated model helper, mirroring how src/services/feed_service.cc
// issues its own queries.
//
// NOTE on the non-blocking signal side effects: the JS fired several
// `.catch(()=>{})` best-effort signals (DNAMatchAlgo.recordInteraction,
// PulseScore, UserBehavior, UserEngagement, feedbackService, Notification).
// These never affect the HTTP response (shape/status). They are recorded here
// where a direct port exists; DNAMatchAlgo.recordInteraction has no C++ port
// (its DB-bound orchestration + VibeClassifier were not ported as a callable
// unit) so that one signal is intentionally omitted — it is purely advisory and
// the JS swallowed all its errors.
#include "pulse/controllers/post_controller.hpp"

#include "pulse/http_response.hpp"
#include "pulse/db.hpp"
#include "pulse/cache.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"

#include "pulse/models/post.hpp"
#include "pulse/models/comment.hpp"
#include "pulse/models/like.hpp"
#include "pulse/models/follow.hpp"
#include "pulse/models/bookmark.hpp"
#include "pulse/models/user.hpp"
#include "pulse/models/notification.hpp"
#include "pulse/models/userbehavior.hpp"
#include "pulse/models/userengagement.hpp"
#include "pulse/models/pulsescore.hpp"

#include "pulse/services/feedback_service.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/oid.hpp>

#include <mongocxx/collection.hpp>
#include <mongocxx/cursor.hpp>
#include <mongocxx/pipeline.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/exception/exception.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace pulse::controllers {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;
using bld::make_array;
namespace bj = pulse::bsonjson;

namespace {

// ── now in epoch millis (Date.now()) ───────────────────────────────────────
long long nowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch()).count();
}
bsoncxx::types::b_date dateFromMillis(long long ms) {
  return bsoncxx::types::b_date{std::chrono::milliseconds{ms}};
}

// ── { success:false, message } response (the JS error shape on this router) ──
HttpResponsePtr fail(HttpStatusCode code, const std::string& message) {
  Json::Value body(Json::objectValue);
  body["success"] = false;
  body["message"] = message;
  return pulse::http::json(code, std::move(body));
}

// ── JSON body accessor (req->getJsonObject) ─────────────────────────────────
const Json::Value& bodyOrEmpty(const std::shared_ptr<Json::Value>& body,
                               Json::Value& empty) {
  return (body && body->isObject()) ? *body : empty;
}

// parseInt(raw) || fallback, then clamp to [lo, hi] — exactly the JS
// Math.min(Math.max(parseInt(...) || fallback, lo), hi) idiom. parseInt reads a
// leading integer prefix; a non-numeric (NaN) OR a 0 result both fall back via
// the JS `|| fallback`.
int clampedInt(const std::string& raw, int fallback, int lo, int hi) {
  int v = fallback;
  if (!raw.empty()) {
    char* end = nullptr;
    long parsed = std::strtol(raw.c_str(), &end, 10);
    // parseInt yields NaN when no leading digits were consumed.
    v = (end == raw.c_str()) ? fallback : static_cast<int>(parsed);
  }
  if (v == 0) v = fallback;  // (parseInt(...) || fallback): 0 is falsy in JS.
  return std::max(lo, std::min(hi, v));
}

// ── string trim (JS String.prototype.trim) ──────────────────────────────────
std::string trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\n\r\f\v");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\n\r\f\v");
  return s.substr(b, e - b + 1);
}

// ── author hex id from a post doc (author may be a ref string or populated) ──
std::string authorHex(const Json::Value& post) {
  if (!post.isObject() || !post.isMember("author")) return "";
  const Json::Value& a = post["author"];
  if (a.isString()) return a.asString();
  if (a.isObject() && a.isMember("_id")) {
    const Json::Value& id = a["_id"];
    if (id.isString()) return id.asString();
  }
  return "";
}

// ── maskAnonymousPost(post) — EXACT controller helper (avatar:null,
//    profile:{avatar:null}) ──────────────────────────────────────────────────
Json::Value maskAnonymousPost(Json::Value post) {
  if (post.isObject() && post.isMember("isAnonymous") &&
      post["isAnonymous"].asBool()) {
    Json::Value author(Json::objectValue);
    author["_id"]        = Json::Value(Json::nullValue);
    author["username"]   = "anonymous";
    author["name"]       = "Anonymous";
    author["avatar"]     = Json::Value(Json::nullValue);
    Json::Value profile(Json::objectValue);
    profile["avatar"]    = Json::Value(Json::nullValue);
    author["profile"]    = profile;
    author["isVerified"] = false;
    post["author"] = author;
  }
  return post;
}

// ── .populate('author', 'username name avatar profile isVerified') ───────────
// Hydrate each post/comment author ObjectId into the projected user subdocument
// in one query. Authors that cannot be resolved keep the original ref (Mongoose
// populate leaves a dangling ref as-is).
void populateAuthors(Json::Value& docs) {
  if (!docs.isArray() || docs.empty()) return;
  std::vector<std::string> ids;
  for (const auto& d : docs) {
    if (!d.isMember("author")) continue;
    const Json::Value& a = d["author"];
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
    auto projection = make_document(kvp("username", 1), kvp("name", 1),
                                    kvp("avatar", 1), kvp("profile", 1),
                                    kvp("isVerified", 1));
    mongocxx::options::find opts{};
    opts.projection(projection.view());
    auto col = pulse::db::collection(pulse::models::user::kCollection);
    auto cursor = col.find(filter.view(), opts);
    for (const auto& doc : cursor) {
      Json::Value u = bj::toJson(doc);
      if (u.isMember("_id") && u["_id"].isString())
        byId[u["_id"].asString()] = u;
    }
  } catch (const std::exception& e) {
    pulse::log::error("[posts] author populate failed: {}", e.what());
    return;
  }

  for (auto& d : docs) {
    if (!d.isMember("author")) continue;
    const Json::Value& a = d["author"];
    std::string hex;
    if (a.isString()) hex = a.asString();
    else if (a.isObject() && a.isMember("_id") && a["_id"].isString())
      hex = a["_id"].asString();
    if (!hex.empty() && byId.isMember(hex)) d["author"] = byId[hex];
  }
}

// Single-doc author populate (wraps the array variant).
void populateAuthorSingle(Json::Value& doc) {
  if (!doc.isObject()) return;
  Json::Value arr(Json::arrayValue);
  arr.append(doc);
  populateAuthors(arr);
  doc = arr[0u];
}

// Find one post by id (Post.findById). Returns nullopt on bad id / not found.
std::optional<Json::Value> findPostById(const std::string& postId) {
  auto oid = bj::tryOid(postId);
  if (!oid) return std::nullopt;
  try {
    auto col = pulse::db::collection(pulse::models::post::kCollection);
    auto doc = col.find_one(make_document(kvp("_id", *oid)));
    if (!doc) return std::nullopt;
    return bj::toJson(doc->view());
  } catch (const std::exception& e) {
    pulse::log::error("[posts] findPostById failed: {}", e.what());
    return std::nullopt;
  }
}

// Persist a PulseScore document mutated by recordAction (the JS `ps.save()`),
// best-effort. recordAction has no model-level persist, so we replace_one the
// document, re-splicing reference / date fields as their proper BSON types so
// the `user` ObjectId and timestamps are never downgraded to strings.
void persistPulseScore(const Json::Value& psIn) {
  try {
    if (!psIn.isObject() || !psIn.isMember("_id") || !psIn["_id"].isString())
      return;
    auto idOid = bj::tryOid(psIn["_id"].asString());
    if (!idOid) return;

    Json::Value ps = pulse::models::pulsescore::sanitizeForOutput(psIn);

    // Pull out fields that must keep real BSON types.
    std::optional<bsoncxx::oid> userOid;
    if (ps.isMember("user") && ps["user"].isString())
      userOid = bj::tryOid(ps["user"].asString());

    long long nowMs = nowMillis();
    ps.removeMember("_id");
    ps.removeMember("user");
    // Dates: re-stamp as BSON dates after the JSON conversion.
    ps.removeMember("createdAt");
    ps.removeMember("updatedAt");
    ps.removeMember("lastComputedAt");

    auto baseDoc = bj::fromJson(ps);

    bld::document repl;
    repl.append(kvp("_id", *idOid));
    if (userOid) repl.append(kvp("user", *userOid));
    for (const auto& el : baseDoc.view()) repl.append(kvp(el.key(), el.get_value()));
    repl.append(kvp("lastComputedAt", dateFromMillis(nowMs)));
    if (psIn.isMember("createdAt"))
      repl.append(kvp("createdAt",
          dateFromMillis(nowMs)));  // preserve existence; value re-stamped
    repl.append(kvp("updatedAt", dateFromMillis(nowMs)));

    auto col = pulse::db::collection(pulse::models::pulsescore::kCollection);
    col.replace_one(make_document(kvp("_id", *idOid)), repl.view());
  } catch (...) { /* best-effort: signal persistence never blocks the response */ }
}

} // namespace

// ===========================================================================
//  CREATE POST  — POST /api/v1/posts
// ===========================================================================
void PostController::createPost(const HttpRequestPtr& req,
                                std::function<void(const HttpResponsePtr&)>&& cb) {
  try {
    Json::Value user = req->getAttributes()->get<Json::Value>("user");
    const std::string userId = user["userId"].asString();

    Json::Value empty(Json::objectValue);
    const Json::Value& b = bodyOrEmpty(req->getJsonObject(), empty);

    // const { text, media, location, visibility, allowComments, isAnonymous } = req.body;
    // new Post({ author, content:{ text, media: media||[] }, location,
    //            visibility: visibility||'public',
    //            allowComments: allowComments!==undefined ? allowComments : true,
    //            isAnonymous: isAnonymous||false })
    Json::Value doc(Json::objectValue);
    doc["author"] = userId;

    Json::Value content(Json::objectValue);
    if (b.isMember("text")) content["text"] = b["text"];
    content["media"] = (b.isMember("media") && b["media"].isArray())
                           ? b["media"] : Json::Value(Json::arrayValue);
    doc["content"] = content;

    if (b.isMember("location") && !b["location"].isNull())
      doc["location"] = b["location"];

    doc["visibility"] = (b.isMember("visibility") && b["visibility"].isString() &&
                         !b["visibility"].asString().empty())
                            ? b["visibility"] : Json::Value("public");

    doc["allowComments"] = b.isMember("allowComments") ? b["allowComments"]
                                                       : Json::Value(true);
    doc["isAnonymous"] = (b.isMember("isAnonymous") && b["isAnonymous"].asBool())
                             ? Json::Value(true) : Json::Value(false);

    // Apply schema defaults (stats, vibe, flags, hashtags from text, timestamps).
    Json::Value toInsert = pulse::models::post::applyDefaults(doc);

    // Build the BSON insert. author must be stored as a real ObjectId; the rest
    // of the fields convert straight from JSON (mirrors pulse_drop_controller's
    // create-post path): remove author, convert the doc, then splice the oid in.
    auto authorOid = bj::tryOid(userId);
    Json::Value forBson = toInsert;
    forBson.removeMember("author");
    auto baseDoc = bj::fromJson(forBson);

    bld::document insert;
    if (authorOid) insert.append(kvp("author", *authorOid));
    for (const auto& el : baseDoc.view()) insert.append(kvp(el.key(), el.get_value()));

    std::string newId;
    {
      auto col = pulse::db::collection(pulse::models::post::kCollection);
      auto res = col.insert_one(insert.view());
      if (!res) return cb(fail(drogon::k500InternalServerError, "Failed to create post"));
      newId = bj::oidToHex(res->inserted_id().get_oid().value);

      // await User.findByIdAndUpdate(userId, { $inc: { 'stats.posts': 1 } })
      if (authorOid) {
        try {
          auto users = pulse::db::collection(pulse::models::user::kCollection);
          users.update_one(make_document(kvp("_id", *authorOid)),
                           make_document(kvp("$inc",
                               make_document(kvp("stats.posts", 1)))));
        } catch (const std::exception& e) {
          pulse::log::warn("[posts] createPost user stats inc failed: {}", e.what());
        }
      }
    }

    // 📊 Record Pulse Score signal (non-blocking).
    try {
      Json::Value ps = pulse::models::pulsescore::getOrCreate(userId);
      ps = pulse::models::pulsescore::recordAction(ps, "post");
      // JS: if (post.image || post.media?.length > 0) — note these are the
      // TOP-LEVEL post.image / post.media (NOT content.media); the Post schema
      // stores media under content.media, so this matches the JS exactly,
      // including its quirk of reading fields that are normally absent.
      bool hasImage = toInsert.isMember("image") && !toInsert["image"].isNull() &&
                      !(toInsert["image"].isString() && toInsert["image"].asString().empty());
      bool hasTopMedia = toInsert.isMember("media") && toInsert["media"].isArray() &&
                         !toInsert["media"].empty();
      if (hasImage || hasTopMedia)
        ps = pulse::models::pulsescore::recordAction(ps, "media_post");
      persistPulseScore(ps);
    } catch (...) { /* best-effort */ }

    // Re-fetch + populate author for the response shape.
    Json::Value created;
    if (auto found = findPostById(newId)) created = *found;
    else created = toInsert;  // fallback
    populateAuthorSingle(created);
    created = pulse::models::post::sanitizeForOutput(created);

    // Mask if anonymous before sending response.
    Json::Value masked = maskAnonymousPost(created);

    Json::Value out(Json::objectValue);
    out["data"] = masked;
    cb(pulse::http::success(out, drogon::k201Created));
  } catch (const std::exception& e) {
    pulse::log::error("Create post error: {}", e.what());
    cb(fail(drogon::k500InternalServerError, e.what()));
  }
}

// ===========================================================================
//  GET SINGLE POST  — GET /api/v1/posts/:postId
// ===========================================================================
void PostController::getPost(const HttpRequestPtr& req,
                             std::function<void(const HttpResponsePtr&)>&& cb,
                             std::string postId) {
  try {
    Json::Value user = req->getAttributes()->get<Json::Value>("user");
    const std::string userId = user["userId"].asString();

    auto found = findPostById(postId);
    // if (!post || !post.isActive) -> 404
    if (!found || !(*found).isMember("isActive") || !(*found)["isActive"].asBool())
      return cb(fail(drogon::k404NotFound, "Post not found"));

    Json::Value post = *found;

    // .populate('author', ...) and .populate('originalPost') for the visibility
    // check + response. (originalPost populate matches the JS; we hydrate author.)
    Json::Value postForResp = post;
    populateAuthorSingle(postForResp);

    // Enforce visibility — 404 (not 403) to avoid confirming existence.
    const std::string aId = authorHex(postForResp);
    const bool isOwner = (aId == userId);
    if (!isOwner) {
      const std::string vis = postForResp.get("visibility", "public").asString();
      if (vis == "private")
        return cb(fail(drogon::k404NotFound, "Post not found"));
      if (vis == "followers") {
        bool follows = pulse::models::follow::isFollowing(userId, aId);
        if (!follows)
          return cb(fail(drogon::k404NotFound, "Post not found"));
      }
    }

    // Increment view count atomically: Post.updateOne({_id},{$inc:{'stats.views':1}})
    if (auto o = bj::tryOid(postId)) {
      try {
        auto col = pulse::db::collection(pulse::models::post::kCollection);
        col.update_one(make_document(kvp("_id", *o)),
                       make_document(kvp("$inc",
                           make_document(kvp("stats.views", 1)))));
      } catch (const std::exception& e) {
        pulse::log::warn("[posts] view inc failed: {}", e.what());
      }
    }

    Json::Value postObj = maskAnonymousPost(postForResp);
    postObj = pulse::models::post::sanitizeForOutput(postObj);

    // [isLiked, likeCount, bookmarkDoc] = Promise.all([...])
    bool isLiked = pulse::models::like::isLikedBy(
        userId, pulse::models::like::kTargetTypePost, postId);
    long long likeCount = pulse::models::like::getLikeCount(
        pulse::models::like::kTargetTypePost, postId);

    bool isBookmarked = false;
    try {
      auto uOid = bj::tryOid(userId);
      auto pOid = bj::tryOid(postId);
      if (uOid && pOid) {
        auto col = pulse::db::collection(pulse::models::bookmark::kCollection);
        auto bm = col.find_one(make_document(
            kvp("user", *uOid), kvp("itemId", *pOid), kvp("itemType", "post")));
        isBookmarked = static_cast<bool>(bm);
      }
    } catch (const std::exception& e) {
      pulse::log::warn("[posts] bookmark lookup failed: {}", e.what());
    }

    // data: { ...postObj, isLiked, likesCount: likeCount, isBookmarked }
    Json::Value data = postObj;
    data["isLiked"] = isLiked;
    data["likesCount"] = static_cast<Json::Int64>(likeCount);
    data["isBookmarked"] = isBookmarked;

    cb(pulse::http::ok(data));
  } catch (const std::exception& e) {
    pulse::log::error("Get post error: {}", e.what());
    cb(fail(drogon::k500InternalServerError, e.what()));
  }
}

// ===========================================================================
//  GET USER POSTS  — GET /api/v1/posts/user/:username
// ===========================================================================
void PostController::getUserPosts(const HttpRequestPtr& req,
                                  std::function<void(const HttpResponsePtr&)>&& cb,
                                  std::string username) {
  try {
    Json::Value user = req->getAttributes()->get<Json::Value>("user");
    const std::string currentUserId =
        user.isMember("userId") ? user["userId"].asString() : "";

    int page  = clampedInt(req->getParameter("page"), 1, 1, 50);
    int limit = clampedInt(req->getParameter("limit"), 20, 1, 50);

    // const user = await User.findOne({ username }).select('_id');
    std::string targetUserId;
    {
      auto col = pulse::db::collection(pulse::models::user::kCollection);
      auto proj = make_document(kvp("_id", 1));
      mongocxx::options::find opts{};
      opts.projection(proj.view());
      auto doc = col.find_one(make_document(kvp("username", username)), opts);
      if (!doc) return cb(fail(drogon::k404NotFound, "User not found"));
      Json::Value u = bj::toJson(doc->view());
      targetUserId = u["_id"].asString();
    }

    const bool isOwnProfile =
        !currentUserId.empty() && targetUserId == currentUserId;

    // Build query: { author: user._id, isActive: true [, isAnonymous: false] }
    bld::document filter;
    if (auto o = bj::tryOid(targetUserId)) filter.append(kvp("author", *o));
    filter.append(kvp("isActive", true));
    if (!isOwnProfile) filter.append(kvp("isAnonymous", false));

    mongocxx::options::find opts{};
    opts.sort(make_document(kvp("isPinned", -1), kvp("createdAt", -1)));
    opts.limit(limit);
    opts.skip(static_cast<std::int64_t>((page - 1) * limit));

    Json::Value posts(Json::arrayValue);
    {
      auto col = pulse::db::collection(pulse::models::post::kCollection);
      auto cursor = col.find(filter.view(), opts);
      for (const auto& d : cursor)
        posts.append(pulse::models::post::sanitizeForOutput(bj::toJson(d)));
    }
    populateAuthors(posts);

    Json::Value out(Json::objectValue);
    out["data"] = posts;
    Json::Value pagination(Json::objectValue);
    pagination["page"]    = page;
    pagination["limit"]   = limit;
    pagination["hasMore"] = (static_cast<int>(posts.size()) == limit);
    out["pagination"] = pagination;
    cb(pulse::http::success(out));
  } catch (const std::exception& e) {
    pulse::log::error("Get user posts error: {}", e.what());
    cb(fail(drogon::k500InternalServerError, e.what()));
  }
}

// ===========================================================================
//  GET MY POSTS  — GET /api/v1/posts/me/posts
// ===========================================================================
void PostController::getMyPosts(const HttpRequestPtr& req,
                                std::function<void(const HttpResponsePtr&)>&& cb) {
  try {
    Json::Value user = req->getAttributes()->get<Json::Value>("user");
    const std::string userId = user["userId"].asString();

    int page  = clampedInt(req->getParameter("page"), 1, 1, 50);
    int limit = clampedInt(req->getParameter("limit"), 20, 1, 50);

    // Post.find({ author: userId, isActive: true }) — owner sees anonymous too.
    bld::document filter;
    if (auto o = bj::tryOid(userId)) filter.append(kvp("author", *o));
    filter.append(kvp("isActive", true));

    mongocxx::options::find opts{};
    opts.sort(make_document(kvp("isPinned", -1), kvp("createdAt", -1)));
    opts.limit(limit);
    opts.skip(static_cast<std::int64_t>((page - 1) * limit));

    Json::Value posts(Json::arrayValue);
    {
      auto col = pulse::db::collection(pulse::models::post::kCollection);
      auto cursor = col.find(filter.view(), opts);
      for (const auto& d : cursor)
        posts.append(pulse::models::post::sanitizeForOutput(bj::toJson(d)));
    }
    populateAuthors(posts);

    Json::Value out(Json::objectValue);
    out["data"] = posts;
    Json::Value pagination(Json::objectValue);
    pagination["page"]    = page;
    pagination["limit"]   = limit;
    pagination["hasMore"] = (static_cast<int>(posts.size()) == limit);
    out["pagination"] = pagination;
    cb(pulse::http::success(out));
  } catch (const std::exception& e) {
    pulse::log::error("Get my posts error: {}", e.what());
    cb(fail(drogon::k500InternalServerError, e.what()));
  }
}

// ===========================================================================
//  TOGGLE LIKE  — POST /api/v1/posts/:postId/like
// ===========================================================================
void PostController::toggleLike(const HttpRequestPtr& req,
                                std::function<void(const HttpResponsePtr&)>&& cb,
                                std::string postId) {
  try {
    Json::Value user = req->getAttributes()->get<Json::Value>("user");
    const std::string userId = user["userId"].asString();

    auto found = findPostById(postId);
    if (!found) return cb(fail(drogon::k404NotFound, "Post not found"));
    Json::Value post = *found;

    // const { liked, likeCount } = await Like.toggleLike(userId, 'post', postId);
    auto toggle = pulse::models::like::toggleLike(
        userId, pulse::models::like::kTargetTypePost, postId);

    // Sync the cached stats.likes count atomically.
    if (auto o = bj::tryOid(postId)) {
      try {
        auto col = pulse::db::collection(pulse::models::post::kCollection);
        col.update_one(make_document(kvp("_id", *o)),
                       make_document(kvp("$set",
                           make_document(kvp("stats.likes",
                               static_cast<std::int64_t>(toggle.likeCount))))));
      } catch (const std::exception& e) {
        pulse::log::warn("[posts] stats.likes sync failed: {}", e.what());
      }
    }

    if (toggle.liked) {
      const std::string aId = authorHex(post);

      // UserBehavior.recordLike(userId, post) (non-blocking)
      try { pulse::models::userbehavior::recordLike(userId, post); } catch (...) {}
      // UserEngagement.recordSignal(userId, authorId, 'likes', 1) (non-blocking)
      try { pulse::models::userengagement::recordSignal(userId, aId, "likes", 1); }
      catch (...) {}
      // feedbackService.recordEngagement (non-blocking)
      try {
        pulse::feedbackService().recordEngagement(userId, post, "post", "like");
      } catch (...) {}
      // PulseScore signals (non-blocking): like_given (user), like_received (author)
      try {
        Json::Value ps = pulse::models::pulsescore::getOrCreate(userId);
        ps = pulse::models::pulsescore::recordAction(ps, "like_given");
        persistPulseScore(ps);
      } catch (...) {}
      try {
        Json::Value ps = pulse::models::pulsescore::getOrCreate(aId);
        ps = pulse::models::pulsescore::recordAction(ps, "like_received");
        persistPulseScore(ps);
      } catch (...) {}

      // Notification.createNotification (non-blocking)
      try {
        Json::Value n(Json::objectValue);
        n["recipient"] = aId;
        n["sender"]    = userId;
        n["type"]      = "like";
        n["post"]      = postId;
        n["message"]   = "liked your post";
        pulse::models::notification::createNotification(n);
      } catch (const std::exception& e) {
        pulse::log::error("Notification error: {}", e.what());
      }
    }

    Json::Value data(Json::objectValue);
    data["isLiked"]   = toggle.liked;
    data["likeCount"] = static_cast<Json::Int64>(toggle.likeCount);
    cb(pulse::http::ok(data));
  } catch (const std::exception& e) {
    pulse::log::error("Toggle like error: {}", e.what());
    cb(fail(drogon::k500InternalServerError, e.what()));
  }
}

// ===========================================================================
//  ADD COMMENT  — POST /api/v1/posts/:postId/comments
// ===========================================================================
void PostController::addComment(const HttpRequestPtr& req,
                                std::function<void(const HttpResponsePtr&)>&& cb,
                                std::string postId) {
  try {
    Json::Value user = req->getAttributes()->get<Json::Value>("user");
    const std::string userId = user["userId"].asString();

    Json::Value empty(Json::objectValue);
    const Json::Value& b = bodyOrEmpty(req->getJsonObject(), empty);

    auto found = findPostById(postId);
    if (!found) return cb(fail(drogon::k404NotFound, "Post not found"));
    Json::Value post = *found;

    // if (!post.allowComments) -> 403 'Comments disabled'
    bool allowComments = post.get("allowComments", true).asBool();
    if (!allowComments)
      return cb(fail(drogon::k403Forbidden, "Comments disabled"));

    // const { content, parentCommentId, gif } = req.body;
    std::string content = (b.isMember("content") && b["content"].isString())
                              ? b["content"].asString() : "";
    std::string contentTrimmed = trim(content);
    bool hasGifUrl = b.isMember("gif") && b["gif"].isObject() &&
                     b["gif"].isMember("url") && b["gif"]["url"].isString() &&
                     !b["gif"]["url"].asString().empty();

    // if (!content?.trim() && !gif?.url) -> 400
    if (contentTrimmed.empty() && !hasGifUrl)
      return cb(fail(drogon::k400BadRequest, "Comment must have text or GIF"));

    bool hasParent = b.isMember("parentCommentId") &&
                     b["parentCommentId"].isString() &&
                     !b["parentCommentId"].asString().empty();
    std::string parentCommentId = hasParent ? b["parentCommentId"].asString() : "";

    // new Comment({ post, author, content: content?.trim()||'', gif: gif||null,
    //               parentComment: parentCommentId||null })
    Json::Value cdoc(Json::objectValue);
    cdoc["post"]    = postId;
    cdoc["author"]  = userId;
    cdoc["content"] = contentTrimmed;
    cdoc["gif"] = (b.isMember("gif") && !b["gif"].isNull()) ? b["gif"]
                                                            : Json::Value(Json::nullValue);
    cdoc["parentComment"] = hasParent ? Json::Value(parentCommentId)
                                      : Json::Value(Json::nullValue);

    Json::Value toInsert = pulse::models::comment::applyDefaults(cdoc);

    // Build BSON insert. Reference fields (post/author/parentComment) must be
    // real ObjectIds; splice them in after converting the rest of the doc.
    std::string newCommentId;
    {
      auto postOid   = bj::tryOid(postId);
      auto authorOid = bj::tryOid(userId);
      auto parentOid = hasParent ? bj::tryOid(parentCommentId) : std::nullopt;

      Json::Value forBson = toInsert;
      forBson.removeMember("post");
      forBson.removeMember("author");
      forBson.removeMember("parentComment");
      auto baseDoc = bj::fromJson(forBson);

      bld::document insert;
      if (postOid)   insert.append(kvp("post", *postOid));
      if (authorOid) insert.append(kvp("author", *authorOid));
      // parentComment: ObjectId when present, else BSON null (schema default).
      if (parentOid) insert.append(kvp("parentComment", *parentOid));
      else           insert.append(kvp("parentComment", bsoncxx::types::b_null{}));
      for (const auto& el : baseDoc.view()) insert.append(kvp(el.key(), el.get_value()));

      auto col = pulse::db::collection(pulse::models::comment::kCollection);
      auto res = col.insert_one(insert.view());
      if (!res) return cb(fail(drogon::k500InternalServerError, "Failed to add comment"));
      newCommentId = bj::oidToHex(res->inserted_id().get_oid().value);
    }

    // Atomic increment: Post.updateOne({_id},{$inc:{'stats.comments':1}})
    if (auto o = bj::tryOid(postId)) {
      try {
        auto col = pulse::db::collection(pulse::models::post::kCollection);
        col.update_one(make_document(kvp("_id", *o)),
                       make_document(kvp("$inc",
                           make_document(kvp("stats.comments", 1)))));
      } catch (const std::exception& e) {
        pulse::log::warn("[posts] comment count inc failed: {}", e.what());
      }
    }

    // if (parentCommentId) Comment.findByIdAndUpdate(parentCommentId,
    //                        { $push: { replies: comment._id } })
    if (hasParent) {
      try {
        auto parentOid = bj::tryOid(parentCommentId);
        auto childOid  = bj::tryOid(newCommentId);
        if (parentOid && childOid) {
          auto col = pulse::db::collection(pulse::models::comment::kCollection);
          col.update_one(make_document(kvp("_id", *parentOid)),
                         make_document(kvp("$push",
                             make_document(kvp("replies", *childOid)))));
        }
      } catch (const std::exception& e) {
        pulse::log::warn("[posts] parent reply push failed: {}", e.what());
      }
    }

    // populatedComment = Comment.findById(comment._id)
    //   .populate('author', 'username name avatar profile isVerified').lean()
    Json::Value populated;
    {
      auto col = pulse::db::collection(pulse::models::comment::kCollection);
      if (auto o = bj::tryOid(newCommentId)) {
        auto d = col.find_one(make_document(kvp("_id", *o)));
        if (d) populated = pulse::models::comment::sanitizeForOutput(bj::toJson(d->view()));
      }
    }
    populateAuthorSingle(populated);

    // Create notification for post author (if not self-comment).
    const std::string aId = authorHex(post);
    if (aId != userId) {
      try {
        Json::Value n(Json::objectValue);
        n["recipient"] = aId;
        n["sender"]    = userId;
        n["type"]      = "comment";
        n["post"]      = postId;
        n["comment"]   = newCommentId;
        n["message"]   = "commented on your post";
        pulse::models::notification::createNotification(n);
      } catch (const std::exception& e) {
        pulse::log::error("Notification error: {}", e.what());
      }
    }

    Json::Value out(Json::objectValue);
    out["data"] = populated;
    cb(pulse::http::success(out, drogon::k201Created));
  } catch (const std::exception& e) {
    pulse::log::error("Add comment error: {}", e.what());
    cb(fail(drogon::k500InternalServerError, e.what()));
  }
}

// ===========================================================================
//  GET COMMENTS  — GET /api/v1/posts/:postId/comments
// ===========================================================================
namespace {

// Recursively collect every comment id in a tree (top-level + nested replies).
void collectIds(const Json::Value& list, std::vector<std::string>& ids) {
  if (!list.isArray()) return;
  for (const auto& c : list) {
    if (c.isMember("_id") && c["_id"].isString()) ids.push_back(c["_id"].asString());
    if (c.isMember("replies") && c["replies"].isArray() && !c["replies"].empty())
      collectIds(c["replies"], ids);
  }
}

// Add isLikedByMe + likesCount to each comment (incl. replies), drop legacy likes[].
void addLikeInfo(Json::Value& list,
                 const std::map<std::string, long long>& likeCounts,
                 const std::set<std::string>& likedSet) {
  if (!list.isArray()) return;
  for (auto& c : list) {
    std::string id = (c.isMember("_id") && c["_id"].isString()) ? c["_id"].asString() : "";
    auto it = likeCounts.find(id);
    c["likesCount"]  = static_cast<Json::Int64>(it != likeCounts.end() ? it->second : 0);
    c["isLikedByMe"] = likedSet.count(id) > 0;
    c.removeMember("likes");
    if (c.isMember("replies") && c["replies"].isArray() && !c["replies"].empty())
      addLikeInfo(c["replies"], likeCounts, likedSet);
  }
}

// Hydrate a `replies` array of ObjectId refs into full reply documents, with
// each reply's author populated. Mirrors .populate({path:'replies',populate:author})
// and the recursive nested-reply population in the JS.
void populateReplies(Json::Value& comment, int depth) {
  if (depth > 8) return;  // safety bound on pathological nesting
  if (!comment.isObject() || !comment.isMember("replies")) return;
  Json::Value& replies = comment["replies"];
  if (!replies.isArray() || replies.empty()) return;

  // Gather reply ids (refs) that are still raw ObjectId hex strings.
  std::vector<std::string> ids;
  for (const auto& r : replies) {
    if (r.isString()) ids.push_back(r.asString());
    else if (r.isObject() && r.isMember("_id") && r["_id"].isString())
      ids.push_back(r["_id"].asString());
  }
  if (ids.empty()) return;

  Json::Value byId(Json::objectValue);
  try {
    bld::array in;
    for (const auto& hex : ids) if (auto o = bj::tryOid(hex)) in.append(*o);
    auto filter = make_document(kvp("_id", make_document(kvp("$in", in.extract()))));
    auto col = pulse::db::collection(pulse::models::comment::kCollection);
    auto cursor = col.find(filter.view());
    for (const auto& d : cursor) {
      Json::Value rc = pulse::models::comment::sanitizeForOutput(bj::toJson(d));
      if (rc.isMember("_id") && rc["_id"].isString())
        byId[rc["_id"].asString()] = rc;
    }
  } catch (const std::exception& e) {
    pulse::log::warn("[posts] replies populate failed: {}", e.what());
    return;
  }

  Json::Value hydrated(Json::arrayValue);
  for (const auto& hex : ids)
    if (byId.isMember(hex)) hydrated.append(byId[hex]);
  comment["replies"] = hydrated;

  // Populate each reply's author, then recurse into nested replies.
  populateAuthors(comment["replies"]);
  for (auto& r : comment["replies"]) populateReplies(r, depth + 1);
}

} // namespace

void PostController::getComments(const HttpRequestPtr& req,
                                 std::function<void(const HttpResponsePtr&)>&& cb,
                                 std::string postId) {
  try {
    Json::Value user = req->getAttributes()->get<Json::Value>("user");
    const std::string userId =
        user.isMember("userId") ? user["userId"].asString() : "";

    std::string sort = req->getParameter("sort");
    if (sort.empty()) sort = "recent";
    int page  = clampedInt(req->getParameter("page"), 1, 1, 100);
    int limit = clampedInt(req->getParameter("limit"), 20, 1, 50);

    // Comment.find({ post, parentComment:null, isActive:true })
    //   .sort({createdAt:-1}).limit(limit).skip((page-1)*limit)
    //   .populate('author', ...).populate({path:'replies', populate:author}).lean()
    Json::Value comments(Json::arrayValue);
    {
      bld::document filter;
      if (auto o = bj::tryOid(postId)) filter.append(kvp("post", *o));
      filter.append(kvp("parentComment", bsoncxx::types::b_null{}));
      filter.append(kvp("isActive", true));

      mongocxx::options::find opts{};
      opts.sort(make_document(kvp("createdAt", -1)));
      opts.limit(limit);
      opts.skip(static_cast<std::int64_t>((page - 1) * limit));

      auto col = pulse::db::collection(pulse::models::comment::kCollection);
      auto cursor = col.find(filter.view(), opts);
      for (const auto& d : cursor)
        comments.append(pulse::models::comment::sanitizeForOutput(bj::toJson(d)));
    }

    // Populate top-level authors, then replies (recursively) with their authors.
    populateAuthors(comments);
    for (auto& c : comments) populateReplies(c, 0);

    // Collect ids and resolve like counts/status from the atomic Like collection.
    std::vector<std::string> ids;
    collectIds(comments, ids);

    std::map<std::string, long long> likeCounts =
        pulse::models::like::getBatchLikeCounts(
            pulse::models::like::kTargetTypeComment, ids);
    std::set<std::string> likedSet;
    if (!userId.empty())
      likedSet = pulse::models::like::getLikedIds(
          userId, pulse::models::like::kTargetTypeComment, ids);

    addLikeInfo(comments, likeCounts, likedSet);

    // Sort by likes count if sort=top.
    if (sort == "top") {
      std::vector<Json::Value> v;
      for (const auto& c : comments) v.push_back(c);
      std::stable_sort(v.begin(), v.end(), [](const Json::Value& a, const Json::Value& b) {
        long long la = a.get("likesCount", 0).asInt64();
        long long lb = b.get("likesCount", 0).asInt64();
        return lb < la;  // descending
      });
      Json::Value sorted(Json::arrayValue);
      for (auto& c : v) sorted.append(c);
      comments = sorted;
    }

    Json::Value out(Json::objectValue);
    out["data"] = comments;
    Json::Value pagination(Json::objectValue);
    pagination["page"]  = page;
    pagination["limit"] = limit;
    out["pagination"] = pagination;
    cb(pulse::http::success(out));
  } catch (const std::exception& e) {
    pulse::log::error("Get comments error: {}", e.what());
    cb(fail(drogon::k500InternalServerError, e.what()));
  }
}

// ===========================================================================
//  TOGGLE COMMENT LIKE — POST /api/v1/posts/:postId/comments/:commentId/like
// ===========================================================================
void PostController::toggleCommentLike(const HttpRequestPtr& req,
                                       std::function<void(const HttpResponsePtr&)>&& cb,
                                       std::string postId, std::string commentId) {
  try {
    Json::Value user = req->getAttributes()->get<Json::Value>("user");
    const std::string userId = user["userId"].asString();

    // const comment = await Comment.findById(commentId).select('_id').lean();
    bool exists = false;
    if (auto o = bj::tryOid(commentId)) {
      try {
        auto col = pulse::db::collection(pulse::models::comment::kCollection);
        auto proj = make_document(kvp("_id", 1));
        mongocxx::options::find opts{};
        opts.projection(proj.view());
        auto d = col.find_one(make_document(kvp("_id", *o)), opts);
        exists = static_cast<bool>(d);
      } catch (const std::exception& e) {
        pulse::log::warn("[posts] comment lookup failed: {}", e.what());
      }
    }
    if (!exists) return cb(fail(drogon::k404NotFound, "Comment not found"));

    auto toggle = pulse::models::like::toggleLike(
        userId, pulse::models::like::kTargetTypeComment, commentId);

    Json::Value data(Json::objectValue);
    data["isLiked"]    = toggle.liked;
    data["likesCount"] = static_cast<Json::Int64>(toggle.likeCount);
    cb(pulse::http::ok(data));
  } catch (const std::exception& e) {
    // JS catch returns a generic 'Server error' message for this handler.
    pulse::log::error("Toggle comment like error: {}", e.what());
    cb(fail(drogon::k500InternalServerError, "Server error"));
  }
}

// ===========================================================================
//  DELETE POST  — DELETE /api/v1/posts/:postId
// ===========================================================================
void PostController::deletePost(const HttpRequestPtr& req,
                                std::function<void(const HttpResponsePtr&)>&& cb,
                                std::string postId) {
  try {
    Json::Value user = req->getAttributes()->get<Json::Value>("user");
    const std::string userId = user["userId"].asString();

    auto found = findPostById(postId);
    if (!found) return cb(fail(drogon::k404NotFound, "Post not found"));
    Json::Value post = *found;

    const std::string aId = authorHex(post);
    if (aId != userId)
      return cb(fail(drogon::k403Forbidden, "You can only delete your own posts"));

    // post.isActive = false; await post.save();
    if (auto o = bj::tryOid(postId)) {
      auto col = pulse::db::collection(pulse::models::post::kCollection);
      col.update_one(make_document(kvp("_id", *o)),
                     make_document(kvp("$set",
                         make_document(kvp("isActive", false),
                                       kvp("updatedAt", dateFromMillis(nowMillis()))))));
    }

    // await User.findByIdAndUpdate(userId, { $inc: { 'stats.posts': -1 } })
    if (auto o = bj::tryOid(userId)) {
      try {
        auto col = pulse::db::collection(pulse::models::user::kCollection);
        col.update_one(make_document(kvp("_id", *o)),
                       make_document(kvp("$inc",
                           make_document(kvp("stats.posts", -1)))));
      } catch (const std::exception& e) {
        pulse::log::warn("[posts] deletePost user stats dec failed: {}", e.what());
      }
    }

    Json::Value out(Json::objectValue);
    out["message"] = "Post deleted successfully";
    cb(pulse::http::success(out));
  } catch (const std::exception& e) {
    pulse::log::error("Delete post error: {}", e.what());
    cb(fail(drogon::k500InternalServerError, "Failed to delete post"));
  }
}

// ===========================================================================
//  UPDATE POST  — PATCH /api/v1/posts/:postId
// ===========================================================================
void PostController::updatePost(const HttpRequestPtr& req,
                                std::function<void(const HttpResponsePtr&)>&& cb,
                                std::string postId) {
  try {
    Json::Value user = req->getAttributes()->get<Json::Value>("user");
    const std::string userId = user["userId"].asString();

    Json::Value empty(Json::objectValue);
    const Json::Value& b = bodyOrEmpty(req->getJsonObject(), empty);

    auto found = findPostById(postId);
    if (!found) return cb(fail(drogon::k404NotFound, "Post not found"));
    Json::Value post = *found;

    // if (post.author.toString() !== req.user.userId) -> 403 'Unauthorized'
    // (raw, UNpopulated author ref — matches the JS which used post.author here.)
    if (authorHex(post) != userId)
      return cb(fail(drogon::k403Forbidden, "Unauthorized"));

    // const { text, visibility, allowComments } = req.body;
    // if (text !== undefined) post.content.text = text;
    // if (visibility) post.visibility = visibility;
    // if (allowComments !== undefined) post.allowComments = allowComments;
    if (b.isMember("text")) {
      if (!post.isMember("content") || !post["content"].isObject())
        post["content"] = Json::Value(Json::objectValue);
      post["content"]["text"] = b["text"];
    }
    if (b.isMember("visibility") && b["visibility"].isString() &&
        !b["visibility"].asString().empty())
      post["visibility"] = b["visibility"];
    if (b.isMember("allowComments"))
      post["allowComments"] = b["allowComments"];

    post["isEdited"] = true;
    post["editedAt"] = bj::nowIso8601();

    // await post.save();  — persist the modified fields.
    if (auto o = bj::tryOid(postId)) {
      bld::document set;
      if (post.isMember("content") && post["content"].isObject() &&
          post["content"].isMember("text")) {
        set.append(kvp("content.text",
            post["content"]["text"].isString() ? post["content"]["text"].asString()
                                               : std::string()));
      }
      if (post.isMember("visibility") && post["visibility"].isString())
        set.append(kvp("visibility", post["visibility"].asString()));
      if (post.isMember("allowComments"))
        set.append(kvp("allowComments", post["allowComments"].asBool()));
      set.append(kvp("isEdited", true));
      set.append(kvp("editedAt", dateFromMillis(nowMillis())));
      set.append(kvp("updatedAt", dateFromMillis(nowMillis())));

      auto col = pulse::db::collection(pulse::models::post::kCollection);
      col.update_one(make_document(kvp("_id", *o)),
                     make_document(kvp("$set", set.extract())));
    }

    Json::Value masked = maskAnonymousPost(pulse::models::post::sanitizeForOutput(post));

    Json::Value out(Json::objectValue);
    out["data"] = masked;
    cb(pulse::http::success(out));
  } catch (const std::exception& e) {
    pulse::log::error("Update post error: {}", e.what());
    cb(fail(drogon::k500InternalServerError, e.what()));
  }
}

// ===========================================================================
//  SEARCH POSTS  — GET /api/v1/posts/search
// ===========================================================================
void PostController::searchPosts(const HttpRequestPtr& req,
                                 std::function<void(const HttpResponsePtr&)>&& cb) {
  try {
    // const { q, page = 1, limit = 20 } = req.query;
    std::string q = req->getParameter("q");

    // if (!q || q.trim().length === 0) return res.json({ success:true, data:[] });
    if (q.empty() || trim(q).empty()) {
      Json::Value out(Json::objectValue);
      out["data"] = Json::Value(Json::arrayValue);
      return cb(pulse::http::success(out));
    }

    // const searchQuery = q.trim().slice(0, 100);
    std::string searchQuery = trim(q);
    if (searchQuery.size() > 100) searchQuery = searchQuery.substr(0, 100);

    int pageNum  = clampedInt(req->getParameter("page"), 1, 1, 20);
    int limitNum = clampedInt(req->getParameter("limit"), 20, 1, 50);

    // Post.find({ $text:{$search}, isActive:true, isAnonymous:false,
    //             visibility:'public' }, { score:{$meta:'textScore'} })
    //   .sort({ score:{$meta:'textScore'} }).limit(limitNum).skip((pageNum-1)*limitNum)
    //   .populate('author', ...).lean()
    Json::Value posts(Json::arrayValue);
    {
      auto filter = make_document(
          kvp("$text", make_document(kvp("$search", searchQuery))),
          kvp("isActive", true),
          kvp("isAnonymous", false),
          kvp("visibility", "public"));

      mongocxx::options::find opts{};
      opts.projection(make_document(
          kvp("score", make_document(kvp("$meta", "textScore")))).view());
      opts.sort(make_document(
          kvp("score", make_document(kvp("$meta", "textScore")))).view());
      opts.limit(limitNum);
      opts.skip(static_cast<std::int64_t>((pageNum - 1) * limitNum));

      auto col = pulse::db::collection(pulse::models::post::kCollection);
      auto cursor = col.find(filter.view(), opts);
      for (const auto& d : cursor)
        posts.append(pulse::models::post::sanitizeForOutput(bj::toJson(d)));
    }
    populateAuthors(posts);

    Json::Value out(Json::objectValue);
    out["data"] = posts;
    Json::Value pagination(Json::objectValue);
    pagination["page"]    = pageNum;
    pagination["limit"]   = limitNum;
    pagination["hasMore"] = (static_cast<int>(posts.size()) == limitNum);
    out["pagination"] = pagination;
    cb(pulse::http::success(out));
  } catch (const std::exception& e) {
    pulse::log::error("Search posts error: {}", e.what());
    cb(fail(drogon::k500InternalServerError, e.what()));
  }
}

// ===========================================================================
//  GET TRENDING HASHTAGS  — GET /api/v1/posts/trending
// ===========================================================================
namespace {

// computeTrendingHashtags() — the $unwind + $group aggregation over the last 24h
// of public active posts, top 10 by count. Mirrors the JS computeTrendingHashtags.
Json::Value computeTrendingHashtags() {
  Json::Value out(Json::arrayValue);
  try {
    long long timeAgoMs = nowMillis() - 24LL * 60 * 60 * 1000;

    mongocxx::pipeline pipe;
    pipe.match(make_document(
        kvp("isActive", true),
        kvp("visibility", "public"),
        kvp("createdAt", make_document(kvp("$gte", dateFromMillis(timeAgoMs))))));
    pipe.unwind("$content.hashtags");
    pipe.group(make_document(
        kvp("_id", "$content.hashtags"),
        kvp("count", make_document(kvp("$sum", 1)))));
    pipe.sort(make_document(kvp("count", -1)));
    pipe.limit(10);

    auto col = pulse::db::collection(pulse::models::post::kCollection);
    auto cursor = col.aggregate(pipe);
    for (const auto& d : cursor) {
      Json::Value row = bj::toJson(d);
      Json::Value entry(Json::objectValue);
      entry["tag"]   = row.isMember("_id") ? row["_id"] : Json::Value(Json::nullValue);
      entry["count"] = row.isMember("count") ? row["count"] : Json::Value(0);
      out.append(entry);
    }
  } catch (const std::exception& e) {
    pulse::log::error("[posts] computeTrendingHashtags failed: {}", e.what());
    throw;  // propagate so the handler's catch returns the 500 the JS produced
  }
  return out;
}

// Compact JSON serialization for cache values.
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
  reader->parse(s.data(), s.data() + s.size(), &v, &errs);
  return v;
}

} // namespace

// Public entry point for the background job (src/jobs/scheduler.cc). Runs the
// trending-hashtags aggregation and returns the compact JSON string that the
// scheduler caches under "trending:hashtags". Has external linkage, unlike the
// anonymous-namespace computeTrendingHashtags() above.
std::string computeTrendingHashtagsJson() {
  return dumpJson(computeTrendingHashtags());
}

void PostController::getTrendingHashtags(const HttpRequestPtr& req,
                                         std::function<void(const HttpResponsePtr&)>&& cb) {
  try {
    // TTL = parseInt(process.env.TRENDING_HASHTAG_TTL_SEC) || 600
    int ttl = 600;
    if (const char* env = std::getenv("TRENDING_HASHTAG_TTL_SEC")) {
      char* end = nullptr;
      long v = std::strtol(env, &end, 10);
      if (end != env && v != 0) ttl = static_cast<int>(v);
    }

    // cacheService.getOrSet('trending:hashtags', () => computeTrendingHashtags(), ttl)
    std::string cached = pulse::cache().getOrSet(
        "trending:hashtags",
        []() { return dumpJson(computeTrendingHashtags()); },
        ttl);

    Json::Value data = parseJson(cached);
    if (data.isNull()) data = Json::Value(Json::arrayValue);

    Json::Value out(Json::objectValue);
    out["data"] = data;
    cb(pulse::http::success(out));
  } catch (const std::exception& e) {
    pulse::log::error("Get trending error: {}", e.what());
    cb(fail(drogon::k500InternalServerError, "Failed to load trending"));
  }
}

} // namespace pulse::controllers
