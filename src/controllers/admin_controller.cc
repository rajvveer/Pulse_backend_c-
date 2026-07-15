// admin_controller.cc — port of src/controllers/adminController.js.
//
// 1:1 functional parity with the Express handlers: same DB collections/queries,
// same projections, same pagination math, same response JSON shapes and status
// codes. Admin responses use the { success, message } / { success, data,
// pagination } shapes from the JS (NOT the { success, error, code } error shape),
// so they are built with pulse::http::json directly to match exactly.
#include "pulse/controllers/admin_controller.hpp"

#include "pulse/http_response.hpp"
#include "pulse/db.hpp"
#include "pulse/cache.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"
#include "pulse/services/auth_service.hpp"
#include "pulse/models/user.hpp"
#include "pulse/models/post.hpp"
#include "pulse/models/reel.hpp"
#include "pulse/models/like.hpp"
#include "pulse/models/message.hpp"
#include "pulse/models/conversation.hpp"
#include "pulse/models/session.hpp"
#include "pulse/models/pulsedrop.hpp"
#include "pulse/models/follow.hpp"
#include "pulse/models/comment.hpp"
#include "pulse/models/reelcomment.hpp"
#include "pulse/models/whisper.hpp"
#include "pulse/models/snap.hpp"
#include "pulse/models/chainstory.hpp"
#include "pulse/models/alterego.hpp"
#include "pulse/models/bookmark.hpp"
#include "pulse/models/notification.hpp"

#include <pulse_bcrypt.h>
#include <openssl/rand.h>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/pipeline.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/find_one_and_update.hpp>
#include <mongocxx/options/find_one_and_delete.hpp>

#include <chrono>
#include <cmath>
#include <cctype>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace pulse::controllers {

using namespace drogon;
using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::make_array;

