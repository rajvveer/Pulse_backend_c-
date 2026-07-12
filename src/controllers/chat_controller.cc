// chat_controller.cc — implementation of the chat route group
// (src/controllers/chatController.js + src/routes/chatRoutes.js).
//
// Ground truth: chatController.js. Every handler mirrors its JS counterpart 1:1 —
// same Mongo queries, same response JSON shapes ({ success, data } /
// { success, message } / { success, data, pagination }), and the same status
// codes (200 / 400 / 403 / 404 / 500). Mongoose .populate() of participants /
// admins / createdBy / sender / replyTo is reproduced by fetching the referenced
// User docs with the exact field selection the JS requested.
#include "pulse/controllers/chat_controller.hpp"

#include "pulse/http_response.hpp"
#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"
#include "pulse/models/conversation.hpp"
#include "pulse/models/message.hpp"
#include "pulse/models/user.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/oid.hpp>

#include <mongocxx/collection.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/find_one_and_update.hpp>
#include <mongocxx/cursor.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <exception>
#include <optional>
#include <string>
#include <vector>

namespace pulse::controllers {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;
using bld::make_array;

namespace {

// ---------------------------------------------------------------------------
// Response helpers — the chat controller does NOT use the { error, code } error
// envelope. It returns Express-style { success, message } / { success, data }.
// Build them verbatim so the JSON shape matches res.json() exactly.
// ---------------------------------------------------------------------------

// res.status(code).json({ success:false, message })
HttpResponsePtr failMessage(HttpStatusCode code, const std::string& message) {
  Json::Value body(Json::objectValue);
  body["success"] = false;
  body["message"] = message;
  return pulse::http::json(code, std::move(body));
}

// res.json({ success:true, data })
HttpResponsePtr okData(Json::Value data) {
  Json::Value body(Json::objectValue);
  body["success"] = true;
  body["data"] = std::move(data);
  return pulse::http::json(drogon::k200OK, std::move(body));
}

// res.json({ success:true, message })
HttpResponsePtr okMessage(const std::string& message) {
  Json::Value body(Json::objectValue);
  body["success"] = true;
  body["message"] = message;
  return pulse::http::json(drogon::k200OK, std::move(body));
}

// ---------------------------------------------------------------------------
// escapeRegex — file-local port of src/utils/escapeRegex.js:
//   String(str).replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
// ---------------------------------------------------------------------------
std::string escapeRegex(const std::string& in) {
  static const std::string specials = ".*+?^${}()|[]\\";
  std::string out;
  out.reserve(in.size() * 2);
  for (char c : in) {
    if (specials.find(c) != std::string::npos) out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

// trim() equivalent for the search query (JS String.prototype.trim removes
// leading/trailing whitespace incl. \t\n\r\f\v and space).
std::string trim(const std::string& s) {
  const char* ws = " \t\n\r\f\v";
  const auto b = s.find_first_not_of(ws);
  if (b == std::string::npos) return "";
  const auto e = s.find_last_not_of(ws);
  return s.substr(b, e - b + 1);
}

bsoncxx::types::b_date nowDate() {
  return bsoncxx::types::b_date{std::chrono::system_clock::now()};
}

// Parse an ISO-8601 UTC timestamp (the shape bsonjson::toJson emits for BSON
// dates) into a BSON b_date for the `before` cursor in getMessages, mirroring
// JS `new Date(before)`. Unparseable input falls back to "now".
bsoncxx::types::b_date parseIsoDate(const std::string& v) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0;
  if (std::sscanf(v.c_str(), "%d-%d-%dT%d:%d:%d.%dZ",
                  &y, &mo, &d, &h, &mi, &s, &ms) >= 6) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon  = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min  = mi;
    tm.tm_sec  = s;
#if defined(_WIN32)
    std::time_t t = _mkgmtime(&tm);
#else
    std::time_t t = timegm(&tm);
#endif
    long long millis = static_cast<long long>(t) * 1000 + ms;
    return bsoncxx::types::b_date{std::chrono::milliseconds{millis}};
  }
  return nowDate();
}

// JS parseInt-style integer parse (leading numeric prefix, NaN-on-failure).
// Returns std::nullopt when no leading integer is present (mirrors NaN).
std::optional<long long> jsParseInt(const std::string& s) {
  std::size_t i = 0;
  while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
  std::size_t start = i;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
  std::size_t digitsStart = i;
  while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
  if (i == digitsStart) return std::nullopt;
  try {
    return std::stoll(s.substr(start, i - start));
  } catch (...) {
    return std::nullopt;
  }
}

// User projection used by every .populate('participants'/'sender'/...) call.
// Mongoose select string: 'username name avatar profile.avatar isVerified
// isOnline' (and the lighter variants for admins / createdBy / replyTo). _id is
// always included by Mongoose. Fields absent on a given user simply do not appear.
bsoncxx::document::value userProjection(bool withName, bool withAvatar,
                                        bool withProfileAvatar, bool withVerified,
                                        bool withOnline) {
  bld::document proj;
  proj.append(kvp("username", 1));
  if (withName) proj.append(kvp("name", 1));
  if (withAvatar) proj.append(kvp("avatar", 1));
  if (withProfileAvatar) proj.append(kvp("profile.avatar", 1));
  if (withVerified) proj.append(kvp("isVerified", 1));
  if (withOnline) proj.append(kvp("isOnline", 1));
  return proj.extract();
}

// Fetch a single User by ObjectId with the given projection, returned as JSON
// (sanitized like Mongoose res.json: __v dropped). Returns the original hex id
// string if the user cannot be resolved (mirrors how an unpopulatable ref stays
// as its id under Mongoose). Returns null Json on invalid id.
Json::Value populateUser(const std::string& hexId,
                         const bsoncxx::document::value& projection) {
  auto oid = pulse::bsonjson::tryOid(hexId);
  if (!oid) return Json::Value(hexId);  // not a valid ref; leave as-is
  mongocxx::options::find opts;
  opts.projection(projection.view());
  auto col = pulse::db::collection(pulse::models::user::kCollection);
  auto res = col.find_one(make_document(kvp("_id", *oid)), opts);
  if (!res) return Json::Value(hexId);  // ref points to a missing user
  return pulse::models::user::sanitizeForOutput(
      pulse::bsonjson::toJson(res->view()));
}

// Replace an array field of ObjectId-ref hex strings with populated user docs.
void populateUserArray(Json::Value& doc, const char* field,
                       const bsoncxx::document::value& projection) {
  if (!doc.isMember(field) || !doc[field].isArray()) return;
  Json::Value out(Json::arrayValue);
  for (const auto& el : doc[field]) {
    if (el.isString()) {
      out.append(populateUser(el.asString(), projection));
    } else {
      out.append(el);  // already an object / unexpected shape — keep as-is
    }
  }
  doc[field] = out;
}

// Replace a single ObjectId-ref field with a populated user doc.
void populateUserField(Json::Value& doc, const char* field,
                       const bsoncxx::document::value& projection) {
  if (!doc.isMember(field) || !doc[field].isString()) return;
  doc[field] = populateUser(doc[field].asString(), projection);
}

// The full participant projection: username name avatar profile.avatar
// isVerified isOnline.
bsoncxx::document::value participantsProjection() {
  return userProjection(/*name*/true, /*avatar*/true, /*profileAvatar*/true,
                        /*verified*/true, /*online*/true);
}
// admins / createdBy projection: username name avatar profile.avatar.
bsoncxx::document::value adminProjection() {
  return userProjection(/*name*/true, /*avatar*/true, /*profileAvatar*/true,
                        /*verified*/false, /*online*/false);
}
// sender projection (messages): username name avatar profile.avatar isVerified.
bsoncxx::document::value senderProjection() {
  return userProjection(/*name*/true, /*avatar*/true, /*profileAvatar*/true,
                        /*verified*/true, /*online*/false);
}
// search-result admins projection: username only.
bsoncxx::document::value adminsUsernameProjection() {
  return make_document(kvp("username", 1));
}

// Apply the conversation-list populates (participants + admins + createdBy).
void populateConversationFull(Json::Value& conv) {
  populateUserArray(conv, "participants", participantsProjection());
  populateUserArray(conv, "admins", adminProjection());
  populateUserField(conv, "createdBy", adminProjection());
}

} // namespace

// ===========================================================================
// POST /api/v1/chat/conversation  — getOrCreateConversation
// ===========================================================================
void ChatController::getOrCreateConversation(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    const auto user = req->getAttributes()->get<Json::Value>("user");
    const std::string currentUserId = user["userId"].asString();

    auto bodyPtr = req->getJsonObject();
    std::string targetUserId;
    if (bodyPtr && bodyPtr->isMember("targetUserId") &&
        (*bodyPtr)["targetUserId"].isString()) {
      targetUserId = (*bodyPtr)["targetUserId"].asString();
    }

    // if (!targetUserId) -> 400 'Target user required'
    if (targetUserId.empty()) {
      callback(failMessage(drogon::k400BadRequest, "Target user required"));
      return;
    }

    const auto currentOid = pulse::bsonjson::oid(currentUserId);
    const auto targetOid  = pulse::bsonjson::oid(targetUserId);

    auto col = pulse::db::collection(pulse::models::conversation::kCollection);

    // 1. Try to find existing DM:
    //    findOne({ type:'direct', participants:{ $all:[cur,tgt], $size:2 } })
    auto existing = col.find_one(make_document(
        kvp("type", pulse::models::conversation::kTypeDirect),
        kvp("participants",
            make_document(kvp("$all", make_array(currentOid, targetOid)),
                          kvp("$size", 2)))));

    Json::Value conversation;

    if (existing) {
      conversation = pulse::models::conversation::sanitizeForOutput(
          pulse::bsonjson::toJson(existing->view()));
    } else {
      // 2. Create if not exists:
      //    Conversation.create({ type:'direct',
      //      participants:[cur,tgt],
      //      unreadCounts:{ [cur]:0, [tgt]:0 } })
      // The remaining schema defaults (type/groupAvatar/lastMessageContent/
      // lastMessageAt/__v + timestamps) are stamped on the insert doc below,
      // mirroring conversation::applyDefaults.

      // unreadCounts map keyed by the string user ids, both 0.
      bld::document unread;
      unread.append(kvp(currentUserId, 0));
      if (targetUserId != currentUserId) unread.append(kvp(targetUserId, 0));

      const bsoncxx::types::b_date now = nowDate();
      auto insertDoc = make_document(
          kvp("type", pulse::models::conversation::kTypeDirect),
          kvp("participants", make_array(currentOid, targetOid)),
          kvp("unreadCounts", unread.extract()),
          kvp("groupAvatar", bsoncxx::types::b_null{}),
          kvp("lastMessageContent",
              pulse::models::conversation::kDefaultLastMessageContent),
          kvp("lastMessageAt", now),
          kvp("createdAt", now),
          kvp("updatedAt", now),
          kvp("__v", 0));

      auto insertRes = col.insert_one(insertDoc.view());

      // Populate immediately:
      //   Conversation.findById(conv._id).populate('participants', ...)
      if (insertRes) {
        auto created = col.find_one(make_document(
            kvp("_id", insertRes->inserted_id().get_oid().value)));
        if (created) {
          conversation = pulse::models::conversation::sanitizeForOutput(
              pulse::bsonjson::toJson(created->view()));
        }
      }
    }

    // .populate('participants', 'username name avatar profile.avatar isVerified isOnline')
    if (conversation.isObject()) {
      populateUserArray(conversation, "participants", participantsProjection());
    }

    callback(okData(conversation));
  } catch (const std::exception& e) {
    pulse::log::error("\xE2\x9D\x8C Create conversation error: {}", e.what());
    callback(failMessage(drogon::k500InternalServerError, e.what()));
  }
}

