// bookmark_controller.cc — implementation of the Bookmark HttpController.
//
// 1:1 port of src/controllers/bookmarkController.js. Every response shape,
// status code, message string, and Mongo query is preserved verbatim. The
// handlers drive the already-ported models (Bookmark / Post / Reel / User)
// rather than re-implementing their logic.
//
// Auth context: the AuthFilter (verifyAccessToken) stores
// { userId, username, email, isVerified } on the request attributes under
// "user"; handlers read it back with req->getAttributes()->get<Json::Value>.
#include "pulse/controllers/bookmark_controller.hpp"

#include "pulse/http_response.hpp"
#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"

#include "pulse/models/bookmark.hpp"
#include "pulse/models/post.hpp"
#include "pulse/models/reel.hpp"
#include "pulse/models/user.hpp"

#include <drogon/HttpRequest.h>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/oid.hpp>

#include <mongocxx/collection.hpp>
#include <mongocxx/cursor.hpp>
#include <mongocxx/options/find.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pulse::controllers {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;
using bld::make_array;
namespace bj = pulse::bsonjson;

namespace {

long long nowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Read the authenticated user JSON ({ userId, username, email, isVerified })
// stored by AuthFilter. Returns null when absent.
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

// Append a ref field (ObjectId) from a hex string to a builder: a 24-hex string
// coerces to bsoncxx::oid (Mongoose ref/_id casting), any other string is kept
// verbatim. Mirrors Mongoose CastError-free path for valid ids.
void appendRef(bld::document& doc, const std::string& key, const std::string& hex) {
  if (auto id = bj::tryOid(hex)) {
    doc.append(kvp(key, *id));
    return;
  }
  doc.append(kvp(key, hex));
}

// JS Number.parseInt(x, 10): parse a leading optional-sign integer prefix,
// ignoring leading whitespace; returns std::nullopt for NaN (no leading digits).
std::optional<long long> parseIntJs(const std::string& s) {
  size_t i = 0;
  while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
  size_t start = i;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
  size_t digitsStart = i;
  while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
  if (i == digitsStart) return std::nullopt;  // no digits -> NaN
  try {
    return std::stoll(s.substr(start, i - start));
  } catch (...) {
    return std::nullopt;
  }
}

// hex _id from a (lean) doc.
std::string docHexId(const Json::Value& d) {
  if (d.isObject() && d.isMember("_id") && d["_id"].isString())
    return d["_id"].asString();
  return "";
}

// .populate('author'|'user', 'username name avatar profile isVerified') — hydrate
// each doc's ref ObjectId into the projected user subdocument, in one $in query.
// Docs whose ref cannot be resolved keep their original ref (Mongoose leaves a
// dangling ref as-is). `key` is the property holding the ref.
void populateUsers(Json::Value& docs, const std::string& key) {
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
    auto projection = make_document(kvp("username", 1), kvp("name", 1),
                                    kvp("avatar", 1), kvp("profile", 1),
                                    kvp("isVerified", 1));
    mongocxx::options::find opts{};
    opts.projection(projection.view());
    auto col = pulse::db::collection(pulse::models::user::kCollection);
    auto cursor = col.find(filter.view(), opts);
    for (const auto& doc : cursor) {
      Json::Value u = bj::toJson(doc);
      if (u.isMember("_id") && u["_id"].isString()) byId[u["_id"].asString()] = u;
    }
  } catch (const std::exception& e) {
    pulse::log::error("[bookmarks] user populate failed: {}", e.what());
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

// post.likes?.some(id => id.toString() === userId.toString())
// `likes` is the array of like ObjectId hex strings from the post document.
bool postIsLikedBy(const Json::Value& post, const std::string& userId) {
  if (!post.isObject() || !post.isMember("likes")) return false;
  const Json::Value& likes = post["likes"];
  if (!likes.isArray()) return false;
  for (const auto& l : likes) {
    std::string hex;
    if (l.isString()) hex = l.asString();
    else if (l.isObject() && l.isMember("_id") && l["_id"].isString())
      hex = l["_id"].asString();
    if (hex == userId) return true;
  }
  return false;
}

// 500 handler shape: res.status(500).json({ success:false, message:error.message }).
HttpResponsePtr serverErrorMessage(const std::string& message) {
  Json::Value body(Json::objectValue);
  body["success"] = false;
  body["message"] = message;
  return pulse::http::json(drogon::k500InternalServerError, body);
}

}  // namespace

// =========================================================
//  Toggle bookmark (add/remove)
//  POST /api/v1/bookmarks
// =========================================================
void BookmarkController::toggleBookmark(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    Json::Value user = authUser(req);
    std::string userId = authUserId(user);

    auto bodyPtr = req->getJsonObject();
    Json::Value body =
        (bodyPtr && bodyPtr->isObject()) ? *bodyPtr : Json::Value(Json::objectValue);

    // const { itemId, itemType } = req.body;
    std::string itemId =
        (body.isMember("itemId") && body["itemId"].isString()) ? body["itemId"].asString() : "";
    std::string itemType =
        (body.isMember("itemType") && body["itemType"].isString()) ? body["itemType"].asString() : "";

    // if (!itemId || !itemType) -> 400 'itemId and itemType required'
    bool itemIdFalsy = itemId.empty();
    bool itemTypeFalsy = itemType.empty();
    if (itemIdFalsy || itemTypeFalsy) {
      Json::Value b(Json::objectValue);
      b["success"] = false;
      b["message"] = "itemId and itemType required";
      callback(pulse::http::json(drogon::k400BadRequest, b));
      return;
    }

    // if (!['post','reel'].includes(itemType)) -> 400 'itemType must be post or reel'
    if (itemType != pulse::models::bookmark::kItemTypePost &&
        itemType != pulse::models::bookmark::kItemTypeReel) {
      Json::Value b(Json::objectValue);
      b["success"] = false;
      b["message"] = "itemType must be post or reel";
      callback(pulse::http::json(drogon::k400BadRequest, b));
      return;
    }

    auto bookmarks = pulse::db::collection(pulse::models::bookmark::kCollection);

    // Bookmark.findOne({ user: userId, itemId, itemType })
    bld::document findFilter;
    appendRef(findFilter, "user", userId);
    appendRef(findFilter, "itemId", itemId);
    findFilter.append(kvp("itemType", itemType));
    auto existing = bookmarks.find_one(findFilter.view());

    if (existing) {
      // Bookmark.deleteOne({ _id: existing._id })
      Json::Value existingJson = bj::toJson(existing->view());
      bld::document delFilter;
      std::string existingId = docHexId(existingJson);
      if (auto o = bj::tryOid(existingId)) delFilter.append(kvp("_id", *o));
      else delFilter.append(kvp("_id", existingId));
      bookmarks.delete_one(delFilter.view());

      // Decrement saves count for reels.
      if (itemType == pulse::models::bookmark::kItemTypeReel) {
        auto reels = pulse::db::collection(pulse::models::reel::kCollection);
        bld::document rf;
        if (auto o = bj::tryOid(itemId)) rf.append(kvp("_id", *o));
        else rf.append(kvp("_id", itemId));
        reels.update_one(
            rf.view(),
            make_document(kvp("$inc", make_document(kvp("stats.saves", -1)))));
      }

      // return res.json({ success: true, data: { isBookmarked: false } });
      Json::Value data(Json::objectValue);
      data["isBookmarked"] = false;
      callback(pulse::http::ok(data, drogon::k200OK));
      return;
    }

    // Bookmark.create({ user: userId, itemId, itemType })
    {
      Json::Value doc(Json::objectValue);
      doc["user"] = userId;
      doc["itemId"] = itemId;
      doc["itemType"] = itemType;
      Json::Value defaulted = pulse::models::bookmark::applyDefaults(doc);

      bsoncxx::oid newId;
      bld::document insert;
      insert.append(kvp("_id", newId));
      appendRef(insert, "user", userId);
      appendRef(insert, "itemId", itemId);
      insert.append(kvp("itemType", itemType));
      long long now = nowMillis();
      insert.append(kvp("createdAt", bsoncxx::types::b_date{std::chrono::milliseconds{now}}));
      insert.append(kvp("updatedAt", bsoncxx::types::b_date{std::chrono::milliseconds{now}}));
      insert.append(kvp("__v", static_cast<std::int64_t>(0)));
      bookmarks.insert_one(insert.view());
    }

    // Increment saves count for reels.
    if (itemType == pulse::models::bookmark::kItemTypeReel) {
      auto reels = pulse::db::collection(pulse::models::reel::kCollection);
      bld::document rf;
      if (auto o = bj::tryOid(itemId)) rf.append(kvp("_id", *o));
      else rf.append(kvp("_id", itemId));
      reels.update_one(
          rf.view(),
          make_document(kvp("$inc", make_document(kvp("stats.saves", 1)))));
    }

    // res.status(201).json({ success: true, data: { isBookmarked: true } });
    Json::Value data(Json::objectValue);
    data["isBookmarked"] = true;
    callback(pulse::http::ok(data, drogon::k201Created));
    return;
  } catch (const std::exception& e) {
    pulse::log::error("Toggle bookmark error: {}", e.what());
    callback(serverErrorMessage(e.what()));
    return;
  }
}

