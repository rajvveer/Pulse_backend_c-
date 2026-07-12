// notification.cc — implementation of the Notification model port.
//
// 1:1 with src/models/Notification.js: same collection, same indexes, same
// filters / sorts / skips / limits / update operators. BSON is built with the
// bsoncxx basic builders; results are converted to Json::Value via
// pulse::bsonjson::toJson.
#include "pulse/models/notification.hpp"

#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/oid.hpp>

#include <mongocxx/collection.hpp>
#include <mongocxx/cursor.hpp>
#include <mongocxx/pipeline.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/index.hpp>
#include <mongocxx/options/count.hpp>
#include <mongocxx/options/find_one_and_update.hpp>
#include <mongocxx/options/update.hpp>

#include <chrono>
#include <vector>

namespace pulse::models::notification {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;
namespace bj = pulse::bsonjson;

namespace {

// Milliseconds since epoch (matches Date.now()).
long long nowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// b_date from a millisecond epoch value.
bsoncxx::types::b_date dateFromMillis(long long ms) {
  return bsoncxx::types::b_date{std::chrono::milliseconds{ms}};
}

// Append a reference field (ObjectId) from a Json value to a builder. A 24-hex
// string coerces to a bsoncxx::oid (matching how Mongoose casts ref ids); any
// other string is kept verbatim as a fallback.
void appendRef(bld::document& doc, const std::string& key, const Json::Value& v) {
  if (v.isString()) {
    if (auto id = bj::tryOid(v.asString())) { doc.append(kvp(key, *id)); return; }
    doc.append(kvp(key, v.asString()));
  }
}

} // namespace

void ensureIndexes() {
  auto col = pulse::db::collection(kCollection);

  // Field-level `index: true`.
  col.create_index(make_document(kvp("recipient", 1)));
  col.create_index(make_document(kvp("isRead", 1)));

  // Compound indexes declared via schema.index(...).
  col.create_index(make_document(kvp("recipient", 1), kvp("createdAt", -1)));
  col.create_index(make_document(kvp("recipient", 1), kvp("isRead", 1), kvp("createdAt", -1)));
  col.create_index(make_document(kvp("recipient", 1), kvp("type", 1), kvp("createdAt", -1)));

  pulse::log::info("Ensured indexes for collection '{}'", kCollection);
}

// ---- getNotifications --------------------------------------------------------
Json::Value getNotifications(const std::string& userId,
                             const GetNotificationsOptions& options) {
  const int page = options.page;
  const int limit = options.limit;

  auto col = pulse::db::collection(kCollection);

  // Build query: { recipient: userId [, type] [, isRead:false] }.
  bld::document query;
  if (auto rid = bj::tryOid(userId)) query.append(kvp("recipient", *rid));
  else                              query.append(kvp("recipient", userId));
  if (options.type)   query.append(kvp("type", *options.type));
  if (options.unreadOnly) query.append(kvp("isRead", false));
  auto queryView = query.view();

  // .sort({createdAt:-1}).skip((page-1)*limit).limit(limit)
  mongocxx::options::find findOpts;
  findOpts.sort(make_document(kvp("createdAt", -1)));
  findOpts.skip(static_cast<std::int64_t>((page - 1) * limit));
  findOpts.limit(static_cast<std::int64_t>(limit));

  Json::Value notifications(Json::arrayValue);
  auto cursor = col.find(queryView, findOpts);
  for (const auto& doc : cursor) {
    notifications.append(sanitizeForOutput(bj::toJson(doc)));
  }

  // countDocuments(query)
  long long total = static_cast<long long>(col.count_documents(queryView));

  Json::Value pagination(Json::objectValue);
  pagination["page"] = page;
  pagination["limit"] = limit;
  pagination["total"] = static_cast<Json::Int64>(total);
  pagination["hasMore"] = (static_cast<long long>(page) * limit < total);

  Json::Value out(Json::objectValue);
  out["notifications"] = notifications;
  out["pagination"] = pagination;
  return out;
}

// ---- getUnreadCount ----------------------------------------------------------
long long getUnreadCount(const std::string& userId) {
  auto col = pulse::db::collection(kCollection);
  bld::document query;
  if (auto rid = bj::tryOid(userId)) query.append(kvp("recipient", *rid));
  else                              query.append(kvp("recipient", userId));
  query.append(kvp("isRead", false));
  return static_cast<long long>(col.count_documents(query.view()));
}

// ---- getUnreadCountByType ----------------------------------------------------
Json::Value getUnreadCountByType(const std::string& userId) {
  auto col = pulse::db::collection(kCollection);

  // [ { $match: { recipient: ObjectId(userId), isRead:false } },
  //   { $group:  { _id: '$type', count: { $sum: 1 } } } ]
  mongocxx::pipeline pipe;
  {
    bld::document match;
    if (auto rid = bj::tryOid(userId)) match.append(kvp("recipient", *rid));
    else                              match.append(kvp("recipient", userId));
    match.append(kvp("isRead", false));
    pipe.match(match.view());
  }
  pipe.group(make_document(
      kvp("_id", "$type"),
      kvp("count", make_document(kvp("$sum", 1)))));

  Json::Value counts(Json::objectValue);
  counts["total"] = 0;
  long long total = 0;

  auto cursor = col.aggregate(pipe);
  for (const auto& view : cursor) {
    // _id may be null if a document had no `type`; guard the read.
    auto idEl = view["_id"];
    if (idEl.type() != bsoncxx::type::k_string) continue;
    std::string type = std::string(idEl.get_string().value);
    long long count = 0;
    auto cntEl = view["count"];
    if (cntEl.type() == bsoncxx::type::k_int32)      count = cntEl.get_int32().value;
    else if (cntEl.type() == bsoncxx::type::k_int64) count = cntEl.get_int64().value;
    else if (cntEl.type() == bsoncxx::type::k_double)count = static_cast<long long>(cntEl.get_double().value);
    counts[type] = static_cast<Json::Int64>(count);
    total += count;
  }
  counts["total"] = static_cast<Json::Int64>(total);
  return counts;
}

// ---- markAsRead --------------------------------------------------------------
std::optional<Json::Value> markAsRead(const std::string& notificationId,
                                      const std::string& userId) {
  auto col = pulse::db::collection(kCollection);

  // findOneAndUpdate({_id, recipient}, {isRead:true}, {new:true})
  bld::document filter;
  if (auto nid = bj::tryOid(notificationId)) filter.append(kvp("_id", *nid));
  else                                       filter.append(kvp("_id", notificationId));
  if (auto rid = bj::tryOid(userId))         filter.append(kvp("recipient", *rid));
  else                                       filter.append(kvp("recipient", userId));

  auto update = make_document(kvp("$set", make_document(kvp("isRead", true))));

  mongocxx::options::find_one_and_update opts;
  opts.return_document(mongocxx::options::return_document::k_after); // {new:true}

  auto result = col.find_one_and_update(filter.view(), update.view(), opts);
  if (!result) return std::nullopt;
  return sanitizeForOutput(bj::toJson(result->view()));
}

// ---- markAllAsRead -----------------------------------------------------------
long long markAllAsRead(const std::string& userId,
                        const std::optional<std::string>& type) {
  auto col = pulse::db::collection(kCollection);

  // updateMany({recipient, isRead:false [, type]}, {isRead:true})
  bld::document query;
  if (auto rid = bj::tryOid(userId)) query.append(kvp("recipient", *rid));
  else                              query.append(kvp("recipient", userId));
  query.append(kvp("isRead", false));
  if (type) query.append(kvp("type", *type));

  auto update = make_document(kvp("$set", make_document(kvp("isRead", true))));

  auto result = col.update_many(query.view(), update.view());
  if (!result) return 0;
  return static_cast<long long>(result->modified_count());
}

// ---- createNotification ------------------------------------------------------
std::optional<Json::Value> createNotification(const Json::Value& data) {
  // Don't notify yourself: data.sender.toString() === data.recipient.toString()
  const std::string sender = data.get("sender", "").asString();
  const std::string recipient = data.get("recipient", "").asString();
  if (sender == recipient) return std::nullopt;

  auto col = pulse::db::collection(kCollection);

  // Check for recent duplicate within 1 minute:
  //   findOne({ recipient, sender, type, post, reel,
  //             createdAt: { $gte: new Date(Date.now() - 60000) } })
  // Mongoose strips `undefined` query values, so when data.post / data.reel are
  // absent those keys are simply NOT part of the filter — mirror that here by
  // appending a reference key only when a value is present.
  {
    bld::document dup;
    appendRef(dup, "recipient", data["recipient"]);
    appendRef(dup, "sender", data["sender"]);
    if (data.isMember("type") && data["type"].isString())
      dup.append(kvp("type", data["type"].asString()));
    if (data.isMember("post") && !data["post"].isNull())
      appendRef(dup, "post", data["post"]);
    if (data.isMember("reel") && !data["reel"].isNull())
      appendRef(dup, "reel", data["reel"]);
    dup.append(kvp("createdAt",
                   make_document(kvp("$gte", dateFromMillis(nowMillis() - 60000)))));

    auto existing = col.find_one(dup.view());
    if (existing) return sanitizeForOutput(bj::toJson(existing->view()));
  }

  // Create the notification (apply schema defaults, then insert).
  Json::Value toInsert = applyDefaults(data);

  // Build the BSON insert doc, coercing reference fields to ObjectIds.
  bld::document insert;
  for (const auto& key : toInsert.getMemberNames()) {
    const Json::Value& v = toInsert[key];
    static const std::vector<std::string> refKeys = {
        "recipient", "sender", "post", "reel", "whisper", "conversation", "comment"};
    bool isRef = false;
    for (const auto& rk : refKeys) if (rk == key) { isRef = true; break; }

    if (key == "isRead") {
      insert.append(kvp("isRead", v.asBool()));
    } else if (key == "__v") {
      insert.append(kvp("__v", v.asInt()));
    } else if (key == "createdAt" || key == "updatedAt") {
      insert.append(kvp(key, dateFromMillis(nowMillis())));
    } else if (isRef) {
      if (v.isString()) {
        if (auto id = bj::tryOid(v.asString())) insert.append(kvp(key, *id));
        else                                    insert.append(kvp(key, v.asString()));
      } else if (v.isNull()) {
        // skip null refs (optional fields) — Mongoose would omit them.
      }
    } else if (v.isString()) {
      insert.append(kvp(key, v.asString()));
    } else if (v.isBool()) {
      insert.append(kvp(key, v.asBool()));
    } else if (v.isIntegral()) {
      insert.append(kvp(key, static_cast<std::int64_t>(v.asInt64())));
    } else if (v.isDouble()) {
      insert.append(kvp(key, v.asDouble()));
    }
  }

  auto inserted = col.insert_one(insert.view());
  if (!inserted) return std::nullopt;

  // Return the freshly created document.
  auto created = col.find_one(make_document(kvp("_id", inserted->inserted_id())));
  if (!created) return std::nullopt;
  return sanitizeForOutput(bj::toJson(created->view()));
}

// ---- deleteOldNotifications --------------------------------------------------
long long deleteOldNotifications(int daysOld) {
  auto col = pulse::db::collection(kCollection);

  // cutoff = Date.now() - daysOld*24*60*60*1000
  long long cutoffMs = nowMillis() - static_cast<long long>(daysOld) * 24 * 60 * 60 * 1000;

  // deleteMany({ createdAt: { $lt: cutoff }, isRead: true })
  auto filter = make_document(
      kvp("createdAt", make_document(kvp("$lt", dateFromMillis(cutoffMs)))),
      kvp("isRead", true));

  auto result = col.delete_many(filter.view());
  if (!result) return 0;
  return static_cast<long long>(result->deleted_count());
}

// ---- applyDefaults -----------------------------------------------------------
Json::Value applyDefaults(Json::Value doc) {
  // isRead default: false
  if (!doc.isMember("isRead")) doc["isRead"] = false;

  // timestamps: true -> createdAt / updatedAt
  const std::string nowIso = bj::nowIso8601();
  if (!doc.isMember("createdAt")) doc["createdAt"] = nowIso;
  if (!doc.isMember("updatedAt")) doc["updatedAt"] = nowIso;

  // Mongoose document version key.
  if (!doc.isMember("__v")) doc["__v"] = 0;

  return doc;
}

// ---- sanitizeForOutput -------------------------------------------------------
Json::Value sanitizeForOutput(Json::Value doc) {
  // Default Mongoose toJSON keeps _id; only the internal version key is dropped
  // by the standard transform used across this codebase. This schema declares no
  // select:false fields and no sensitive fields.
  doc.removeMember("__v");
  return doc;
}

} // namespace pulse::models::notification
