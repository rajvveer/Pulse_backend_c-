// group_controller.cc — port of src/controllers/groupController.js
// (route group src/routes/groupRoutes.js, mounted at /api/v1/groups).
//
// 1:1 with the Express handlers. The JS leans on Mongoose `.populate()`, which
// has no model helper here, so the controller performs the populate inline:
// it resolves the participant / admin / createdBy ObjectId refs to the selected
// User sub-fields, matching the projection each route passed to .populate().
//
// Reference fields (participants, admins, createdBy, conversation, sender) are
// persisted as real BSON ObjectIds — pulse::bsonjson::fromJson does NOT coerce
// hex strings to oids, and the membership match queries (e.g.
// { participants: req.user.userId }) only work against true ObjectIds.
#include "pulse/controllers/group_controller.hpp"

#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/http_response.hpp"
#include "pulse/logger.hpp"
#include "pulse/models/conversation.hpp"
#include "pulse/models/message.hpp"
#include "pulse/models/user.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/oid.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/stdx/optional.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/cursor.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <unordered_set>
#include <vector>

namespace pulse::controllers {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;
using bld::make_array;

namespace {

// ── small helpers ───────────────────────────────────────────────────────────

// Read a possibly-missing string field ("" when absent/null/non-string).
std::string str(const Json::Value& v, const char* key) {
  return v.isObject() && v.isMember(key) && v[key].isString() ? v[key].asString() : "";
}

constexpr std::size_t kMaxGroupMembers = 100;
constexpr std::size_t kMaxGroupNameBytes = 100;
constexpr std::size_t kMaxGroupDescriptionBytes = 1000;
constexpr std::size_t kMaxGroupAvatarBytes = 2048;

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

// Every member reference must resolve to a currently active account. Silently
// dropping malformed/deleted IDs can create undersized groups and dangling
// authorization entries.
bool allActiveUsers(const std::vector<std::string>& ids) {
  if (ids.empty()) return true;
  bld::array oidArray;
  for (const auto& id : ids) {
    auto oid = pulse::bsonjson::tryOid(id);
    if (!oid) return false;
    oidArray.append(*oid);
  }
  auto users = pulse::db::collection(pulse::models::user::kCollection);
  const auto count = users.count_documents(make_document(
      kvp("_id", make_document(kvp("$in", oidArray.extract()))),
      kvp("isActive", true)));
  return count == static_cast<std::int64_t>(ids.size());
}

// The authenticated user stored on the request by AuthFilter:
//   Json::Value{ userId, username, email, isVerified }
Json::Value authUser(const drogon::HttpRequestPtr& req) {
  return req->getAttributes()->get<Json::Value>("user");
}

// 500 handler matching the Express catch:
//   res.status(500).json({ success: false, message: error.message })
drogon::HttpResponsePtr serverFail(const std::string& message) {
  Json::Value body(Json::objectValue);
  body["success"] = false;
  body["message"] = message;
  return pulse::http::json(drogon::k500InternalServerError, std::move(body));
}

// { success: false, message } with a chosen status (mirrors the many JS
// res.status(x).json({ success:false, message }) early returns).
drogon::HttpResponsePtr fail(drogon::HttpStatusCode code, const std::string& message) {
  Json::Value body(Json::objectValue);
  body["success"] = false;
  body["message"] = message;
  return pulse::http::json(code, std::move(body));
}

// res.json({ success: true, message }) — 200.
drogon::HttpResponsePtr okMessage(const std::string& message) {
  Json::Value body(Json::objectValue);
  body["success"] = true;
  body["message"] = message;
  return pulse::http::json(drogon::k200OK, std::move(body));
}

// res.status(code).json({ success: true, data }) — used by getGroupDetails (200)
// and createGroup (201).
drogon::HttpResponsePtr okData(Json::Value data, drogon::HttpStatusCode code = drogon::k200OK) {
  return pulse::http::ok(std::move(data), code);
}

// bsoncxx b_date for "now" — used for the BSON date fields that participate in
// range/TTL queries and sorting, matching the rest of the codebase (chat /
// realtime controllers store Mongoose dates as real b_date, not ISO strings).
bsoncxx::types::b_date nowDate() {
  return bsoncxx::types::b_date{std::chrono::system_clock::now()};
}

// ── populate ────────────────────────────────────────────────────────────────

// Build a User projection from a space-separated Mongoose select string
// (e.g. 'username name avatar profile.avatar isVerified isOnline'). _id is
// always included by Mongoose, so it is not listed.
bsoncxx::document::value userProjection(const std::vector<std::string>& fields) {
  bld::document proj;
  for (const auto& f : fields) proj.append(kvp(f, 1));
  return proj.extract();
}

// Fetch one User by ObjectId hex with the given projection, returned as JSON
// (sanitized like Mongoose res.json: __v / sensitive fields dropped). Mirrors
// chat_controller: an unresolvable ref (invalid id or missing user) is left as
// its original hex id string, matching how Mongoose leaves an unpopulated ref.
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
    if (el.isString()) out.append(populateUser(el.asString(), projection));
    else out.append(el);  // already an object / unexpected shape — keep as-is
  }
  doc[field] = out;
}

