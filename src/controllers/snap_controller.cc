// snap_controller.cc — implementation of SnapController (see snap_controller.hpp).
//
// 1:1 port of src/controllers/snapController.js. Responses, status codes, and
// JSON shapes mirror the Express handlers exactly. Note the snap endpoints use
// the bare { success, message } body (NOT the { success, error, code } shape),
// so those responses are built directly with pulse::http::json rather than the
// error()/badRequest() helpers (which inject a "code").
#include "pulse/controllers/snap_controller.hpp"

#include "pulse/http_response.hpp"
#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"
#include "pulse/config.hpp"
#include "pulse/models/snap.hpp"
#include "pulse/models/follow.hpp"
#include "pulse/services/media_service.hpp"

#include <drogon/MultiPart.h>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/oid.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/cursor.hpp>
#include <mongocxx/pipeline.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/find_one_and_update.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace pulse::controllers {

namespace bbuild = bsoncxx::builder::basic;
using bbuild::kvp;
using bbuild::make_document;
using bbuild::make_array;

namespace {

// Max viewers kept inline on a story doc (mirrors MAX_VIEWERS in the JS).
constexpr int kMaxViewers = 500;

// --- file-local validation helpers (ports of the inline JS checks) ---

// mongoose.isValidObjectId(id) — true iff a parseable 24-hex ObjectId.
bool isValidObjectId(const std::string& id) {
  return pulse::bsonjson::isValidOid(id);
}

// parseInt(x) || fallback then clamp(min,max) — mirrors
// Math.min(Math.max(parseInt(req.body.durationMs) || 5000, 1000), 15000).
long long parseIntOr(const std::string& s, long long fallback) {
  if (s.empty()) return fallback;
  try {
    size_t pos = 0;
    long long v = std::stoll(s, &pos);
    // JS parseInt: leading numeric prefix is enough; if nothing parsed -> NaN -> fallback.
    if (pos == 0) return fallback;
    return v;
  } catch (...) {
    return fallback;
  }
}

// (req.body.caption || '').slice(0, 280)
std::string sliceStr(const std::string& s, size_t n) {
  return s.size() > n ? s.substr(0, n) : s;
}

// { success:false, message } body at a given status — the snap error shape.
drogon::HttpResponsePtr failMsg(drogon::HttpStatusCode status, const std::string& message) {
  Json::Value body(Json::objectValue);
  body["success"] = false;
  body["message"] = message;
  return pulse::http::json(status, std::move(body));
}

// Resolve the authenticated user's id (req.user.userId).
std::string currentUserId(const drogon::HttpRequestPtr& req) {
  auto user = req->getAttributes()->get<Json::Value>("user");
  return user["userId"].asString();
}

bool canAccessSnap(const Json::Value& snap, const std::string& userId) {
  const std::string authorId = snap.get("user", "").asString();
  if (authorId == userId) return true;

  const std::string audience = snap.get("audience", "").asString();
  if (audience == "direct") {
    if (!snap.isMember("recipients") || !snap["recipients"].isArray()) return false;
    for (const auto& recipient : snap["recipients"]) {
      if (recipient.isString() && recipient.asString() == userId) return true;
    }
    return false;
  }

  if (audience != "story") return false;
  // Stories are exposed by the story rail only to the author's followers.
  return !authorId.empty() &&
      pulse::models::follow::isFollowing(userId, authorId);
}

} // namespace