namespace {

// ── file-local helpers ───────────────────────────────────────────────────────

// crypto.randomBytes(n).toString('hex') — n random bytes as lowercase hex.
std::string randomHex(int bytes) {
  static const char* kHex = "0123456789abcdef";
  std::vector<unsigned char> buf(static_cast<size_t>(bytes));
  if (RAND_bytes(buf.data(), bytes) != 1) {
    throw std::runtime_error("Secure random generation failed");
  }
  std::string out;
  out.reserve(static_cast<size_t>(bytes) * 2);
  for (unsigned char b : buf) {
    out.push_back(kHex[(b >> 4) & 0xF]);
    out.push_back(kHex[b & 0xF]);
  }
  return out;
}

// utils/escapeRegex.js — escape regex metacharacters in user input before using
// it in a MongoDB $regex. /[.*+?^${}()|[\]\\]/g -> '\\$&'.
std::string escapeRegex(const std::string& str) {
  static const std::string kMeta = ".*+?^${}()|[]\\";
  std::string out;
  out.reserve(str.size() * 2);
  for (char c : str) {
    if (kMeta.find(c) != std::string::npos) out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

// String(x).toLowerCase().trim() — lowercase + trim ASCII whitespace.
std::string lowerTrim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  std::string out = s.substr(b, e - b);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return out;
}

// q.trim() — trim ASCII whitespace.
std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

// bcrypt.compare(password, hash) — 0 == match.
bool bcryptCompare(const std::string& plain, const std::string& hash) {
  return bcrypt_checkpw(plain.c_str(), hash.c_str()) == 0;
}

// parseInt(req.query.page) || 1, clamped: Math.max(.., 1).
int parsePage(const HttpRequestPtr& req) {
  int page = 1;
  try {
    const std::string s = req->getParameter("page");
    if (!s.empty()) page = std::stoi(s);
  } catch (...) { page = 1; }
  if (page < 1) page = 1;
  return page;
}

// parseInt(req.query.limit) || 20, clamped: Math.min(Math.max(.., 1), 100).
int parseLimit(const HttpRequestPtr& req) {
  int limit = 20;
  try {
    const std::string s = req->getParameter("limit");
    if (!s.empty()) limit = std::stoi(s);
  } catch (...) { limit = 20; }
  if (limit < 1) limit = 1;
  if (limit > 100) limit = 100;
  return limit;
}

// res.status(code).json({ success: false, message }) — admin error shape.
HttpResponsePtr message(HttpStatusCode code, const std::string& msg) {
  Json::Value body(Json::objectValue);
  body["success"] = false;
  body["message"] = msg;
  return pulse::http::json(code, std::move(body));
}

// Math.ceil(total / limit).
int totalPages(long long total, int limit) {
  if (limit <= 0) return 0;
  return static_cast<int>(std::ceil(static_cast<double>(total) / limit));
}

Json::Value pagination(int page, int limit, long long total) {
  Json::Value p(Json::objectValue);
  p["page"] = page;
  p["limit"] = limit;
  p["total"] = static_cast<Json::Int64>(total);
  p["totalPages"] = totalPages(total, limit);
  return p;
}

// Populate one ref field on a row: replace `field` (an ObjectId hex string) with
// a sub-document projected to { _id, username, profile.avatar, avatar } from the
// users collection — mirroring .populate('author'|'user', 'username
// profile.avatar avatar'). A missing/invalid ref leaves the field as-is (Mongoose
// populate leaves the original value / null when no doc matches).
void populateUser(Json::Value& row, const char* field) {
  if (!row.isMember(field) || !row[field].isString()) return;
  const std::string hex = row[field].asString();
  auto oid = pulse::bsonjson::tryOid(hex);
  if (!oid) return;  // populate of a non-ObjectId ref is a no-op (leave as-is).
  auto users = pulse::db::collection(pulse::models::user::kCollection);
  mongocxx::options::find opts;
  opts.projection(make_document(kvp("username", 1), kvp("profile.avatar", 1),
                                kvp("avatar", 1)));
  auto doc = users.find_one(make_document(kvp("_id", *oid)), opts);
  if (!doc) return;
  row[field] = pulse::bsonjson::toJson(doc->view());
}

}  // namespace

// ==========================================
// AUTH — Admin login with username/email + password
// ==========================================
void AdminController::login(const HttpRequestPtr& req,
                            std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    auto body = req->getJsonObject();
    std::string identifier, password;
    if (body) {
      if ((*body).isMember("identifier") && (*body)["identifier"].isString())
        identifier = (*body)["identifier"].asString();
      if ((*body).isMember("password") && (*body)["password"].isString())
        password = (*body)["password"].asString();
    }

    if (identifier.empty() || password.empty()) {
      callback(message(k400BadRequest, "Identifier and password are required"));
      return;
    }

    // User.findOne({ $or:[{username},{email}], isActive:true }).select('+passwordHash')
    const std::string normalized = lowerTrim(identifier);
    auto users = pulse::db::collection(pulse::models::user::kCollection);
    auto userDoc = users.find_one(make_document(
        kvp("$or", make_array(make_document(kvp("username", normalized)),
                              make_document(kvp("email", normalized)))),
        kvp("isActive", true)));

    // Same error for every failure mode — don't reveal which part was wrong.
    auto invalid = [&]() { return message(k401Unauthorized, "Invalid credentials"); };

    if (!userDoc) { callback(invalid()); return; }
    Json::Value user = pulse::bsonjson::toJson(userDoc->view());

    const std::string passwordHash =
        (user.isMember("passwordHash") && user["passwordHash"].isString())
            ? user["passwordHash"].asString()
            : "";
    if (passwordHash.empty()) { callback(invalid()); return; }

    const std::string role =
        (user.isMember("role") && user["role"].isString()) ? user["role"].asString() : "";
    if (role != "admin") { callback(invalid()); return; }

    if (!bcryptCompare(password, passwordHash)) { callback(invalid()); return; }

    pulse::services::DeviceInfo deviceInfo;
    deviceInfo.deviceId = std::string("admin-panel-") + randomHex(8);
    deviceInfo.platform = "web";
    deviceInfo.deviceName = std::string("Admin Panel");

    std::string ip = req->getPeerAddr().toIp();
    if (ip.empty()) ip = "127.0.0.1";

    Json::Value sessionResult =
        pulse::services::authService().createUserSession(user, deviceInfo, ip);

    // avatar: user.profile?.avatar || user.avatar || null
    Json::Value avatar = Json::nullValue;
    if (user.isMember("profile") && user["profile"].isObject() &&
        user["profile"].isMember("avatar") && !user["profile"]["avatar"].isNull() &&
        !(user["profile"]["avatar"].isString() && user["profile"]["avatar"].asString().empty())) {
      avatar = user["profile"]["avatar"];
    } else if (user.isMember("avatar") && !user["avatar"].isNull() &&
               !(user["avatar"].isString() && user["avatar"].asString().empty())) {
      avatar = user["avatar"];
    }

    Json::Value userOut(Json::objectValue);
    userOut["_id"] = user.isMember("_id") ? user["_id"] : Json::Value(Json::nullValue);
    userOut["username"] = user.isMember("username") ? user["username"] : Json::Value(Json::nullValue);
    userOut["email"] = user.isMember("email") ? user["email"] : Json::Value(Json::nullValue);
    userOut["role"] = role;
    userOut["avatar"] = avatar;

    Json::Value data(Json::objectValue);
    data["tokens"] = sessionResult.isMember("tokens") ? sessionResult["tokens"]
                                                      : Json::Value(Json::objectValue);
    data["user"] = userOut;

    callback(pulse::http::ok(std::move(data)));
  } catch (const std::exception& e) {
    pulse::log::error("Admin login error: {}", e.what());
    callback(message(k500InternalServerError, "Login failed"));
  }
}