// ===========================================================================
// GET /api/v1/chat/conversations  — getConversations
// ===========================================================================
void ChatController::getConversations(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    const auto user = req->getAttributes()->get<Json::Value>("user");
    const std::string userId = user["userId"].asString();

    // page = Math.max(parseInt(page) || 1, 1)
    long long page = 1;
    {
      auto p = jsParseInt(req->getParameter("page"));
      long long v = (p && *p != 0) ? *p : 1;  // parseInt||1 => 0 falls back to 1
      page = std::max<long long>(v, 1);
    }
    // limit = Math.min(Math.max(parseInt(limit) || 50, 1), 100)
    long long limit = 50;
    {
      auto p = jsParseInt(req->getParameter("limit"));
      long long v = (p && *p != 0) ? *p : 50;
      limit = std::min<long long>(std::max<long long>(v, 1), 100);
    }

    auto col = pulse::db::collection(pulse::models::conversation::kCollection);

    mongocxx::options::find opts;
    opts.sort(make_document(kvp("lastMessageAt", -1)));
    opts.skip((page - 1) * limit);
    opts.limit(limit);

    auto cursor = col.find(
        make_document(kvp("participants", pulse::bsonjson::oid(userId))), opts);

    Json::Value data(Json::arrayValue);
    long long count = 0;
    for (auto&& doc : cursor) {
      Json::Value conv = pulse::models::conversation::sanitizeForOutput(
          pulse::bsonjson::toJson(doc));
      populateConversationFull(conv);
      data.append(std::move(conv));
      ++count;
    }

    Json::Value body(Json::objectValue);
    body["success"] = true;
    body["data"] = data;
    Json::Value pagination(Json::objectValue);
    pagination["page"] = static_cast<Json::Int64>(page);
    pagination["limit"] = static_cast<Json::Int64>(limit);
    pagination["hasMore"] = (count == limit);
    body["pagination"] = pagination;

    callback(pulse::http::json(drogon::k200OK, std::move(body)));
  } catch (const std::exception& e) {
    pulse::log::error("\xE2\x9D\x8C Get conversations error: {}", e.what());
    callback(failMessage(drogon::k500InternalServerError, e.what()));
  }
}

