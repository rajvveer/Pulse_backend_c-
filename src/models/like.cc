// like.cc — C++ port of src/models/Like.js. See include/pulse/models/like.hpp.
//
// Preserves the exact filters, aggregation pipelines, sorts, and atomic toggle
// semantics from the Mongoose statics. Collection name and field names match
// the JS schema 1:1.
#include "pulse/models/like.hpp"

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
#include <mongocxx/exception/operation_exception.hpp>
#include <mongocxx/options/index.hpp>
#include <mongocxx/options/find.hpp>

#include <chrono>

namespace pulse::models::like {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;

namespace {

// modelMap from the JS toggleLike: targetType -> targetTypeModel.
// { post: 'Post', reel: 'Reel', comment: 'Comment' }.
std::string modelForTargetType(const std::string& targetType) {
  if (targetType == kTargetTypePost)    return kModelPost;
  if (targetType == kTargetTypeReel)    return kModelReel;
  if (targetType == kTargetTypeComment) return kModelComment;
  return std::string{};
}

// Convert a hex string to an ObjectId; on failure (mirrors the JS try/catch in
// the batch helpers) fall back to keeping the raw string as the BSON value.
// Returns true if a valid ObjectId was produced.
bool appendIdValue(bld::sub_array& arr, const std::string& id) {
  auto maybe = pulse::bsonjson::tryOid(id);
  if (maybe) { arr.append(*maybe); return true; }
  arr.append(id);
  return false;
}

// Milliseconds "now - hoursWindow" as a BSON date, matching:
//   new Date(Date.now() - hoursWindow * 60 * 60 * 1000)
bsoncxx::types::b_date sinceDate(double hoursWindow) {
  long long now = pulse::bsonjson::nowMillis();
  long long delta = static_cast<long long>(hoursWindow * 60.0 * 60.0 * 1000.0);
  return bsoncxx::types::b_date{std::chrono::milliseconds{now - delta}};
}

} // namespace

// =========================================================
//  INDEXES
// =========================================================
void ensureIndexes() {
  auto col = pulse::db::collection(kCollection);

  // user: { type: ObjectId, index: true }
  col.create_index(make_document(kvp("user", 1)));

  // Unique compound index — prevents duplicate likes.
  // likeSchema.index({ user: 1, targetType: 1, targetId: 1 }, { unique: true });
  {
    mongocxx::options::index opts{};
    opts.unique(true);
    col.create_index(
        make_document(kvp("user", 1), kvp("targetType", 1), kvp("targetId", 1)),
        opts);
  }

  // Fast lookup: "did user X like item Y?"
  // likeSchema.index({ targetType: 1, targetId: 1, user: 1 });
  col.create_index(
      make_document(kvp("targetType", 1), kvp("targetId", 1), kvp("user", 1)));

  // Fast count: "how many likes does item Y have?"
  // likeSchema.index({ targetType: 1, targetId: 1, createdAt: -1 });
  col.create_index(
      make_document(kvp("targetType", 1), kvp("targetId", 1), kvp("createdAt", -1)));

  // Velocity tracking: likes in time window for trending.
  // likeSchema.index({ targetType: 1, targetId: 1, createdAt: 1 });
  col.create_index(
      make_document(kvp("targetType", 1), kvp("targetId", 1), kvp("createdAt", 1)));

  // User's like history.
  // likeSchema.index({ user: 1, createdAt: -1 });
  col.create_index(make_document(kvp("user", 1), kvp("createdAt", -1)));

  pulse::log::info("Ensured indexes for collection '{}'", kCollection);
}

// =========================================================
//  DEFAULTS / OUTPUT SHAPING
// =========================================================
Json::Value applyDefaults(Json::Value doc) {
  // timestamps: true — set createdAt/updatedAt if missing.
  std::string now = pulse::bsonjson::nowIso8601();
  if (!doc.isMember("createdAt")) doc["createdAt"] = now;
  if (!doc.isMember("updatedAt")) doc["updatedAt"] = now;

  // Derive targetTypeModel from targetType when not explicitly provided,
  // mirroring the modelMap used by toggleLike.
  if ((!doc.isMember("targetTypeModel") || doc["targetTypeModel"].asString().empty())
      && doc.isMember("targetType")) {
    std::string m = modelForTargetType(doc["targetType"].asString());
    if (!m.empty()) doc["targetTypeModel"] = m;
  }

  return doc;
}

Json::Value sanitizeForOutput(Json::Value doc) {
  // The Like schema has no select:false / sensitive fields. Strip the Mongoose
  // version key to match the lean/toJSON output shape used across the API.
  doc.removeMember("__v");
  return doc;
}

// =========================================================
//  STATIC METHODS
// =========================================================

ToggleResult toggleLike(const std::string& userId,
                        const std::string& targetType,
                        const std::string& targetId) {
  auto col = pulse::db::collection(kCollection);
  const std::string targetTypeModel = modelForTargetType(targetType);

  auto userOid = pulse::bsonjson::oid(userId);
  auto targetOid = pulse::bsonjson::oid(targetId);

  // Delete-first toggle: deleteOne both checks and removes in one atomic op.
  // deleteOne({ user: userId, targetType, targetId })
  auto deleted = col.delete_one(make_document(
      kvp("user", userOid),
      kvp("targetType", targetType),
      kvp("targetId", targetOid)));

  ToggleResult result;

  if (deleted && deleted->deleted_count() > 0) {
    // Unlike — recount.
    result.liked = false;
    result.likeCount = col.count_documents(make_document(
        kvp("targetType", targetType),
        kvp("targetId", targetOid)));
    return result;
  }

  // Like — a concurrent request may have just created it; the unique index
  // makes that a duplicate-key error (code 11000), treated as "already liked".
  try {
    // create({ user, targetType, targetId, targetTypeModel }) + timestamps.
    auto nowDate = bsoncxx::types::b_date{std::chrono::milliseconds{pulse::bsonjson::nowMillis()}};
    col.insert_one(make_document(
        kvp("user", userOid),
        kvp("targetType", targetType),
        kvp("targetId", targetOid),
        kvp("targetTypeModel", targetTypeModel),
        kvp("createdAt", nowDate),
        kvp("updatedAt", nowDate)));
  } catch (const mongocxx::operation_exception& err) {
    // if (err.code !== 11000) throw err;
    if (err.code().value() != 11000) throw;
  }

  result.liked = true;
  result.likeCount = col.count_documents(make_document(
      kvp("targetType", targetType),
      kvp("targetId", targetOid)));
  return result;
}

bool isLikedBy(const std::string& userId,
               const std::string& targetType,
               const std::string& targetId) {
  auto col = pulse::db::collection(kCollection);
  auto userOid = pulse::bsonjson::oid(userId);
  auto targetOid = pulse::bsonjson::oid(targetId);

  // countDocuments({ user, targetType, targetId }) > 0
  long long count = col.count_documents(make_document(
      kvp("user", userOid),
      kvp("targetType", targetType),
      kvp("targetId", targetOid)));
  return count > 0;
}

std::set<std::string> getLikedIds(const std::string& userId,
                                  const std::string& targetType,
                                  const std::vector<std::string>& targetIds) {
  std::set<std::string> liked;
  auto col = pulse::db::collection(kCollection);
  auto userOid = pulse::bsonjson::oid(userId);

  // find({ user, targetType, targetId: { $in: targetIds } }).select('targetId').lean()
  auto idArray = bld::array{};
  for (const auto& id : targetIds) {
    auto maybe = pulse::bsonjson::tryOid(id);
    if (maybe) idArray.append(*maybe);
    else       idArray.append(id);
  }

  mongocxx::options::find opts{};
  opts.projection(make_document(kvp("targetId", 1)));

  auto cursor = col.find(make_document(
      kvp("user", userOid),
      kvp("targetType", targetType),
      kvp("targetId", make_document(kvp("$in", idArray)))), opts);

  for (auto&& d : cursor) {
    auto el = d["targetId"];
    if (el && el.type() == bsoncxx::type::k_oid) {
      liked.insert(el.get_oid().value.to_string());
    } else if (el && el.type() == bsoncxx::type::k_string) {
      liked.insert(std::string(el.get_string().value));
    }
  }
  return liked;
}

long long getLikeCount(const std::string& targetType,
                       const std::string& targetId) {
  auto col = pulse::db::collection(kCollection);
  auto targetOid = pulse::bsonjson::oid(targetId);

  // countDocuments({ targetType, targetId })
  return col.count_documents(make_document(
      kvp("targetType", targetType),
      kvp("targetId", targetOid)));
}

double getLikeVelocity(const std::string& targetType,
                       const std::string& targetId,
                       double hoursWindow) {
  auto col = pulse::db::collection(kCollection);
  auto targetOid = pulse::bsonjson::oid(targetId);
  auto since = sinceDate(hoursWindow);

  // countDocuments({ targetType, targetId, createdAt: { $gte: since } })
  long long count = col.count_documents(make_document(
      kvp("targetType", targetType),
      kvp("targetId", targetOid),
      kvp("createdAt", make_document(kvp("$gte", since)))));

  return static_cast<double>(count) / hoursWindow;
}

std::map<std::string, double> getBatchLikeVelocities(
    const std::string& targetType,
    const std::vector<std::string>& targetIds,
    double hoursWindow) {
  auto col = pulse::db::collection(kCollection);
  auto since = sinceDate(hoursWindow);

  // objectIds = targetIds.map(id => new ObjectId(id) | id)
  auto idArray = bld::array{};
  for (const auto& id : targetIds) {
    appendIdValue(idArray, id);
  }

  // aggregate([
  //   { $match: { targetType, targetId: { $in: objectIds }, createdAt: { $gte: since } } },
  //   { $group: { _id: '$targetId', count: { $sum: 1 } } }
  // ])
  mongocxx::pipeline pipeline{};
  pipeline.match(make_document(
      kvp("targetType", targetType),
      kvp("targetId", make_document(kvp("$in", idArray))),
      kvp("createdAt", make_document(kvp("$gte", since)))));
  pipeline.group(make_document(
      kvp("_id", "$targetId"),
      kvp("count", make_document(kvp("$sum", 1)))));

  std::map<std::string, double> velocityMap;
  auto cursor = col.aggregate(pipeline);
  for (auto&& r : cursor) {
    auto idEl = r["_id"];
    auto cEl = r["count"];
    std::string key;
    if (idEl && idEl.type() == bsoncxx::type::k_oid)       key = idEl.get_oid().value.to_string();
    else if (idEl && idEl.type() == bsoncxx::type::k_string) key = std::string(idEl.get_string().value);
    long long count = 0;
    if (cEl && cEl.type() == bsoncxx::type::k_int32)      count = cEl.get_int32().value;
    else if (cEl && cEl.type() == bsoncxx::type::k_int64) count = cEl.get_int64().value;
    else if (cEl && cEl.type() == bsoncxx::type::k_double) count = static_cast<long long>(cEl.get_double().value);
    velocityMap[key] = static_cast<double>(count) / hoursWindow;
  }

  // Fill zeros for items with no likes (targetIds.forEach ... if (!has) set 0).
  for (const auto& id : targetIds) {
    if (velocityMap.find(id) == velocityMap.end()) velocityMap[id] = 0.0;
  }
  return velocityMap;
}

std::map<std::string, long long> getBatchLikeCounts(
    const std::string& targetType,
    const std::vector<std::string>& targetIds) {
  auto col = pulse::db::collection(kCollection);

  // objectIds = targetIds.map(id => new ObjectId(id) | id)
  auto idArray = bld::array{};
  for (const auto& id : targetIds) {
    appendIdValue(idArray, id);
  }

  // aggregate([
  //   { $match: { targetType, targetId: { $in: objectIds } } },
  //   { $group: { _id: '$targetId', count: { $sum: 1 } } }
  // ])
  mongocxx::pipeline pipeline{};
  pipeline.match(make_document(
      kvp("targetType", targetType),
      kvp("targetId", make_document(kvp("$in", idArray)))));
  pipeline.group(make_document(
      kvp("_id", "$targetId"),
      kvp("count", make_document(kvp("$sum", 1)))));

  std::map<std::string, long long> countMap;
  auto cursor = col.aggregate(pipeline);
  for (auto&& r : cursor) {
    auto idEl = r["_id"];
    auto cEl = r["count"];
    std::string key;
    if (idEl && idEl.type() == bsoncxx::type::k_oid)       key = idEl.get_oid().value.to_string();
    else if (idEl && idEl.type() == bsoncxx::type::k_string) key = std::string(idEl.get_string().value);
    long long count = 0;
    if (cEl && cEl.type() == bsoncxx::type::k_int32)      count = cEl.get_int32().value;
    else if (cEl && cEl.type() == bsoncxx::type::k_int64) count = cEl.get_int64().value;
    else if (cEl && cEl.type() == bsoncxx::type::k_double) count = static_cast<long long>(cEl.get_double().value);
    countMap[key] = count;
  }

  // Fill zeros for items with no likes.
  for (const auto& id : targetIds) {
    if (countMap.find(id) == countMap.end()) countMap[id] = 0;
  }
  return countMap;
}

} // namespace pulse::models::like