// ==========================================
// DASHBOARD STATS
// ==========================================
void AdminController::getStats(const HttpRequestPtr& req,
                               std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    using namespace std::chrono;
    const long long nowMs =
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    const long long sinceMs = nowMs - 14LL * 24 * 60 * 60 * 1000;
    bsoncxx::types::b_date since{milliseconds{sinceMs}};
    bsoncxx::types::b_date nowDate{milliseconds{nowMs}};

    auto usersCol = pulse::db::collection(pulse::models::user::kCollection);
    auto postsCol = pulse::db::collection(pulse::models::post::kCollection);
    auto reelsCol = pulse::db::collection(pulse::models::reel::kCollection);
    auto messagesCol = pulse::db::collection(pulse::models::message::kCollection);
    auto conversationsCol = pulse::db::collection(pulse::models::conversation::kCollection);
    auto sessionsCol = pulse::db::collection(pulse::models::session::kCollection);
    auto dropsCol = pulse::db::collection(pulse::models::pulsedrop::kCollection);
    auto followsCol = pulse::db::collection(pulse::models::follow::kCollection);
    auto likesCol = pulse::db::collection(pulse::models::like::kCollection);
    auto commentsCol = pulse::db::collection(pulse::models::comment::kCollection);
    auto reelCommentsCol = pulse::db::collection(pulse::models::reelcomment::kCollection);
    auto whispersCol = pulse::db::collection(pulse::models::whisper::kCollection);
    auto snapsCol = pulse::db::collection(pulse::models::snap::kCollection);
    auto chainsCol = pulse::db::collection(pulse::models::chainstory::kCollection);
    auto alterEgosCol = pulse::db::collection(pulse::models::alterego::kCollection);
    auto bookmarksCol = pulse::db::collection(pulse::models::bookmark::kCollection);
    auto notificationsCol = pulse::db::collection(pulse::models::notification::kCollection);

    const long long totalUsers = usersCol.count_documents(make_document());
    const long long activeUsers = usersCol.count_documents(make_document(kvp("isActive", true)));
    const long long verifiedUsers =
        usersCol.count_documents(make_document(kvp("isVerified", true)));
    const long long onlineUsers = usersCol.count_documents(make_document(kvp("isOnline", true)));
    const long long totalPosts = postsCol.count_documents(make_document());
    const long long activePosts = postsCol.count_documents(make_document(kvp("isActive", true)));
    const long long anonymousPosts =
        postsCol.count_documents(make_document(kvp("isAnonymous", true)));
    const long long totalReels = reelsCol.count_documents(make_document());
    const long long totalMessages = messagesCol.count_documents(make_document());
    const long long totalConversations = conversationsCol.count_documents(make_document());
    const long long activeSessions = sessionsCol.count_documents(make_document(
        kvp("isActive", true), kvp("expiresAt", make_document(kvp("$gt", nowDate)))));
    const long long activeDrops =
        dropsCol.count_documents(make_document(kvp("status", "active")));
    const long long totalDrops = dropsCol.count_documents(make_document());
    const long long totalFollows = followsCol.count_documents(make_document());
    const long long totalLikes = likesCol.count_documents(make_document());
    const long long totalComments = commentsCol.count_documents(make_document());
    const long long totalReelComments = reelCommentsCol.count_documents(make_document());
    const long long totalWhispers = whispersCol.count_documents(make_document());
    const long long totalSnaps = snapsCol.count_documents(make_document());
    const long long totalChainStories = chainsCol.count_documents(make_document());
    const long long totalAlterEgos = alterEgosCol.count_documents(make_document());
    const long long totalBookmarks = bookmarksCol.count_documents(make_document());
    const long long totalNotifications = notificationsCol.count_documents(make_document());

    // dailyAgg(Model): match createdAt>=since, group by %Y-%m-%d, sort _id asc.
    auto dailyAgg = [&](mongocxx::collection& col) -> Json::Value {
      mongocxx::pipeline p;
      p.match(make_document(kvp("createdAt", make_document(kvp("$gte", since)))));
      p.group(make_document(
          kvp("_id", make_document(kvp(
                  "$dateToString",
                  make_document(kvp("format", "%Y-%m-%d"), kvp("date", "$createdAt"))))),
          kvp("count", make_document(kvp("$sum", 1)))));
      p.sort(make_document(kvp("_id", 1)));

      Json::Value out(Json::arrayValue);
      auto cursor = col.aggregate(p);
      for (auto&& doc : cursor) {
        Json::Value row = pulse::bsonjson::toJson(doc);
        Json::Value item(Json::objectValue);
        item["date"] = row.isMember("_id") ? row["_id"] : Json::Value(Json::nullValue);
        item["count"] = row.isMember("count") ? row["count"] : Json::Value(0);
        out.append(item);
      }
      return out;
    };

    Json::Value signupsByDay = dailyAgg(usersCol);
    Json::Value postsByDay = dailyAgg(postsCol);
    Json::Value reelsByDay = dailyAgg(reelsCol);
    Json::Value messagesByDay = dailyAgg(messagesCol);

    // topN(col, sortField, projection, populateField): find sorted desc, limit n.
    auto topN = [&](mongocxx::collection& col, const std::string& sortField,
                    bsoncxx::document::value proj, const char* popField,
                    int n) -> Json::Value {
      mongocxx::options::find o;
      o.sort(make_document(kvp(sortField, -1)));
      o.projection(proj.view());
      o.limit(n);
      Json::Value arr(Json::arrayValue);
      auto cur = col.find(make_document(), o);
      for (auto&& d : cur) {
        Json::Value r = pulse::bsonjson::toJson(d);
        if (popField) populateUser(r, popField);
        arr.append(r);
      }
      return arr;
    };

    Json::Value topPosts = topN(
        postsCol, "stats.likes",
        make_document(kvp("content.text", 1), kvp("stats", 1), kvp("isActive", 1),
                      kvp("isAnonymous", 1), kvp("createdAt", 1), kvp("author", 1)),
        "author", 5);
    Json::Value topReels = topN(
        reelsCol, "stats.views",
        make_document(kvp("caption", 1), kvp("stats", 1), kvp("duration", 1),
                      kvp("createdAt", 1), kvp("user", 1)),
        "user", 5);
    Json::Value topUsers = topN(
        usersCol, "stats.followers",
        make_document(kvp("username", 1), kvp("profile.displayName", 1),
                      kvp("profile.avatar", 1), kvp("avatar", 1), kvp("stats", 1),
                      kvp("isActive", 1), kvp("createdAt", 1)),
        nullptr, 5);

    Json::Value totals(Json::objectValue);
    totals["users"] = static_cast<Json::Int64>(totalUsers);
    totals["activeUsers"] = static_cast<Json::Int64>(activeUsers);
    totals["bannedUsers"] = static_cast<Json::Int64>(totalUsers - activeUsers);
    totals["verifiedUsers"] = static_cast<Json::Int64>(verifiedUsers);
    totals["onlineUsers"] = static_cast<Json::Int64>(onlineUsers);
    totals["posts"] = static_cast<Json::Int64>(totalPosts);
    totals["activePosts"] = static_cast<Json::Int64>(activePosts);
    totals["anonymousPosts"] = static_cast<Json::Int64>(anonymousPosts);
    totals["reels"] = static_cast<Json::Int64>(totalReels);
    totals["messages"] = static_cast<Json::Int64>(totalMessages);
    totals["conversations"] = static_cast<Json::Int64>(totalConversations);
    totals["activeSessions"] = static_cast<Json::Int64>(activeSessions);
    totals["activeDrops"] = static_cast<Json::Int64>(activeDrops);
    totals["totalDrops"] = static_cast<Json::Int64>(totalDrops);
    totals["follows"] = static_cast<Json::Int64>(totalFollows);
    totals["likes"] = static_cast<Json::Int64>(totalLikes);
    totals["comments"] = static_cast<Json::Int64>(totalComments);
    totals["reelComments"] = static_cast<Json::Int64>(totalReelComments);
    totals["whispers"] = static_cast<Json::Int64>(totalWhispers);
    totals["snaps"] = static_cast<Json::Int64>(totalSnaps);
    totals["chainStories"] = static_cast<Json::Int64>(totalChainStories);
    totals["alterEgos"] = static_cast<Json::Int64>(totalAlterEgos);
    totals["bookmarks"] = static_cast<Json::Int64>(totalBookmarks);
    totals["notifications"] = static_cast<Json::Int64>(totalNotifications);

    Json::Value charts(Json::objectValue);
    charts["signupsByDay"] = signupsByDay;
    charts["postsByDay"] = postsByDay;
    charts["reelsByDay"] = reelsByDay;
    charts["messagesByDay"] = messagesByDay;

    Json::Value top(Json::objectValue);
    top["posts"] = topPosts;
    top["reels"] = topReels;
    top["users"] = topUsers;

    Json::Value data(Json::objectValue);
    data["totals"] = totals;
    data["charts"] = charts;
    data["top"] = top;

    callback(pulse::http::ok(std::move(data)));
  } catch (const std::exception& e) {
    pulse::log::error("Admin stats error: {}", e.what());
    callback(message(k500InternalServerError, "Failed to load stats"));
  }
}