// ===========================================================================
// GET /api/v1/chat/:conversationId/messages  — getMessages
// ===========================================================================
void ChatController::getMessages(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    std::string conversationId) {
  try {
    const auto user = req->getAttributes()->get<Json::Value>("user");
    const std::string userId = user["userId"].asString();

    const std::string limitParam = req->getParameter("limit");  // default 50
    const std::string before = req->getParameter("before");

    auto convCol = pulse::db::collection(pulse::models::conversation::kCollection);

    // Security: findOne({ _id: conversationId, participants: userId })
    auto conversation = convCol.find_one(make_document(
        kvp("_id", pulse::bsonjson::oid(conversationId)),
        kvp("participants", pulse::bsonjson::oid(userId))));

    if (!conversation) {
      callback(failMessage(drogon::k403Forbidden, "Not authorized"));
      return;
    }

    // query = { conversation: conversationId, isDeleted: { $ne: true } }
    bld::document query;
    query.append(kvp("conversation", pulse::bsonjson::oid(conversationId)));
    query.append(kvp("isDeleted", make_document(kvp("$ne", true))));
    // if (before) query.createdAt = { $lt: new Date(before) }
    if (!before.empty()) {
      query.append(kvp("createdAt",
                       make_document(kvp("$lt", parseIsoDate(before)))));
    }

    // parseInt(limit) — default 50 when the query param is absent.
    long long limit = 50;
    if (!limitParam.empty()) {
      auto p = jsParseInt(limitParam);
      if (p) limit = *p;  // NaN -> Mongoose treats as no limit; JS passes NaN to
                          // .limit which Mongo ignores. We keep parsed value.
    }

    auto msgCol = pulse::db::collection(pulse::models::message::kCollection);

    mongocxx::options::find opts;
    opts.sort(make_document(kvp("createdAt", -1)));  // newest first
    opts.limit(limit);
    auto cursor = msgCol.find(query.extract(), opts);

    const auto senderProj = senderProjection();
    const auto replySenderProj = adminProjection();  // 'username name avatar profile.avatar'

    Json::Value data(Json::arrayValue);
    for (auto&& doc : cursor) {
      Json::Value msg = pulse::models::message::sanitizeForOutput(
          pulse::bsonjson::toJson(doc));

      // .populate('sender', 'username name avatar profile.avatar isVerified')
      populateUserField(msg, "sender", senderProj);

      // .populate({ path:'replyTo', select:'content sender type media',
      //             populate:{ path:'sender',
      //                        select:'username name avatar profile.avatar' } })
      if (msg.isMember("replyTo") && msg["replyTo"].isString()) {
        auto replyOid = pulse::bsonjson::tryOid(msg["replyTo"].asString());
        if (replyOid) {
          mongocxx::options::find rOpts;
          rOpts.projection(make_document(kvp("content", 1), kvp("sender", 1),
                                         kvp("type", 1), kvp("media", 1)));
          auto reply = msgCol.find_one(make_document(kvp("_id", *replyOid)),
                                       rOpts);
          if (reply) {
            Json::Value replyJson = pulse::models::message::sanitizeForOutput(
                pulse::bsonjson::toJson(reply->view()));
            populateUserField(replyJson, "sender", replySenderProj);
            msg["replyTo"] = replyJson;
          }
          // if reply missing, leave the id in place (Mongoose: replyTo -> null,
          // but our ref stays as id; harmless for the client).
        }
      }

      data.append(std::move(msg));
    }

    callback(okData(data));
  } catch (const std::exception& e) {
    pulse::log::error("\xE2\x9D\x8C Get messages error: {}", e.what());
    callback(failMessage(drogon::k500InternalServerError, e.what()));
  }
}