// ---------------------------------------------------------------------------
// POST /api/v1/snaps — createSnap
// ---------------------------------------------------------------------------
void SnapController::createSnap(const HttpRequestPtr& req,
                                std::function<void(const HttpResponsePtr&)>&& callback) {
  const std::string userId = currentUserId(req);

  // Parse multipart: file (single('file')) + text fields (req.body.*).
  drogon::MultiPartParser parser;
  if (parser.parse(req) != 0) {
    // No file part -> mirrors `if (!req.file)`.
    return callback(failMsg(drogon::k400BadRequest, "No media provided"));
  }
  const auto& files = parser.getFiles();
  if (files.empty()) {
    return callback(failMsg(drogon::k400BadRequest, "No media provided"));
  }
  const auto& file = files.front();

  // req.body fields (multipart text params).
  const auto& params = parser.getParameters();
  auto field = [&](const char* k) -> std::string {
    auto it = params.find(k);
    return it == params.end() ? std::string() : it->second;
  };

  // audience = req.body.audience === 'direct' ? 'direct' : 'story'
  const std::string audience = (field("audience") == "direct") ? "direct" : "story";

  // recipients: parse JSON array, keep only valid ObjectIds.
  std::vector<std::string> recipients;
  if (audience == "direct") {
    const std::string raw = field("recipients");
    Json::Value arr;
    bool parsed = false;
    if (!raw.empty()) {
      Json::CharReaderBuilder rb;
      std::string errs;
      std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
      parsed = reader->parse(raw.data(), raw.data() + raw.size(), &arr, &errs);
    }
    if (parsed && arr.isArray()) {
      for (const auto& v : arr) {
        if (v.isString() && isValidObjectId(v.asString())) {
          recipients.push_back(v.asString());
        }
      }
    }
    // catch {} -> recipients = [] (already empty on parse failure)
    if (recipients.empty()) {
      return callback(failMsg(drogon::k400BadRequest, "Direct snaps need recipients"));
    }
  }

  // isVideo = (req.file.mimetype || '').startsWith('video')
  // Drogon's HttpFile exposes the declared content-type via getContentType()
  // (the multipart part's Content-Type header). A "video/*" MIME maps to the
  // CT_VIDEO_* / CT_APPLICATION_* enum range; we additionally fall back to the
  // file extension (mp4/mov/webm) for parity with the multer mimetype check.
  const std::string ext = [&] {
    std::string e{file.getFileExtension()};
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return e;
  }();
  const bool isVideo =
      (file.getContentType() == drogon::CT_VIDEO_MP4) ||
      ext == "mp4" || ext == "mov" || ext == "webm";

  // Upload to Cloudinary (snap folder, image|video transform). The JS upload
  // callback's error -> 500 { message:'Upload failed' }; the outer try/catch ->
  // 500 { message:'Server error' }.
  pulse::UploadResult result;
  try {
    result = pulse::media().uploadSnap(std::string(file.fileContent()),
                                       file.getFileName(), isVideo);
  } catch (const std::exception& e) {
    pulse::log::error("Snap upload error: {}", e.what());
    return callback(failMsg(drogon::k500InternalServerError, "Upload failed"));
  }
  if (result.secureUrl.empty()) {
    pulse::log::error("Snap upload error: empty result");
    return callback(failMsg(drogon::k500InternalServerError, "Upload failed"));
  }

  // Build the Snap document (applyDefaults fills schema defaults/timestamps).
  Json::Value doc(Json::objectValue);
  doc["user"] = userId;
  doc["audience"] = audience;
  {
    Json::Value rec(Json::arrayValue);
    for (const auto& r : recipients) rec.append(r);
    doc["recipients"] = rec;
  }
  doc["mediaType"] = isVideo ? "video" : "image";
  doc["mediaUrl"] = result.secureUrl;
  doc["publicId"] = result.publicId;
  doc["thumbnailUrl"] = isVideo
      ? pulse::media().videoThumbnailUrl(result.secureUrl, true)
      : result.secureUrl;
  doc["durationMs"] = static_cast<Json::Int64>(
      std::min<long long>(std::max<long long>(parseIntOr(field("durationMs"), 5000), 1000), 15000));
  doc["caption"] = sliceStr(field("caption"), 280);
  doc["expiresAt"] = pulse::models::snap::defaultExpiry();

  doc = pulse::models::snap::applyDefaults(std::move(doc));

  try {
    auto col = pulse::db::collection(pulse::models::snap::kCollection);

    // Convert the JSON doc to BSON, coercing ObjectId-typed fields.
    bbuild::document b;
    bsoncxx::oid newId;            // _id like Mongoose generates
    b.append(kvp("_id", newId));
    if (auto o = pulse::bsonjson::tryOid(userId)) b.append(kvp("user", *o));
    b.append(kvp("audience", doc["audience"].asString()));
    {
      bbuild::array recArr;
      for (const auto& r : doc["recipients"]) {
        if (auto o = pulse::bsonjson::tryOid(r.asString())) recArr.append(*o);
      }
      b.append(kvp("recipients", recArr));
    }
    b.append(kvp("mediaType", doc["mediaType"].asString()));
    b.append(kvp("mediaUrl", doc["mediaUrl"].asString()));
    b.append(kvp("publicId", doc["publicId"].asString()));
    b.append(kvp("thumbnailUrl", doc["thumbnailUrl"].asString()));
    b.append(kvp("durationMs", static_cast<int64_t>(doc["durationMs"].asInt64())));
    b.append(kvp("caption", doc["caption"].asString()));
    b.append(kvp("viewers", bbuild::array{}));
    b.append(kvp("viewCount", static_cast<int64_t>(0)));
    b.append(kvp("reactions", bbuild::document{}));
    // expiresAt (now + DEFAULT_TTL_MS) / createdAt / updatedAt as BSON dates.
    b.append(kvp("expiresAt", bsoncxx::types::b_date{
        std::chrono::system_clock::now() +
        std::chrono::milliseconds(pulse::models::snap::kDefaultTtlMs)}));
    auto nowB = bsoncxx::types::b_date{std::chrono::system_clock::now()};
    b.append(kvp("createdAt", nowB));
    b.append(kvp("updatedAt", nowB));

    col.insert_one(b.view());

    // Return the created snap doc (matches res.status(201).json({ data: snap })).
    auto saved = col.find_one(make_document(kvp("_id", newId)));
    Json::Value snapJson = saved
        ? pulse::models::snap::sanitizeForOutput(pulse::bsonjson::toJson(saved->view()))
        : doc;

    Json::Value body(Json::objectValue);
    body["success"] = true;
    body["data"] = snapJson;
    return callback(pulse::http::json(drogon::k201Created, std::move(body)));
  } catch (const std::exception& dbErr) {
    pulse::log::error("Snap save error: {}", dbErr.what());
    return callback(failMsg(drogon::k500InternalServerError, "Could not save snap"));
  }
}