// ==========================================
// USER MANAGEMENT
// ==========================================
void AdminController::listUsers(const HttpRequestPtr& req,
                                std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    const int page = parsePage(req);
    const int limit = parseLimit(req);
    const std::string q = req->getParameter("q");
    const std::string role = req->getParameter("role");
    const std::string status = req->getParameter("status");

    bsoncxx::builder::basic::document query;
    if (!trim(q).empty()) {
      const std::string safe = escapeRegex(trim(q));
      query.append(kvp("$or", make_array(
          make_document(kvp("username", make_document(kvp("$regex", safe), kvp("$options", "i")))),
          make_document(kvp("email", make_document(kvp("$regex", safe), kvp("$options", "i")))),
          make_document(kvp("profile.displayName",
                            make_document(kvp("$regex", safe), kvp("$options", "i")))))));
    }
    if (role == "user" || role == "admin" || role == "moderator") {
      query.append(kvp("role", role));
    }
    if (status == "active") query.append(kvp("isActive", true));
    if (status == "banned") query.append(kvp("isActive", false));

    auto queryDoc = query.extract();

    auto users = pulse::db::collection(pulse::models::user::kCollection);

    mongocxx::options::find opts;
    opts.projection(make_document(
        kvp("username", 1), kvp("email", 1), kvp("phone", 1), kvp("role", 1),
        kvp("isActive", 1), kvp("isVerified", 1), kvp("isOnline", 1),
        kvp("lastActive", 1), kvp("createdAt", 1), kvp("stats", 1),
        kvp("profile.displayName", 1), kvp("profile.avatar", 1), kvp("avatar", 1)));
    opts.sort(make_document(kvp("createdAt", -1)));
    opts.skip((page - 1) * limit);
    opts.limit(limit);

    Json::Value data(Json::arrayValue);
    auto cursor = users.find(queryDoc.view(), opts);
    for (auto&& doc : cursor) data.append(pulse::bsonjson::toJson(doc));

    const long long total = users.count_documents(queryDoc.view());

    Json::Value out(Json::objectValue);
    out["data"] = data;
    out["pagination"] = pagination(page, limit, total);
    callback(pulse::http::success(std::move(out)));
  } catch (const std::exception& e) {
    pulse::log::error("Admin list users error: {}", e.what());
    callback(message(k500InternalServerError, "Failed to load users"));
  }
}