// ===========================================================================
// POST /api/v1/chat/:conversationId/read  — markConversationRead
// ===========================================================================
void ChatController::markConversationRead(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    std::string conversationId) {
  try {
    const auto user = req->getAttributes()->get<Json::Value>("user");
    const std::string userId = user["userId"].asString();

    auto col = pulse::db::collection(pulse::models::conversation::kCollection);

    // findOneAndUpdate({ _id, participants:userId },
    //                  { $set: { ['unreadCounts.'+userId]: 0 } })
    // (no { new:true } => returns the pre-update doc; only truthiness matters.)
    const std::string field = "unreadCounts." + userId;
    auto updated = col.find_one_and_update(
        make_document(kvp("_id", pulse::bsonjson::oid(conversationId)),
                      kvp("participants", pulse::bsonjson::oid(userId))),
        make_document(kvp("$set", make_document(kvp(field, 0)))));

    if (!updated) {
      callback(failMessage(drogon::k403Forbidden, "Not authorized"));
      return;
    }

    callback(okMessage("Conversation marked as read"));
  } catch (const std::exception& e) {
    pulse::log::error("\xE2\x9D\x8C Mark read error: {}", e.what());
    callback(failMessage(drogon::k500InternalServerError, e.what()));
  }
}

// ===========================================================================
// DELETE /api/v1/chat/messages/:messageId  — deleteMessage
// ===========================================================================
void ChatController::deleteMessage(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    std::string messageId) {
  try {
    const auto user = req->getAttributes()->get<Json::Value>("user");
    const std::string userId = user["userId"].asString();

    auto col = pulse::db::collection(pulse::models::message::kCollection);

    // findOne({ _id: messageId, sender: userId })
    auto message = col.find_one(make_document(
        kvp("_id", pulse::bsonjson::oid(messageId)),
        kvp("sender", pulse::bsonjson::oid(userId))));

    if (!message) {
      callback(failMessage(drogon::k404NotFound,
                           "Message not found or unauthorized"));
      return;
    }

    // Soft delete:
    //   findByIdAndUpdate(messageId,
    //     { isDeleted:true, content:'This message was deleted' })
    col.update_one(
        make_document(kvp("_id", pulse::bsonjson::oid(messageId))),
        make_document(kvp("$set",
            make_document(kvp("isDeleted", true),
                          kvp("content", "This message was deleted")))));

    callback(okMessage("Message deleted"));
  } catch (const std::exception& e) {
    pulse::log::error("\xE2\x9D\x8C Delete message error: {}", e.what());
    callback(failMessage(drogon::k500InternalServerError, e.what()));
  }
}