// ---------------------------------------------------------------------------
// GET /api/v1/snaps/rail — getStoryRail
// ---------------------------------------------------------------------------
void SnapController::getStoryRail(const HttpRequestPtr& req,
                                  std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    const std::string userId = currentUserId(req);
    auto followingIds = pulse::models::follow::getFollowingIds(userId);
    Json::Value rings = pulse::models::snap::getStoryRail(userId, followingIds);

    Json::Value body(Json::objectValue);
    body["success"] = true;
    body["data"] = rings;
    return callback(pulse::http::json(drogon::k200OK, std::move(body)));
  } catch (const std::exception& err) {
    pulse::log::error("Story rail error: {}", err.what());
    return callback(failMsg(drogon::k500InternalServerError, "Failed to load stories"));
  }
}

// ---------------------------------------------------------------------------
// GET /api/v1/snaps/direct — getDirectInbox
// ---------------------------------------------------------------------------
void SnapController::getDirectInbox(const HttpRequestPtr& req,
                                    std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    Json::Value snaps = pulse::models::snap::getDirectInbox(currentUserId(req));

    Json::Value body(Json::objectValue);
    body["success"] = true;
    body["data"] = snaps;
    return callback(pulse::http::json(drogon::k200OK, std::move(body)));
  } catch (const std::exception& err) {
    pulse::log::error("Direct inbox error: {}", err.what());
    return callback(failMsg(drogon::k500InternalServerError, "Failed to load snaps"));
  }
}