// GET /users/:userId — full admin view of one account plus their recent posts.
// Not part of the JS admin API; added for the admin panel's user detail page.
void AdminController::getUser(const HttpRequestPtr& req,
                             std::function<void(const HttpResponsePtr&)>&& callback,
                             std::string userId) {
  try {
    auto oid = pulse::bsonjson::tryOid(userId);
    if (!oid) { callback(message(k404NotFound, "User not found")); return; }

    auto users = pulse::db::collection(pulse::models::user::kCollection);
    auto doc = users.find_one(make_document(kvp("_id", *oid)));
    if (!doc) { callback(message(k404NotFound, "User not found")); return; }

    // sanitizeForOutput strips passwordHash / loginAttempts / etc. (toJSON parity).
    Json::Value user =
        pulse::models::user::sanitizeForOutput(pulse::bsonjson::toJson(doc->view()));
    // Admin detail view still shouldn't expose device push tokens, the private
    // block list, or raw auth identifiers.
    user.removeMember("fcmTokens");
    user.removeMember("blockedUsers");
    user.removeMember("authMethods");

    // Recent posts by this author — admin view, so removed posts are included.
    auto posts = pulse::db::collection(pulse::models::post::kCollection);
    mongocxx::options::find popts;
    popts.projection(make_document(
        kvp("content", 1), kvp("visibility", 1), kvp("isActive", 1),
        kvp("isAnonymous", 1), kvp("stats", 1), kvp("createdAt", 1)));
    popts.sort(make_document(kvp("createdAt", -1)));
    popts.limit(10);
    Json::Value recentPosts(Json::arrayValue);
    auto pcur = posts.find(make_document(kvp("author", *oid)), popts);
    for (auto&& p : pcur) recentPosts.append(pulse::bsonjson::toJson(p));

    Json::Value data(Json::objectValue);
    data["user"] = user;
    data["recentPosts"] = recentPosts;
    callback(pulse::http::ok(std::move(data)));
  } catch (const std::exception& e) {
    pulse::log::error("Admin get user error: {}", e.what());
    callback(message(k500InternalServerError, "Failed to load user"));
  }
}

void AdminController::updateUserStatus(const HttpRequestPtr& req,
                                       std::function<void(const HttpResponsePtr&)>&& callback,
                                       std::string userId) {
  try {
    auto body = req->getJsonObject();
    if (!body || !(*body).isMember("isActive") || !(*body)["isActive"].isBool()) {
      callback(message(k400BadRequest, "isActive (boolean) is required"));
      return;
    }
    const bool isActive = (*body)["isActive"].asBool();

    Json::Value authUser = req->getAttributes()->get<Json::Value>("user");
    const std::string selfId =
        authUser.isMember("userId") ? authUser["userId"].asString() : "";
    if (userId == selfId) {
      callback(message(k400BadRequest, "You cannot change your own account status"));
      return;
    }

    // findByIdAndUpdate casts userId to ObjectId; a malformed id throws a
    // CastError in Mongoose -> caught below -> 500 (matching the JS try/catch).
    const auto oid = pulse::bsonjson::oid(userId);

    auto users = pulse::db::collection(pulse::models::user::kCollection);
    mongocxx::options::find_one_and_update fopts;
    fopts.return_document(mongocxx::options::return_document::k_after);
    fopts.projection(make_document(kvp("username", 1), kvp("role", 1), kvp("isActive", 1)));
    auto updated = users.find_one_and_update(
        make_document(kvp("_id", oid)),
        make_document(kvp("$set", make_document(kvp("isActive", isActive)))), fopts);

    if (!updated) {
      callback(message(k404NotFound, "User not found"));
      return;
    }

    if (!isActive) {
      // Kill the user's sessions and the auth cache so the ban takes effect now.
      auto sessions = pulse::db::collection(pulse::models::session::kCollection);
      sessions.update_many(
          make_document(kvp("userId", oid), kvp("isActive", true)),
          make_document(kvp("$set", make_document(kvp("isActive", false)))));
      try {
        pulse::cache().del("auth_user:" + userId);
      } catch (...) { /* cache miss is fine */ }
    }

    callback(pulse::http::ok(pulse::bsonjson::toJson(updated->view())));
  } catch (const std::exception& e) {
    pulse::log::error("Admin update user status error: {}", e.what());
    callback(message(k500InternalServerError, "Failed to update user"));
  }
}