// Replace a single ObjectId-ref field with a populated user doc.
void populateUserField(Json::Value& doc, const char* field,
                       const bsoncxx::document::value& projection) {
  if (!doc.isMember(field) || !doc[field].isString()) return;
  doc[field] = populateUser(doc[field].asString(), projection);
}

// Fetch a conversation by _id and apply the populate projections used by the
// route. `adminFields` / `createdByFields` empty => that field is not populated
// (matches the routes that only .populate('participants')). Returns null Json if
// not found (mirrors Mongoose findById -> null).
Json::Value findByIdPopulated(const std::string& groupId,
                              const std::vector<std::string>& participantFields,
                              const std::vector<std::string>& adminFields,
                              const std::vector<std::string>& createdByFields) {
  auto oid = pulse::bsonjson::tryOid(groupId);
  if (!oid) return Json::Value(Json::nullValue);
  auto col = pulse::db::collection(pulse::models::conversation::kCollection);
  auto doc = col.find_one(make_document(kvp("_id", *oid)));
  if (!doc) return Json::Value(Json::nullValue);

  Json::Value group = pulse::models::conversation::sanitizeForOutput(
      pulse::bsonjson::toJson(doc->view()));

  populateUserArray(group, "participants", userProjection(participantFields));
  if (!adminFields.empty())
    populateUserArray(group, "admins", userProjection(adminFields));
  if (!createdByFields.empty())
    populateUserField(group, "createdBy", userProjection(createdByFields));

  return group;
}

// Insert a system Message, mirroring Message.create({ conversation, sender,
// type:'system', content }). conversation + sender are stored as ObjectIds;
// createdAt/updatedAt are real BSON dates and the schema defaults (reactions {},
// readBy [], isDeleted false) are applied, matching the realtime message insert.
void createSystemMessage(const std::string& conversationId,
                         const std::string& senderId,
                         const std::string& content) {
  auto convOid = pulse::bsonjson::tryOid(conversationId);
  auto senderOid = pulse::bsonjson::tryOid(senderId);
  if (!convOid || !senderOid) return;  // required refs; nothing to insert

  bld::document doc;
  doc.append(kvp("conversation", *convOid));
  doc.append(kvp("sender", *senderOid));
  doc.append(kvp("type", "system"));
  doc.append(kvp("content", content));
  doc.append(kvp("reactions", make_document()));  // default {}
  doc.append(kvp("readBy", make_array()));         // [] (schema array)
  doc.append(kvp("isDeleted", false));             // default false
  const bsoncxx::types::b_date now = nowDate();
  doc.append(kvp("createdAt", now));
  doc.append(kvp("updatedAt", now));

  pulse::db::collection(pulse::models::message::kCollection).insert_one(doc.extract());
}

// Look up usernames for a set of ids, in the SAME order as `ids` (matches the
// JS `User.find({_id:{$in}}).select('username')` -> `.map(u => u.username)`).
// Note: Mongo returns docs in arbitrary order; the JS join order is whatever the
// driver returned, so we simply join the usernames it found.
std::vector<std::string> usernamesFor(const std::vector<std::string>& ids) {
  std::vector<std::string> out;
  bld::array oids;
  bool any = false;
  for (const auto& id : ids) {
    if (auto o = pulse::bsonjson::tryOid(id)) { oids.append(*o); any = true; }
  }
  if (!any) return out;
  auto col = pulse::db::collection(pulse::models::user::kCollection);
  mongocxx::options::find opts;
  opts.projection(make_document(kvp("username", 1)));
  auto cursor = col.find(
      make_document(kvp("_id", make_document(kvp("$in", oids.extract())))), opts);
  for (auto&& doc : cursor) {
    Json::Value u = pulse::bsonjson::toJson(doc);
    out.push_back(str(u, "username"));
  }
  return out;
}

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) out += sep;
    out += parts[i];
  }
  return out;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// POST /api/v1/groups — create a group