// ---------------------------------------------------------------------------
// POST /api/v1/snaps/:snapId/view — viewSnap
// ---------------------------------------------------------------------------
void SnapController::viewSnap(const HttpRequestPtr& req,
                              std::function<void(const HttpResponsePtr&)>&& callback,
                              std::string snapId) {
  try {
    const std::string userId = currentUserId(req);
    if (!isValidObjectId(snapId)) {
      return callback(failMsg(drogon::k400BadRequest, "Invalid snap"));
    }

    auto col = pulse::db::collection(pulse::models::snap::kCollection);
    auto oid = pulse::bsonjson::oid(snapId);

    // findById(snapId).select('audience user recipients viewers viewCount')
    mongocxx::options::find opts;
    opts.projection(make_document(
        kvp("audience", 1), kvp("user", 1), kvp("recipients", 1),
        kvp("viewers", 1), kvp("viewCount", 1)));
    auto found = col.find_one(make_document(kvp("_id", oid)), opts);
    if (!found) {
      return callback(failMsg(drogon::k404NotFound, "Snap not found"));
    }
    Json::Value snap = pulse::bsonjson::toJson(found->view());

    const std::string snapUser = snap["user"].asString();

    // Authorization: direct snaps only viewable by sender/recipients.
    if (snap.get("audience", "").asString() == "direct") {
      bool allowed = (snapUser == userId);
      if (!allowed && snap.isMember("recipients") && snap["recipients"].isArray()) {
        for (const auto& r : snap["recipients"]) {
          if (r.asString() == userId) { allowed = true; break; }
        }
      }
      if (!allowed) {
        return callback(failMsg(drogon::k403Forbidden, "Not authorized"));
      }
    }

    // already = (snap.viewers || []).some(v => String(v.user) === String(userId))
    bool already = false;
    if (snap.isMember("viewers") && snap["viewers"].isArray()) {
      for (const auto& v : snap["viewers"]) {
        std::string vu;
        if (v.isObject() && v.isMember("user")) vu = v["user"].asString();
        else if (v.isString()) vu = v.asString();
        if (vu == userId) { already = true; break; }
      }
    }

    if (!already && snapUser != userId) {
      // $push viewers { $each:[{user, viewedAt:now}], $slice:-MAX_VIEWERS } + $inc viewCount
      auto viewerOid = pulse::bsonjson::oid(userId);
      auto update = make_document(
          kvp("$push", make_document(kvp("viewers", make_document(
              kvp("$each", make_array(make_document(
                  kvp("user", viewerOid),
                  kvp("viewedAt", bsoncxx::types::b_date{std::chrono::system_clock::now()})))),
              kvp("$slice", -kMaxViewers))))),
          kvp("$inc", make_document(kvp("viewCount", 1))));
      col.update_one(make_document(kvp("_id", oid)), update.view());
    }

    Json::Value body(Json::objectValue);
    body["success"] = true;
    return callback(pulse::http::json(drogon::k200OK, std::move(body)));
  } catch (const std::exception& err) {
    pulse::log::error("View snap error: {}", err.what());
    return callback(failMsg(drogon::k500InternalServerError, "Server error"));
  }
}