void AdminController::updateUserRole(const HttpRequestPtr& req,
                                     std::function<void(const HttpResponsePtr&)>&& callback,
                                     std::string userId) {
  try {
    auto body = req->getJsonObject();
    const std::string role =
        (body && (*body).isMember("role") && (*body)["role"].isString())
            ? (*body)["role"].asString()
            : "";
    if (!(role == "user" || role == "admin" || role == "moderator")) {
      callback(message(k400BadRequest, "Role must be user, admin, or moderator"));
      return;
    }

    Json::Value authUser = req->getAttributes()->get<Json::Value>("user");
    const std::string selfId =
        authUser.isMember("userId") ? authUser["userId"].asString() : "";
    if (userId == selfId) {
      callback(message(k400BadRequest, "You cannot change your own role"));
      return;
    }

    const auto oid = pulse::bsonjson::oid(userId);  // CastError -> 500 (JS parity).

    auto users = pulse::db::collection(pulse::models::user::kCollection);
    mongocxx::options::find_one_and_update fopts;
    fopts.return_document(mongocxx::options::return_document::k_after);
    fopts.projection(make_document(kvp("username", 1), kvp("role", 1), kvp("isActive", 1)));
    auto updated = users.find_one_and_update(
        make_document(kvp("_id", oid)),
        make_document(kvp("$set", make_document(kvp("role", role)))), fopts);

    if (!updated) {
      callback(message(k404NotFound, "User not found"));
      return;
    }

    callback(pulse::http::ok(pulse::bsonjson::toJson(updated->view())));
  } catch (const std::exception& e) {
    pulse::log::error("Admin update user role error: {}", e.what());
    callback(message(k500InternalServerError, "Failed to update role"));
  }
}

// ==========================================
// POST MODERATION
// ==========================================
void AdminController::listPosts(const HttpRequestPtr& req,
                                std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    const int page = parsePage(req);
    const int limit = parseLimit(req);
    const std::string q = req->getParameter("q");
    const std::string status = req->getParameter("status");

    bsoncxx::builder::basic::document query;
    if (!trim(q).empty()) {
      const std::string safe = escapeRegex(trim(q));
      query.append(kvp("content.text",
                       make_document(kvp("$regex", safe), kvp("$options", "i"))));
    }
    if (status == "active") query.append(kvp("isActive", true));
    if (status == "removed") query.append(kvp("isActive", false));

    auto queryDoc = query.extract();

    auto posts = pulse::db::collection(pulse::models::post::kCollection);

    mongocxx::options::find opts;
    opts.projection(make_document(
        kvp("content", 1), kvp("visibility", 1), kvp("isActive", 1),
        kvp("isAnonymous", 1), kvp("stats", 1), kvp("createdAt", 1), kvp("author", 1)));
    opts.sort(make_document(kvp("createdAt", -1)));
    opts.skip((page - 1) * limit);
    opts.limit(limit);

    Json::Value data(Json::arrayValue);
    auto cursor = posts.find(queryDoc.view(), opts);
    for (auto&& doc : cursor) {
      Json::Value row = pulse::bsonjson::toJson(doc);
      populateUser(row, "author");  // .populate('author','username profile.avatar avatar')
      data.append(row);
    }

    const long long total = posts.count_documents(queryDoc.view());

    Json::Value out(Json::objectValue);
    out["data"] = data;
    out["pagination"] = pagination(page, limit, total);
    callback(pulse::http::success(std::move(out)));
  } catch (const std::exception& e) {
    pulse::log::error("Admin list posts error: {}", e.what());
    callback(message(k500InternalServerError, "Failed to load posts"));
  }
}

// GET /posts/:postId — full post (removed ones too) + author + its comments.
void AdminController::getPost(const HttpRequestPtr& req,
                             std::function<void(const HttpResponsePtr&)>&& callback,
                             std::string postId) {
  try {
    auto oid = pulse::bsonjson::tryOid(postId);
    if (!oid) { callback(message(k404NotFound, "Post not found")); return; }

    auto posts = pulse::db::collection(pulse::models::post::kCollection);
    auto doc = posts.find_one(make_document(kvp("_id", *oid)));  // no isActive filter.
    if (!doc) { callback(message(k404NotFound, "Post not found")); return; }

    Json::Value post =
        pulse::models::post::sanitizeForOutput(pulse::bsonjson::toJson(doc->view()));
    populateUser(post, "author");

    // Comments on this post, newest first, each with its author populated.
    auto commentsCol = pulse::db::collection(pulse::models::comment::kCollection);
    mongocxx::options::find copts;
    copts.sort(make_document(kvp("createdAt", -1)));
    copts.limit(50);
    Json::Value comments(Json::arrayValue);
    auto ccur = commentsCol.find(make_document(kvp("post", *oid)), copts);
    for (auto&& c : ccur) {
      Json::Value row = pulse::bsonjson::toJson(c);
      populateUser(row, "author");
      comments.append(row);
    }

    Json::Value data(Json::objectValue);
    data["post"] = post;
    data["comments"] = comments;
    callback(pulse::http::ok(std::move(data)));
  } catch (const std::exception& e) {
    pulse::log::error("Admin get post error: {}", e.what());
    callback(message(k500InternalServerError, "Failed to load post"));
  }
}