// =========================================================
//  Get user's bookmarks
//  GET /api/v1/bookmarks?type=post|reel
// =========================================================
void BookmarkController::getBookmarks(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    Json::Value user = authUser(req);
    std::string userId = authUserId(user);

    // const { type = 'post', page = 1, limit = 20 } = req.query;
    std::string typeRaw = req->getParameter("type");
    std::string type = typeRaw.empty() ? "post" : typeRaw;

    std::string pageRaw = req->getParameter("page");
    std::string limitRaw = req->getParameter("limit");

    // parseInt(page) / parseInt(limit) with JS defaults 1 / 20 (numbers when the
    // query param is absent).
    long long page = pageRaw.empty() ? 1 : parseIntJs(pageRaw).value_or(0);
    long long limit = limitRaw.empty() ? 20 : parseIntJs(limitRaw).value_or(0);
    // echoed values in pagination are parseInt(page)/parseInt(limit).
    long long pageEcho = pageRaw.empty() ? 1 : parseIntJs(pageRaw).value_or(0);
    long long limitEcho = limitRaw.empty() ? 20 : parseIntJs(limitRaw).value_or(0);

    long long skip = (page - 1) * limit;

    // Bookmark.find({ user: userId, itemType: type }).sort({createdAt:-1})
    //   .skip((page-1)*limit).limit(limit).lean()
    std::vector<std::string> itemIds;
    {
      auto bookmarks = pulse::db::collection(pulse::models::bookmark::kCollection);
      bld::document filter;
      appendRef(filter, "user", userId);
      filter.append(kvp("itemType", type));
      mongocxx::options::find opts{};
      opts.sort(make_document(kvp("createdAt", -1)));
      if (skip > 0) opts.skip(static_cast<std::int64_t>(skip));
      opts.limit(static_cast<std::int64_t>(limit));
      auto cursor = bookmarks.find(filter.view(), opts);
      for (const auto& d : cursor) {
        Json::Value b = bj::toJson(d);
        // const itemIds = bookmarks.map(b => b.itemId)
        if (b.isMember("itemId")) {
          const Json::Value& it = b["itemId"];
          if (it.isString()) itemIds.push_back(it.asString());
          else if (it.isObject() && it.isMember("_id") && it["_id"].isString())
            itemIds.push_back(it["_id"].asString());
        }
      }
    }

    Json::Value items(Json::arrayValue);

    if (type == "post") {
      // Post.find({ _id: { $in: itemIds }, isActive: true })
      //   .populate('author', 'username name avatar profile isVerified').lean()
      auto posts = pulse::db::collection(pulse::models::post::kCollection);
      bld::array in;
      for (const auto& id : itemIds)
        if (auto o = bj::tryOid(id)) in.append(*o);
      auto filter = make_document(
          kvp("_id", make_document(kvp("$in", in.extract()))),
          kvp("isActive", true));
      auto cursor = posts.find(filter.view());
      for (const auto& d : cursor) items.append(bj::toJson(d));
      populateUsers(items, "author");

      // items = items.map(post => ({ ...post,
      //   isLiked: post.likes?.some(id => id == userId) || false,
      //   isBookmarked: true }))
      for (auto& post : items) {
        post["isLiked"] = postIsLikedBy(post, userId);
        post["isBookmarked"] = true;
      }
    } else if (type == "reel") {
      // Reel.find({ _id: { $in: itemIds } })
      //   .populate('user', 'username name avatar profile isVerified').lean()
      auto reels = pulse::db::collection(pulse::models::reel::kCollection);
      bld::array in;
      for (const auto& id : itemIds)
        if (auto o = bj::tryOid(id)) in.append(*o);
      auto filter = make_document(kvp("_id", make_document(kvp("$in", in.extract()))));
      auto cursor = reels.find(filter.view());
      for (const auto& d : cursor) items.append(bj::toJson(d));
      populateUsers(items, "user");

      // items = items.map(reel => ({ ...reel, isSaved: true }))
      for (auto& reel : items) reel["isSaved"] = true;
    }

    // Maintain bookmark order:
    //   itemMap = new Map(items.map(i => [i._id.toString(), i]))
    //   ordered = itemIds.map(id => itemMap.get(id.toString())).filter(Boolean)
    std::map<std::string, Json::Value> itemMap;
    for (const auto& i : items) {
      std::string id = docHexId(i);
      if (!id.empty()) itemMap[id] = i;
    }
    Json::Value ordered(Json::arrayValue);
    for (const auto& id : itemIds) {
      auto it = itemMap.find(id);
      if (it != itemMap.end()) ordered.append(it->second);
    }

    // res.json({ success:true, data:ordered, pagination:{ page, limit } })
    Json::Value extra(Json::objectValue);
    extra["data"] = ordered;
    Json::Value pagination(Json::objectValue);
    pagination["page"] = static_cast<Json::Int64>(pageEcho);
    pagination["limit"] = static_cast<Json::Int64>(limitEcho);
    extra["pagination"] = pagination;
    callback(pulse::http::success(extra, drogon::k200OK));
    return;
  } catch (const std::exception& e) {
    pulse::log::error("Get bookmarks error: {}", e.what());
    callback(serverErrorMessage(e.what()));
    return;
  }
}

// =========================================================
//  Check if item is bookmarked
//  GET /api/v1/bookmarks/check/:itemId
// =========================================================
void BookmarkController::checkBookmark(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    std::string itemId) {
  try {
    Json::Value user = authUser(req);
    std::string userId = authUserId(user);

    // Bookmark.findOne({ user: userId, itemId })
    auto bookmarks = pulse::db::collection(pulse::models::bookmark::kCollection);
    bld::document filter;
    appendRef(filter, "user", userId);
    appendRef(filter, "itemId", itemId);
    auto exists = bookmarks.find_one(filter.view());

    // res.json({ success: true, data: { isBookmarked: !!exists } })
    Json::Value data(Json::objectValue);
    data["isBookmarked"] = static_cast<bool>(exists);
    callback(pulse::http::ok(data, drogon::k200OK));
    return;
  } catch (const std::exception& e) {
    pulse::log::error("Check bookmark error: {}", e.what());
    callback(serverErrorMessage(e.what()));
    return;
  }
}

}  // namespace pulse::controllers