// ---------------------------------------------------------------------------
// POST /api/v1/snaps/:snapId/react — reactSnap
// ---------------------------------------------------------------------------
void SnapController::reactSnap(const HttpRequestPtr& req,
                               std::function<void(const HttpResponsePtr&)>&& callback,
                               std::string snapId) {
  try {
    const std::string userId = currentUserId(req);

    // reaction = String(req.body.reaction || '').slice(0, 8)
    std::string reaction;
    auto bodyJson = req->getJsonObject();
    if (bodyJson && bodyJson->isMember("reaction") && (*bodyJson)["reaction"].isString()) {
      reaction = sliceStr((*bodyJson)["reaction"].asString(), 8);
    }

    if (!isValidObjectId(snapId)) {
      return callback(failMsg(drogon::k400BadRequest, "Invalid snap"));
    }

    auto col = pulse::db::collection(pulse::models::snap::kCollection);
    auto oid = pulse::bsonjson::oid(snapId);
    const std::string field = "reactions." + userId;

    mongocxx::options::find accessOpts;
    accessOpts.projection(make_document(kvp("audience", 1), kvp("user", 1),
                                        kvp("recipients", 1)));
    auto activeFilter = make_document(
        kvp("_id", oid),
        kvp("expiresAt", make_document(kvp(
            "$gt", bsoncxx::types::b_date{std::chrono::system_clock::now()}))));
    auto accessDoc = col.find_one(activeFilter.view(), accessOpts);
    if (!accessDoc)
      return callback(failMsg(drogon::k404NotFound, "Snap not found"));
    if (!canAccessSnap(pulse::bsonjson::toJson(accessDoc->view()), userId))
      return callback(failMsg(drogon::k403Forbidden, "Not authorized"));

    // reaction ? $set:{[`reactions.${userId}`]:reaction} : $unset:{[`reactions.${userId}`]:''}
    bsoncxx::document::value update = reaction.empty()
        ? make_document(kvp("$unset", make_document(kvp(field, ""))))
        : make_document(kvp("$set", make_document(kvp(field, reaction))));

    // findByIdAndUpdate(snapId, update, { new:true }).select('reactions')
    mongocxx::options::find_one_and_update opts;
    opts.return_document(mongocxx::options::return_document::k_after);
    opts.projection(make_document(kvp("reactions", 1)));
    auto updateFilter = make_document(
        kvp("_id", oid),
        kvp("expiresAt", make_document(kvp(
            "$gt", bsoncxx::types::b_date{std::chrono::system_clock::now()}))));
    auto snap = col.find_one_and_update(updateFilter.view(), update.view(), opts);
    if (!snap) {
      return callback(failMsg(drogon::k404NotFound, "Snap not found"));
    }

    Json::Value data(Json::objectValue);
    data["reaction"] = reaction;
    Json::Value body(Json::objectValue);
    body["success"] = true;
    body["data"] = data;
    return callback(pulse::http::json(drogon::k200OK, std::move(body)));
  } catch (const std::exception& err) {
    pulse::log::error("React snap error: {}", err.what());
    return callback(failMsg(drogon::k500InternalServerError, "Server error"));
  }
}

// ---------------------------------------------------------------------------
// GET /api/v1/snaps/:snapId/viewers — getViewers
// ---------------------------------------------------------------------------
void SnapController::getViewers(const HttpRequestPtr& req,
                                std::function<void(const HttpResponsePtr&)>&& callback,
                                std::string snapId) {
  try {
    if (!isValidObjectId(snapId)) {
      // Matches Mongoose CastError -> caught -> 500 'Server error' would occur,
      // but the JS never validates here; an invalid id throws on findById and is
      // caught by the try/catch. Preserve that behavior: fall through to a 404 is
      // wrong — replicate by treating an unparseable id as not found via query.
      return callback(failMsg(drogon::k500InternalServerError, "Server error"));
    }

    auto col = pulse::db::collection(pulse::models::snap::kCollection);
    auto oid = pulse::bsonjson::oid(snapId);

    // findById(snapId).select('user viewCount viewers')
    //   .populate('viewers.user', 'username name avatar profile.avatar isVerified')
    mongocxx::pipeline p;
    p.match(make_document(kvp("_id", oid)));
    p.project(make_document(kvp("user", 1), kvp("viewCount", 1), kvp("viewers", 1)));
    // $lookup the viewer user docs, then re-map viewers[].user to the populated doc.
    p.lookup(make_document(
        kvp("from", "users"),
        kvp("localField", "viewers.user"),
        kvp("foreignField", "_id"),
        kvp("pipeline", make_array(make_document(kvp("$project", make_document(
            kvp("username", 1), kvp("name", 1), kvp("avatar", 1),
            kvp("profile.avatar", 1), kvp("isVerified", 1)))))),
        kvp("as", "viewerUsers")));

    auto cursor = col.aggregate(p);
    auto it = cursor.begin();
    if (it == cursor.end()) {
      return callback(failMsg(drogon::k404NotFound, "Snap not found"));
    }
    Json::Value snap = pulse::bsonjson::toJson(*it);

    // Authorization: only the author may see the viewers list.
    if (snap["user"].asString() != currentUserId(req)) {
      return callback(failMsg(drogon::k403Forbidden, "Not authorized"));
    }

    // Build a lookup of populated user docs by _id.
    std::map<std::string, Json::Value> usersById;
    if (snap.isMember("viewerUsers") && snap["viewerUsers"].isArray()) {
      for (const auto& u : snap["viewerUsers"]) {
        if (u.isObject() && u.isMember("_id")) usersById[u["_id"].asString()] = u;
      }
    }

    // viewers: snap.viewers.map(v => v.user).filter(Boolean) — populated user docs.
    Json::Value viewers(Json::arrayValue);
    if (snap.isMember("viewers") && snap["viewers"].isArray()) {
      for (const auto& v : snap["viewers"]) {
        std::string uid;
        if (v.isObject() && v.isMember("user")) uid = v["user"].asString();
        else if (v.isString()) uid = v.asString();
        auto found = usersById.find(uid);
        if (found != usersById.end()) viewers.append(found->second);
        // filter(Boolean): unmatched (deleted user) refs are dropped.
      }
    }

    Json::Value data(Json::objectValue);
    data["viewCount"] = snap.get("viewCount", 0);
    data["viewers"] = viewers;
    Json::Value body(Json::objectValue);
    body["success"] = true;
    body["data"] = data;
    return callback(pulse::http::json(drogon::k200OK, std::move(body)));
  } catch (const std::exception& err) {
    pulse::log::error("Get viewers error: {}", err.what());
    return callback(failMsg(drogon::k500InternalServerError, "Server error"));
  }
}