void AdminController::updatePostStatus(const HttpRequestPtr& req,
                                       std::function<void(const HttpResponsePtr&)>&& callback,
                                       std::string postId) {
  try {
    auto body = req->getJsonObject();
    if (!body || !(*body).isMember("isActive") || !(*body)["isActive"].isBool()) {
      callback(message(k400BadRequest, "isActive (boolean) is required"));
      return;
    }
    const bool isActive = (*body)["isActive"].asBool();

    const auto oid = pulse::bsonjson::oid(postId);  // CastError -> 500 (JS parity).

    auto posts = pulse::db::collection(pulse::models::post::kCollection);
    mongocxx::options::find_one_and_update fopts;
    fopts.return_document(mongocxx::options::return_document::k_after);
    fopts.projection(make_document(kvp("isActive", 1), kvp("author", 1), kvp("content.text", 1)));
    auto updated = posts.find_one_and_update(
        make_document(kvp("_id", oid)),
        make_document(kvp("$set", make_document(kvp("isActive", isActive)))), fopts);

    if (!updated) {
      callback(message(k404NotFound, "Post not found"));
      return;
    }

    callback(pulse::http::ok(pulse::bsonjson::toJson(updated->view())));
  } catch (const std::exception& e) {
    pulse::log::error("Admin update post status error: {}", e.what());
    callback(message(k500InternalServerError, "Failed to update post"));
  }
}

// ==========================================
// REEL MODERATION
// ==========================================
void AdminController::listReels(const HttpRequestPtr& req,
                                std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    const int page = parsePage(req);
    const int limit = parseLimit(req);
    const std::string q = req->getParameter("q");

    bsoncxx::builder::basic::document query;
    if (!trim(q).empty()) {
      const std::string safe = escapeRegex(trim(q));
      query.append(kvp("caption", make_document(kvp("$regex", safe), kvp("$options", "i"))));
    }

    auto queryDoc = query.extract();

    auto reels = pulse::db::collection(pulse::models::reel::kCollection);

    mongocxx::options::find opts;
    opts.projection(make_document(
        kvp("caption", 1), kvp("videoUrl", 1), kvp("stats", 1), kvp("duration", 1),
        kvp("hashtags", 1), kvp("createdAt", 1), kvp("user", 1)));
    opts.sort(make_document(kvp("createdAt", -1)));
    opts.skip((page - 1) * limit);
    opts.limit(limit);

    Json::Value data(Json::arrayValue);
    auto cursor = reels.find(queryDoc.view(), opts);
    for (auto&& doc : cursor) {
      Json::Value row = pulse::bsonjson::toJson(doc);
      populateUser(row, "user");  // .populate('user','username profile.avatar avatar')
      data.append(row);
    }

    const long long total = reels.count_documents(queryDoc.view());

    Json::Value out(Json::objectValue);
    out["data"] = data;
    out["pagination"] = pagination(page, limit, total);
    callback(pulse::http::success(std::move(out)));
  } catch (const std::exception& e) {
    pulse::log::error("Admin list reels error: {}", e.what());
    callback(message(k500InternalServerError, "Failed to load reels"));
  }
}

// GET /reels/:reelId — full reel + uploader + its comments.
void AdminController::getReel(const HttpRequestPtr& req,
                             std::function<void(const HttpResponsePtr&)>&& callback,
                             std::string reelId) {
  try {
    auto oid = pulse::bsonjson::tryOid(reelId);
    if (!oid) { callback(message(k404NotFound, "Reel not found")); return; }

    auto reels = pulse::db::collection(pulse::models::reel::kCollection);
    auto doc = reels.find_one(make_document(kvp("_id", *oid)));
    if (!doc) { callback(message(k404NotFound, "Reel not found")); return; }

    Json::Value reel =
        pulse::models::reel::sanitizeForOutput(pulse::bsonjson::toJson(doc->view()));
    populateUser(reel, "user");

    // Reel comments, newest first, each with its commenter populated.
    auto commentsCol = pulse::db::collection(pulse::models::reelcomment::kCollection);
    mongocxx::options::find copts;
    copts.sort(make_document(kvp("createdAt", -1)));
    copts.limit(50);
    Json::Value comments(Json::arrayValue);
    auto ccur = commentsCol.find(make_document(kvp("reel", *oid)), copts);
    for (auto&& c : ccur) {
      Json::Value row = pulse::bsonjson::toJson(c);
      populateUser(row, "author");  // ReelComment stores its commenter as `author`.
      comments.append(row);
    }

    Json::Value data(Json::objectValue);
    data["reel"] = reel;
    data["comments"] = comments;
    callback(pulse::http::ok(std::move(data)));
  } catch (const std::exception& e) {
    pulse::log::error("Admin get reel error: {}", e.what());
    callback(message(k500InternalServerError, "Failed to load reel"));
  }
}

void AdminController::deleteReel(const HttpRequestPtr& req,
                                 std::function<void(const HttpResponsePtr&)>&& callback,
                                 std::string reelId) {
  try {
    const auto oid = pulse::bsonjson::oid(reelId);  // CastError -> 500 (JS parity).

    auto reels = pulse::db::collection(pulse::models::reel::kCollection);
    auto deleted = reels.find_one_and_delete(make_document(kvp("_id", oid)));
    if (!deleted) {
      callback(message(k404NotFound, "Reel not found"));
      return;
    }

    // Clean up associated likes (Reel has no soft-delete flag).
    // Like.deleteMany({ targetType:'reel', targetId:reelId })
    auto likes = pulse::db::collection(pulse::models::like::kCollection);
    likes.delete_many(make_document(kvp("targetType", "reel"), kvp("targetId", oid)));

    Json::Value out(Json::objectValue);
    out["message"] = "Reel deleted";
    callback(pulse::http::success(std::move(out)));
  } catch (const std::exception& e) {
    pulse::log::error("Admin delete reel error: {}", e.what());
    callback(message(k500InternalServerError, "Failed to delete reel"));
  }
}