// ===========================================================================
// GET /api/v1/chat/:conversationId  — getConversationDetails
// ===========================================================================
void ChatController::getConversationDetails(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    std::string conversationId) {
  try {
    const auto user = req->getAttributes()->get<Json::Value>("user");
    const std::string userId = user["userId"].asString();

    auto col = pulse::db::collection(pulse::models::conversation::kCollection);

    // findOne({ _id: conversationId, participants: userId })
    auto conversation = col.find_one(make_document(
        kvp("_id", pulse::bsonjson::oid(conversationId)),
        kvp("participants", pulse::bsonjson::oid(userId))));

    if (!conversation) {
      callback(failMessage(drogon::k404NotFound, "Conversation not found"));
      return;
    }

    Json::Value conv = pulse::models::conversation::sanitizeForOutput(
        pulse::bsonjson::toJson(conversation->view()));

    // .populate('participants', 'username name avatar profile.avatar isVerified isOnline')
    // .populate('admins', 'username name avatar profile.avatar')
    // .populate('createdBy', 'username name avatar profile.avatar')
    populateConversationFull(conv);

    callback(okData(conv));
  } catch (const std::exception& e) {
    pulse::log::error("\xE2\x9D\x8C Get conversation details error: {}", e.what());
    callback(failMessage(drogon::k500InternalServerError, e.what()));
  }
}