// ---------------------------------------------------------------------------
// DELETE /api/v1/snaps/:snapId — deleteSnap
// ---------------------------------------------------------------------------
void SnapController::deleteSnap(const HttpRequestPtr& req,
                                std::function<void(const HttpResponsePtr&)>&& callback,
                                std::string snapId) {
  try {
    const std::string userId = currentUserId(req);
    if (!isValidObjectId(snapId)) {
      // findOne with an invalid id -> Mongoose CastError -> caught -> 500.
      return callback(failMsg(drogon::k500InternalServerError, "Server error"));
    }

    auto col = pulse::db::collection(pulse::models::snap::kCollection);
    auto oid = pulse::bsonjson::oid(snapId);

    // findOne({ _id: snapId, user: req.user.userId })
    bbuild::document filter;
    filter.append(kvp("_id", oid));
    if (auto o = pulse::bsonjson::tryOid(userId)) filter.append(kvp("user", *o));
    else filter.append(kvp("user", userId));

    auto found = col.find_one(filter.view());
    if (!found) {
      return callback(failMsg(drogon::k404NotFound, "Snap not found"));
    }
    Json::Value snap = pulse::bsonjson::toJson(found->view());

    // Fire-and-forget Cloudinary destroy (errors swallowed, .catch(() => {})).
    if (snap.isMember("publicId") && snap["publicId"].isString() &&
        !snap["publicId"].asString().empty()) {
      const std::string resourceType =
          (snap.get("mediaType", "").asString() == "video") ? "video" : "image";
      const std::string publicId = snap["publicId"].asString();
      try {
        pulse::media().destroy(publicId, resourceType);
      } catch (...) {
        // swallow — parity with .catch(() => {})
      }
    }

    // snap.deleteOne()
    col.delete_one(make_document(kvp("_id", oid)));

    Json::Value body(Json::objectValue);
    body["success"] = true;
    body["message"] = "Snap deleted";
    return callback(pulse::http::json(drogon::k200OK, std::move(body)));
  } catch (const std::exception& err) {
    pulse::log::error("Delete snap error: {}", err.what());
    return callback(failMsg(drogon::k500InternalServerError, "Server error"));
  }
}

} // namespace pulse::controllers