// ==========================================
// PULSE DROPS
// ==========================================
void AdminController::listDrops(const HttpRequestPtr& req,
                                std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    const int page = parsePage(req);
    const int limit = parseLimit(req);

    auto drops = pulse::db::collection(pulse::models::pulsedrop::kCollection);

    mongocxx::options::find opts;
    opts.sort(make_document(kvp("createdAt", -1)));
    opts.skip((page - 1) * limit);
    opts.limit(limit);

    Json::Value data(Json::arrayValue);
    auto cursor = drops.find(make_document(), opts);
    for (auto&& doc : cursor) data.append(pulse::bsonjson::toJson(doc));

    const long long total = drops.count_documents(make_document());

    Json::Value out(Json::objectValue);
    out["data"] = data;
    out["pagination"] = pagination(page, limit, total);
    callback(pulse::http::success(std::move(out)));
  } catch (const std::exception& e) {
    pulse::log::error("Admin list drops error: {}", e.what());
    callback(message(k500InternalServerError, "Failed to load drops"));
  }
}

// GET /drops/:dropId — full drop with its embedded participants[] (each user
// ref populated) so the detail page can show the responses.
void AdminController::getDrop(const HttpRequestPtr& req,
                             std::function<void(const HttpResponsePtr&)>&& callback,
                             std::string dropId) {
  try {
    auto oid = pulse::bsonjson::tryOid(dropId);
    if (!oid) { callback(message(k404NotFound, "Drop not found")); return; }

    auto drops = pulse::db::collection(pulse::models::pulsedrop::kCollection);
    auto doc = drops.find_one(make_document(kvp("_id", *oid)));
    if (!doc) { callback(message(k404NotFound, "Drop not found")); return; }

    Json::Value drop =
        pulse::models::pulsedrop::sanitizeForOutput(pulse::bsonjson::toJson(doc->view()));

    // participants: [{ user, response, joinedAt }] — viral drops can accumulate
    // thousands of entries, so cap what we return and resolve all user refs with
    // ONE batched $in lookup instead of a per-participant find_one.
    if (drop.isMember("participants") && drop["participants"].isArray()) {
      Json::Value& parts = drop["participants"];
      constexpr Json::ArrayIndex kMaxParticipants = 100;
      if (parts.size() > kMaxParticipants) {
        Json::Value trimmed(Json::arrayValue);
        for (Json::ArrayIndex i = 0; i < kMaxParticipants; ++i) trimmed.append(parts[i]);
        parts = std::move(trimmed);
      }

      bsoncxx::builder::basic::array oids;
      for (const Json::Value& p : parts) {
        if (p.isObject() && p.isMember("user") && p["user"].isString()) {
          if (auto o = pulse::bsonjson::tryOid(p["user"].asString())) oids.append(*o);
        }
      }

      std::unordered_map<std::string, Json::Value> usersById;
      auto usersCol = pulse::db::collection(pulse::models::user::kCollection);
      mongocxx::options::find uopts;
      uopts.projection(make_document(kvp("username", 1), kvp("profile.avatar", 1),
                                     kvp("avatar", 1)));
      auto ucur = usersCol.find(
          make_document(kvp("_id", make_document(kvp("$in", oids.extract())))), uopts);
      for (auto&& u : ucur) {
        Json::Value uj = pulse::bsonjson::toJson(u);
        if (uj.isMember("_id") && uj["_id"].isString()) usersById[uj["_id"].asString()] = uj;
      }

      for (Json::Value& p : parts) {
        if (p.isObject() && p.isMember("user") && p["user"].isString()) {
          auto it = usersById.find(p["user"].asString());
          if (it != usersById.end()) p["user"] = it->second;
        }
      }
    }

    Json::Value data(Json::objectValue);
    data["drop"] = drop;
    callback(pulse::http::ok(std::move(data)));
  } catch (const std::exception& e) {
    pulse::log::error("Admin get drop error: {}", e.what());
    callback(message(k500InternalServerError, "Failed to load drop"));
  }
}

void AdminController::expireDrop(const HttpRequestPtr& req,
                                 std::function<void(const HttpResponsePtr&)>&& callback,
                                 std::string dropId) {
  try {
    const auto oid = pulse::bsonjson::oid(dropId);  // CastError -> 500 (JS parity).

    auto drops = pulse::db::collection(pulse::models::pulsedrop::kCollection);
    mongocxx::options::find_one_and_update fopts;
    fopts.return_document(mongocxx::options::return_document::k_after);
    auto updated = drops.find_one_and_update(
        make_document(kvp("_id", oid)),
        make_document(kvp("$set", make_document(kvp("status", "expired")))), fopts);

    if (!updated) {
      callback(message(k404NotFound, "Drop not found"));
      return;
    }

    callback(pulse::http::ok(pulse::bsonjson::toJson(updated->view())));
  } catch (const std::exception& e) {
    pulse::log::error("Admin expire drop error: {}", e.what());
    callback(message(k500InternalServerError, "Failed to expire drop"));
  }
}

} // namespace pulse::controllers