// ===========================================================================
// GET /api/v1/chat/search  — searchConversations
// ===========================================================================
void ChatController::searchConversations(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {
  try {
    const auto user = req->getAttributes()->get<Json::Value>("user");
    const std::string userId = user["userId"].asString();

    const std::string q = req->getParameter("q");

    // if (!q || q.trim().length === 0) return res.json({ success:true, data:[] })
    const std::string trimmed = trim(q);
    if (q.empty() || trimmed.empty()) {
      callback(okData(Json::Value(Json::arrayValue)));
      return;
    }

    auto col = pulse::db::collection(pulse::models::conversation::kCollection);

    // find({ participants:userId,
    //        $or:[ { groupName: { $regex: escapeRegex(q.trim()), $options:'i' } } ] })
    auto query = make_document(
        kvp("participants", pulse::bsonjson::oid(userId)),
        kvp("$or", make_array(make_document(kvp("groupName",
                 make_document(kvp("$regex", escapeRegex(trimmed)),
                               kvp("$options", "i")))))));

    mongocxx::options::find opts;
    opts.sort(make_document(kvp("lastMessageAt", -1)));
    opts.limit(20);
    auto cursor = col.find(query.view(), opts);

    const auto participantsProj = senderProjection();  // 'username name avatar profile.avatar isVerified'
    const auto adminsProj = adminsUsernameProjection();  // 'username'

    Json::Value data(Json::arrayValue);
    for (auto&& doc : cursor) {
      Json::Value conv = pulse::models::conversation::sanitizeForOutput(
          pulse::bsonjson::toJson(doc));
      // .populate('participants', 'username name avatar profile.avatar isVerified')
      populateUserArray(conv, "participants", participantsProj);
      // .populate('admins', 'username')
      populateUserArray(conv, "admins", adminsProj);
      data.append(std::move(conv));
    }

    callback(okData(data));
  } catch (const std::exception& e) {
    pulse::log::error("\xE2\x9D\x8C Search conversations error: {}", e.what());
    callback(failMessage(drogon::k500InternalServerError, e.what()));
  }
}

} // namespace pulse::controllers