// ─────────────────────────────────────────────────────────────────────────────
void GroupController::createGroup(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  try {
    auto bodyPtr = req->getJsonObject();
    Json::Value body = bodyPtr ? *bodyPtr : Json::Value(Json::objectValue);

    const std::string groupName = trim(str(body, "groupName"));
    const Json::Value& participants = body["participants"];
    Json::Value user = authUser(req);
    const std::string creatorId = str(user, "userId");

    if (groupName.empty() || groupName.size() > kMaxGroupNameBytes ||
        !participants.isArray() || participants.size() < 2 ||
        participants.size() > kMaxGroupMembers - 1 ||
        !pulse::bsonjson::isValidOid(creatorId)) {
      return callback(fail(drogon::k400BadRequest,
                           "Invalid group name or participants"));
    }
    if ((body.isMember("groupDescription") &&
         !body["groupDescription"].isNull() &&
         (!body["groupDescription"].isString() ||
          body["groupDescription"].asString().size() >
              kMaxGroupDescriptionBytes)) ||
        (body.isMember("groupAvatar") && !body["groupAvatar"].isNull() &&
         (!body["groupAvatar"].isString() ||
          body["groupAvatar"].asString().size() > kMaxGroupAvatarBytes))) {
      return callback(fail(drogon::k400BadRequest,
                           "Invalid group description or avatar"));
    }

    // allParticipants = [...new Set([creatorId, ...participants])]
    std::vector<std::string> allParticipants;
    std::unordered_set<std::string> seen;
    auto addUnique = [&](const std::string& id) {
      if (seen.insert(id).second) allParticipants.push_back(id);
    };
    addUnique(creatorId);
    for (const auto& p : participants) {
      if (!p.isString() || !pulse::bsonjson::isValidOid(p.asString())) {
        return callback(fail(drogon::k400BadRequest,
                             "Participant IDs must be valid user IDs"));
      }
      addUnique(p.asString());
    }
    if (allParticipants.size() < 3 ||
        allParticipants.size() > kMaxGroupMembers) {
      return callback(fail(drogon::k400BadRequest,
                           "At least 2 distinct participants are required"));
    }
    std::vector<std::string> invited(allParticipants.begin() + 1,
                                     allParticipants.end());
    if (!allActiveUsers(invited)) {
      return callback(fail(drogon::k400BadRequest,
                           "One or more participants are unavailable"));
    }

    // unreadCounts: { [id]: 0 } for every participant.
    bld::document unreadCounts;
    for (const auto& id : allParticipants) unreadCounts.append(kvp(id, 0));

    // groupDescription / groupAvatar pass through as given (may be undefined).
    const bool hasDesc = body.isMember("groupDescription") && !body["groupDescription"].isNull();
    const bool hasAvatar = body.isMember("groupAvatar") && !body["groupAvatar"].isNull();

    const std::string username = str(user, "username");
    const std::string lastMessageContent =
        (username.empty() ? std::string("Someone") : username) + " created the group";

    const bsoncxx::types::b_date now = nowDate();

    // Build the conversation document with ObjectId refs.
    bld::array participantOids;
    for (const auto& id : allParticipants) {
      if (auto o = pulse::bsonjson::tryOid(id)) participantOids.append(*o);
    }
    bld::array adminOids;
    if (auto o = pulse::bsonjson::tryOid(creatorId)) adminOids.append(*o);

    bld::document conv;
    conv.append(kvp("type", pulse::models::conversation::kTypeGroup));
    conv.append(kvp("groupName", groupName));
    if (hasDesc) {
      conv.append(kvp("groupDescription", std::string(body["groupDescription"].asString())));
    }
    if (hasAvatar) {
      conv.append(kvp("groupAvatar", std::string(body["groupAvatar"].asString())));
    } else {
      conv.append(kvp("groupAvatar", bsoncxx::types::b_null{}));  // schema default null
    }
    conv.append(kvp("participants", participantOids.extract()));
    conv.append(kvp("admins", adminOids.extract()));
    if (auto o = pulse::bsonjson::tryOid(creatorId)) conv.append(kvp("createdBy", *o));
    conv.append(kvp("lastMessageContent", lastMessageContent));
    conv.append(kvp("lastMessageAt", now));
    conv.append(kvp("unreadCounts", unreadCounts.extract()));
    conv.append(kvp("createdAt", now));
    conv.append(kvp("updatedAt", now));
    conv.append(kvp("__v", 0));

    auto col = pulse::db::collection(pulse::models::conversation::kCollection);
    auto res = col.insert_one(conv.extract());
    if (!res) return callback(serverFail("Failed to create group"));
    std::string groupId = res->inserted_id().get_oid().value.to_string();

    // System message: `${req.user.username} created "${groupName}"`
    createSystemMessage(groupId, creatorId, username + " created \"" + groupName + "\"");

    // Populate and return:
    //   participants: 'username name avatar profile.avatar isVerified'
    //   admins:       'username name avatar profile.avatar'
    //   createdBy:    'username name avatar profile.avatar'
    Json::Value populated = findByIdPopulated(
        groupId,
        {"username", "name", "avatar", "profile.avatar", "isVerified"},
        {"username", "name", "avatar", "profile.avatar"},
        {"username", "name", "avatar", "profile.avatar"});

    return callback(okData(std::move(populated), drogon::k201Created));
  } catch (const std::exception& e) {
    pulse::log::error("Create group error: {}", e.what());
    return callback(serverFail(e.what()));
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// GET /api/v1/groups/{groupId} — group details
// ─────────────────────────────────────────────────────────────────────────────
void GroupController::getGroupDetails(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string groupId) {
  try {
    Json::Value user = authUser(req);
    const std::string userId = str(user, "userId");

    auto gOid = pulse::bsonjson::tryOid(groupId);
    auto uOid = pulse::bsonjson::tryOid(userId);
    auto col = pulse::db::collection(pulse::models::conversation::kCollection);

    bsoncxx::stdx::optional<bsoncxx::document::value> doc;
    if (gOid && uOid) {
      doc = col.find_one(make_document(
          kvp("_id", *gOid),
          kvp("type", pulse::models::conversation::kTypeGroup),
          kvp("participants", *uOid)));
    }

    if (!doc) {
      return callback(fail(drogon::k404NotFound, "Group not found"));
    }

    // Populate the same authorized snapshot. Re-fetching by ID alone after the
    // membership check could disclose the group if removal raced this request.
    Json::Value group = pulse::models::conversation::sanitizeForOutput(
        pulse::bsonjson::toJson(doc->view()));
    populateUserArray(
        group, "participants",
        userProjection({"username", "name", "avatar", "profile.avatar",
                        "isVerified", "isOnline"}));
    populateUserArray(
        group, "admins",
        userProjection({"username", "name", "avatar", "profile.avatar"}));
    populateUserField(
        group, "createdBy",
        userProjection({"username", "name", "avatar", "profile.avatar"}));

    return callback(okData(std::move(group)));
  } catch (const std::exception& e) {
    pulse::log::error("Get group error: {}", e.what());
    return callback(serverFail(e.what()));
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// POST /api/v1/groups/{groupId}/members — add members
// ─────────────────────────────────────────────────────────────────────────────
void GroupController::addGroupMembers(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string groupId) {
  try {
    auto bodyPtr = req->getJsonObject();
    Json::Value body = bodyPtr ? *bodyPtr : Json::Value(Json::objectValue);
    const Json::Value& userIds = body["userIds"];

    // !userIds || !Array.isArray(userIds) || userIds.length === 0
    if (!userIds.isArray() || userIds.empty() ||
        userIds.size() > kMaxGroupMembers) {
      return callback(fail(drogon::k400BadRequest, "User IDs required"));
    }

    Json::Value user = authUser(req);
    const std::string requesterId = str(user, "userId");
    const std::string requesterName = str(user, "username");

    auto gOid = pulse::bsonjson::tryOid(groupId);
    auto rOid = pulse::bsonjson::tryOid(requesterId);
    if (!gOid || !rOid)
      return callback(fail(drogon::k400BadRequest, "Invalid group ID"));
    auto col = pulse::db::collection(pulse::models::conversation::kCollection);

    // Admin check: findOne({ _id, type:'group', admins: req.user.userId })
    bsoncxx::stdx::optional<bsoncxx::document::value> groupDoc;
    if (gOid && rOid) {
      groupDoc = col.find_one(make_document(
          kvp("_id", *gOid),
          kvp("type", pulse::models::conversation::kTypeGroup),
          kvp("admins", *rOid)));
    }
    if (!groupDoc) {
      return callback(fail(drogon::k403Forbidden, "Not authorized or group not found"));
    }

    Json::Value group = pulse::bsonjson::toJson(groupDoc->view());

    // Existing participant id set (hex strings).
    std::unordered_set<std::string> existing;
    if (group.isMember("participants") && group["participants"].isArray()) {
      for (const auto& p : group["participants"]) {
        if (p.isString()) existing.insert(p.asString());
      }
    }

    // newMembers = userIds.filter(id => !group.participants.includes(id))
    std::vector<std::string> newMembers;
    std::unordered_set<std::string> requested;
    for (const auto& id : userIds) {
      if (!id.isString() || !pulse::bsonjson::isValidOid(id.asString()))
        return callback(fail(drogon::k400BadRequest,
                             "User IDs must be valid"));
      if (existing.find(id.asString()) == existing.end() &&
          requested.insert(id.asString()).second)
        newMembers.push_back(id.asString());
    }

    if (newMembers.empty()) {
      return callback(fail(drogon::k400BadRequest, "All users are already members"));
    }
    if (existing.size() + newMembers.size() > kMaxGroupMembers) {
      return callback(fail(drogon::k400BadRequest,
                           "Group member limit exceeded"));
    }
    if (!allActiveUsers(newMembers)) {
      return callback(fail(drogon::k400BadRequest,
                           "One or more users are unavailable"));
    }

    // $addToSet: { participants: { $each: newMembers } }
    bld::array eachOids;
    for (const auto& id : newMembers) {
      if (auto o = pulse::bsonjson::tryOid(id)) eachOids.append(*o);
    }
    // Add members and initialize unread counts in one authorized update so an
    // admin removal cannot split the two writes.
    bld::document unreadSet;
    for (const auto& id : newMembers) unreadSet.append(kvp("unreadCounts." + id, 0));
    auto updated = col.update_one(
        make_document(kvp("_id", *gOid),
                      kvp("type", pulse::models::conversation::kTypeGroup),
                      kvp("admins", *rOid)),
        make_document(
            kvp("$addToSet", make_document(kvp(
                "participants",
                make_document(kvp("$each", eachOids.extract()))))),
            kvp("$set", unreadSet.extract())));
    if (!updated || updated->matched_count() != 1)
      return callback(fail(drogon::k409Conflict,
                           "Group membership changed; try again"));

    // System message: `${req.user.username} added ${usernames}`
    std::vector<std::string> usernames = usernamesFor(newMembers);
    createSystemMessage(groupId, requesterId,
                        requesterName + " added " + join(usernames, ", "));

    // Return populated group (participants only:
    //   'username name avatar profile.avatar isVerified').
    Json::Value populated = findByIdPopulated(
        groupId,
        {"username", "name", "avatar", "profile.avatar", "isVerified"},
        {}, {});

    return callback(okData(std::move(populated)));
  } catch (const std::exception& e) {
    pulse::log::error("Add members error: {}", e.what());
    return callback(serverFail(e.what()));
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// DELETE /api/v1/groups/{groupId}/members/{userId} — remove member
// ─────────────────────────────────────────────────────────────────────────────
void GroupController::removeGroupMember(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string groupId, std::string userId) {
  try {
    Json::Value user = authUser(req);
    const std::string requesterId = str(user, "userId");
    const std::string requesterName = str(user, "username");

    auto gOid = pulse::bsonjson::tryOid(groupId);
    auto rOid = pulse::bsonjson::tryOid(requesterId);
    auto pullOid = pulse::bsonjson::tryOid(userId);
    if (!gOid || !rOid || !pullOid)
      return callback(fail(drogon::k400BadRequest, "Invalid user or group ID"));
    auto col = pulse::db::collection(pulse::models::conversation::kCollection);

    bsoncxx::stdx::optional<bsoncxx::document::value> groupDoc;
    if (gOid && rOid) {
      groupDoc = col.find_one(make_document(
          kvp("_id", *gOid),
          kvp("type", pulse::models::conversation::kTypeGroup),
          kvp("admins", *rOid)));
    }
    if (!groupDoc) {
      return callback(fail(drogon::k403Forbidden, "Not authorized"));
    }

    Json::Value group = pulse::bsonjson::toJson(groupDoc->view());

    // Can't remove creator: String(group.createdBy) === String(userId)
    if (str(group, "createdBy") == userId) {
      return callback(fail(drogon::k400BadRequest, "Cannot remove group creator"));
    }

    // Keep authorization and membership in the mutation predicate so a stale
    // pre-check cannot remove a user after the requester loses admin access.
    auto removed = col.update_one(
        make_document(
            kvp("_id", *gOid), kvp("admins", *rOid),
            kvp("participants", *pullOid),
            kvp("createdBy", make_document(kvp("$ne", *pullOid)))),
        make_document(
            kvp("$pull", make_document(kvp("participants", *pullOid),
                                         kvp("admins", *pullOid))),
            kvp("$unset", make_document(
                kvp("unreadCounts." + userId, ""))),
            kvp("$set", make_document(kvp("updatedAt", nowDate())))));
    if (!removed || removed->modified_count() != 1)
      return callback(fail(drogon::k409Conflict,
                           "Member or group authorization changed"));

    // System message: `${req.user.username} removed ${removedUser.username}`
    std::vector<std::string> names = usernamesFor({userId});
    std::string removedName = names.empty() ? std::string() : names[0];
    createSystemMessage(groupId, requesterId, requesterName + " removed " + removedName);

    return callback(okMessage("Member removed"));
  } catch (const std::exception& e) {
    pulse::log::error("Remove member error: {}", e.what());
    return callback(serverFail(e.what()));
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// POST /api/v1/groups/{groupId}/leave — leave group
// ─────────────────────────────────────────────────────────────────────────────
void GroupController::leaveGroup(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string groupId) {
  try {
    Json::Value user = authUser(req);
    const std::string userId = str(user, "userId");
    const std::string username = str(user, "username");

    auto gOid = pulse::bsonjson::tryOid(groupId);
    auto uOid = pulse::bsonjson::tryOid(userId);
    if (!gOid || !uOid)
      return callback(fail(drogon::k400BadRequest, "Invalid group ID"));
    auto col = pulse::db::collection(pulse::models::conversation::kCollection);

    bsoncxx::stdx::optional<bsoncxx::document::value> groupDoc;
    if (gOid && uOid) {
      groupDoc = col.find_one(make_document(
          kvp("_id", *gOid),
          kvp("type", pulse::models::conversation::kTypeGroup),
          kvp("participants", *uOid)));
    }
    if (!groupDoc) {
      return callback(fail(drogon::k404NotFound, "Group not found"));
    }

    Json::Value group = pulse::bsonjson::toJson(groupDoc->view());

    // Creator can't leave: String(group.createdBy) === String(userId)
    if (str(group, "createdBy") == userId) {
      return callback(fail(drogon::k400BadRequest,
                           "Group creator cannot leave. Delete the group instead."));
    }

    auto left = col.update_one(
        make_document(
            kvp("_id", *gOid), kvp("participants", *uOid),
            kvp("createdBy", make_document(kvp("$ne", *uOid)))),
        make_document(
            kvp("$pull", make_document(kvp("participants", *uOid),
                                         kvp("admins", *uOid))),
            kvp("$unset", make_document(
                kvp("unreadCounts." + userId, ""))),
            kvp("$set", make_document(kvp("updatedAt", nowDate())))));
    if (!left || left->modified_count() != 1)
      return callback(fail(drogon::k409Conflict,
                           "Group membership changed; try again"));

    // System message: `${req.user.username} left the group`
    createSystemMessage(groupId, userId, username + " left the group");

    return callback(okMessage("Left group successfully"));
  } catch (const std::exception& e) {
    pulse::log::error("Leave group error: {}", e.what());
    return callback(serverFail(e.what()));
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// PUT /api/v1/groups/{groupId} — update group info
// ─────────────────────────────────────────────────────────────────────────────
void GroupController::updateGroupInfo(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string groupId) {
  try {
    auto bodyPtr = req->getJsonObject();
    Json::Value body = bodyPtr ? *bodyPtr : Json::Value(Json::objectValue);

    Json::Value user = authUser(req);
    const std::string requesterId = str(user, "userId");
    const std::string requesterName = str(user, "username");

    auto gOid = pulse::bsonjson::tryOid(groupId);
    auto rOid = pulse::bsonjson::tryOid(requesterId);
    auto col = pulse::db::collection(pulse::models::conversation::kCollection);

    bsoncxx::stdx::optional<bsoncxx::document::value> groupDoc;
    if (gOid && rOid) {
      groupDoc = col.find_one(make_document(
          kvp("_id", *gOid),
          kvp("type", pulse::models::conversation::kTypeGroup),
          kvp("admins", *rOid)));
    }
    if (!groupDoc) {
      return callback(fail(drogon::k403Forbidden, "Not authorized"));
    }

    Json::Value group = pulse::bsonjson::toJson(groupDoc->view());
    const std::string previousName = str(group, "groupName");

    // Build $set:
    //   if (groupName) updates.groupName = groupName  (truthy)
    //   if (groupDescription !== undefined) updates.groupDescription = ...
    //   if (groupAvatar !== undefined) updates.groupAvatar = ...
    const std::string groupName = trim(str(body, "groupName"));
    const bool nameProvided = !groupName.empty();
    const bool descProvided = body.isMember("groupDescription");
    const bool avatarProvided = body.isMember("groupAvatar");

    if ((body.isMember("groupName") &&
         (!body["groupName"].isString() || groupName.empty() ||
          groupName.size() > kMaxGroupNameBytes)) ||
        (descProvided && !body["groupDescription"].isNull() &&
         (!body["groupDescription"].isString() ||
          body["groupDescription"].asString().size() >
              kMaxGroupDescriptionBytes)) ||
        (avatarProvided && !body["groupAvatar"].isNull() &&
         (!body["groupAvatar"].isString() ||
          body["groupAvatar"].asString().size() > kMaxGroupAvatarBytes))) {
      return callback(fail(drogon::k400BadRequest,
                           "Invalid group update"));
    }

    bld::document updates;
    bool hasUpdate = false;
    if (nameProvided) { updates.append(kvp("groupName", groupName)); hasUpdate = true; }
    if (descProvided) {
      const Json::Value& d = body["groupDescription"];
      if (d.isNull()) updates.append(kvp("groupDescription", bsoncxx::types::b_null{}));
      else updates.append(kvp("groupDescription", std::string(d.asString())));
      hasUpdate = true;
    }
    if (avatarProvided) {
      const Json::Value& a = body["groupAvatar"];
      if (a.isNull()) updates.append(kvp("groupAvatar", bsoncxx::types::b_null{}));
      else updates.append(kvp("groupAvatar", std::string(a.asString())));
      hasUpdate = true;
    }

    if (hasUpdate) {
      updates.append(kvp("updatedAt", nowDate()));
      auto result = col.update_one(
          make_document(kvp("_id", *gOid),
                        kvp("type", pulse::models::conversation::kTypeGroup),
                        kvp("admins", *rOid)),
          make_document(kvp("$set", updates.extract())));
      if (!result || result->matched_count() != 1)
        return callback(fail(drogon::k409Conflict,
                             "Group authorization changed; try again"));
    }

    // System message for name change:
    //   if (groupName && groupName !== group.groupName)
    if (nameProvided && groupName != previousName) {
      createSystemMessage(groupId, requesterId,
                          requesterName + " changed the group name to \"" + groupName + "\"");
    }

    // Return populated group (participants only:
    //   'username name avatar profile.avatar isVerified').
    Json::Value populated = findByIdPopulated(
        groupId,
        {"username", "name", "avatar", "profile.avatar", "isVerified"},
        {}, {});

    return callback(okData(std::move(populated)));
  } catch (const std::exception& e) {
    pulse::log::error("Update group error: {}", e.what());
    return callback(serverFail(e.what()));
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// POST /api/v1/groups/{groupId}/admins — make admin
// ─────────────────────────────────────────────────────────────────────────────
void GroupController::makeAdmin(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string groupId) {
  try {
    auto bodyPtr = req->getJsonObject();
    Json::Value body = bodyPtr ? *bodyPtr : Json::Value(Json::objectValue);
    const std::string userId = str(body, "userId");

    Json::Value user = authUser(req);
    const std::string requesterId = str(user, "userId");

    auto gOid = pulse::bsonjson::tryOid(groupId);
    auto rOid = pulse::bsonjson::tryOid(requesterId);
    auto targetOid = pulse::bsonjson::tryOid(userId);
    if (!gOid || !rOid || !targetOid)
      return callback(fail(drogon::k400BadRequest, "Invalid user or group ID"));
    auto col = pulse::db::collection(pulse::models::conversation::kCollection);

    // Only creator can make admins: findOne({ _id, type:'group', createdBy: req.user.userId })
    bsoncxx::stdx::optional<bsoncxx::document::value> groupDoc;
    if (gOid && rOid) {
      groupDoc = col.find_one(make_document(
          kvp("_id", *gOid),
          kvp("type", pulse::models::conversation::kTypeGroup),
          kvp("createdBy", *rOid)));
    }
    if (!groupDoc) {
      return callback(fail(drogon::k403Forbidden, "Not authorized"));
    }

    Json::Value group = pulse::bsonjson::toJson(groupDoc->view());

    // group.participants.includes(userId)
    bool isMember = false;
    if (group.isMember("participants") && group["participants"].isArray()) {
      for (const auto& p : group["participants"]) {
        if (p.isString() && p.asString() == userId) { isMember = true; break; }
      }
    }
    if (!isMember) {
      return callback(fail(drogon::k400BadRequest, "User is not a group member"));
    }

    // $addToSet: { admins: userId }
    auto promoted = col.update_one(
        make_document(kvp("_id", *gOid), kvp("createdBy", *rOid),
                      kvp("participants", *targetOid),
                      kvp("admins", make_document(kvp("$ne", *targetOid)))),
        make_document(
            kvp("$addToSet", make_document(kvp("admins", *targetOid))),
            kvp("$set", make_document(kvp("updatedAt", nowDate())))));
    if (!promoted || promoted->modified_count() != 1)
      return callback(fail(drogon::k409Conflict,
                           "User is already an admin or membership changed"));

    // System message: `${user.username} is now an admin`
    std::vector<std::string> names = usernamesFor({userId});
    std::string targetName = names.empty() ? std::string() : names[0];
    createSystemMessage(groupId, requesterId, targetName + " is now an admin");

    return callback(okMessage("Admin added"));
  } catch (const std::exception& e) {
    pulse::log::error("Make admin error: {}", e.what());
    return callback(serverFail(e.what()));
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// DELETE /api/v1/groups/{groupId}/admins/{userId} — remove admin
// ─────────────────────────────────────────────────────────────────────────────
void GroupController::removeAdmin(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string groupId, std::string userId) {
  try {
    Json::Value user = authUser(req);
    const std::string requesterId = str(user, "userId");

    auto gOid = pulse::bsonjson::tryOid(groupId);
    auto rOid = pulse::bsonjson::tryOid(requesterId);
    auto targetOid = pulse::bsonjson::tryOid(userId);
    if (!gOid || !rOid || !targetOid)
      return callback(fail(drogon::k400BadRequest, "Invalid user or group ID"));
    auto col = pulse::db::collection(pulse::models::conversation::kCollection);

    bsoncxx::stdx::optional<bsoncxx::document::value> groupDoc;
    if (gOid && rOid) {
      groupDoc = col.find_one(make_document(
          kvp("_id", *gOid),
          kvp("type", pulse::models::conversation::kTypeGroup),
          kvp("createdBy", *rOid)));
    }
    if (!groupDoc) {
      return callback(fail(drogon::k403Forbidden, "Not authorized"));
    }

    Json::Value group = pulse::bsonjson::toJson(groupDoc->view());

    // Can't remove creator as admin: String(group.createdBy) === String(userId)
    if (str(group, "createdBy") == userId) {
      return callback(fail(drogon::k400BadRequest, "Cannot remove creator as admin"));
    }

    auto demoted = col.update_one(
        make_document(
            kvp("_id", *gOid), kvp("createdBy", *rOid),
            kvp("admins", *targetOid)),
        make_document(
            kvp("$pull", make_document(kvp("admins", *targetOid))),
            kvp("$set", make_document(kvp("updatedAt", nowDate())))));
    if (!demoted || demoted->modified_count() != 1)
      return callback(fail(drogon::k409Conflict,
                           "Admin state changed; try again"));

    // System message: `${user.username} is no longer an admin`
    std::vector<std::string> names = usernamesFor({userId});
    std::string targetName = names.empty() ? std::string() : names[0];
    createSystemMessage(groupId, requesterId, targetName + " is no longer an admin");

    return callback(okMessage("Admin removed"));
  } catch (const std::exception& e) {
    pulse::log::error("Remove admin error: {}", e.what());
    return callback(serverFail(e.what()));
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// DELETE /api/v1/groups/{groupId} — delete group
// ─────────────────────────────────────────────────────────────────────────────
void GroupController::deleteGroup(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string groupId) {
  try {
    Json::Value user = authUser(req);
    const std::string requesterId = str(user, "userId");

    auto gOid = pulse::bsonjson::tryOid(groupId);
    auto rOid = pulse::bsonjson::tryOid(requesterId);
    auto col = pulse::db::collection(pulse::models::conversation::kCollection);

    // Only creator can delete: findOne({ _id, type:'group', createdBy: req.user.userId })
    bsoncxx::stdx::optional<bsoncxx::document::value> groupDoc;
    if (gOid && rOid) {
      groupDoc = col.find_one(make_document(
          kvp("_id", *gOid),
          kvp("type", pulse::models::conversation::kTypeGroup),
          kvp("createdBy", *rOid)));
    }
    if (!groupDoc) {
      return callback(fail(drogon::k403Forbidden, "Not authorized or group not found"));
    }

    // Delete all messages: Message.deleteMany({ conversation: groupId })
    if (gOid) {
      pulse::db::collection(pulse::models::message::kCollection)
          .delete_many(make_document(kvp("conversation", *gOid)));
      // Delete conversation: Conversation.findByIdAndDelete(groupId)
      col.delete_one(make_document(kvp("_id", *gOid)));
    }

    return callback(okMessage("Group deleted"));
  } catch (const std::exception& e) {
    pulse::log::error("Delete group error: {}", e.what());
    return callback(serverFail(e.what()));
  }
}

}  // namespace pulse::controllers
