// follow.cc — implementation of the Follow model port (src/models/Follow.js).
#include "pulse/models/follow.hpp"

#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/oid.hpp>
#include <bsoncxx/types.hpp>

#include <mongocxx/collection.hpp>
#include <mongocxx/options/index.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/exception/operation_exception.hpp>

#include <chrono>

namespace pulse::models::follow {

namespace bld = bsoncxx::builder::basic;
using bld::kvp;
using bld::make_document;
using pulse::bsonjson::oid;
using pulse::bsonjson::oidToHex;
using pulse::bsonjson::nowIso8601;

// ---------------------------------------------------------------------------
// Indexes
// ---------------------------------------------------------------------------
void ensureIndexes() {
  auto col = pulse::db::collection(kCollection);

  // Single-field index on `follower` (schema `index: true`).
  col.create_index(make_document(kvp("follower", 1)));

  // Single-field index on `following` (schema `index: true`).
  col.create_index(make_document(kvp("following", 1)));

  // Compound unique index — prevents duplicate follows.
  {
    mongocxx::options::index opts{};
    opts.unique(true);
    col.create_index(make_document(kvp("follower", 1), kvp("following", 1)), opts);
  }

  // Index for efficient "get followers of X" queries.
  col.create_index(make_document(kvp("following", 1), kvp("createdAt", -1)));

  // Index for efficient "get who X follows" queries.
  col.create_index(make_document(kvp("follower", 1), kvp("createdAt", -1)));
}

// ---------------------------------------------------------------------------
// Defaults / output shaping
// ---------------------------------------------------------------------------
Json::Value applyDefaults(Json::Value doc) {
  // Mongoose `timestamps: true` sets createdAt/updatedAt on insert.
  const std::string now = nowIso8601();
  if (!doc.isMember("createdAt")) doc["createdAt"] = now;
  if (!doc.isMember("updatedAt")) doc["updatedAt"] = now;
  return doc;
}

Json::Value sanitizeForOutput(Json::Value doc) {
  // Default Mongoose toJSON drops the version key; the Follow schema has no
  // select:false / sensitive fields.
  doc.removeMember("__v");
  return doc;
}

namespace {
// Build the BSON insert document for a follow relationship, applying the
// Mongoose `timestamps: true` defaults (createdAt/updatedAt). The follower /
// following fields must be stored as real BSON ObjectIds, and the timestamps as
// real BSON Dates (not strings) so the { ..., createdAt:-1 } indexes sort
// correctly — matching how Mongoose persists these fields.
bsoncxx::document::value buildFollowDoc(const bsoncxx::oid& followerOid,
                                        const bsoncxx::oid& followingOid) {
  const bsoncxx::types::b_date now{
      std::chrono::milliseconds{pulse::bsonjson::nowMillis()}};
  return make_document(
      kvp("follower", followerOid),
      kvp("following", followingOid),
      kvp("createdAt", now),
      kvp("updatedAt", now));
}
} // namespace

// ---------------------------------------------------------------------------
// Static methods
// ---------------------------------------------------------------------------
ToggleResult toggleFollow(const std::string& followerId, const std::string& followingId) {
  auto col = pulse::db::collection(kCollection);
  const bsoncxx::oid followerOid = oid(followerId);
  const bsoncxx::oid followingOid = oid(followingId);

  ToggleResult result;

  // Delete-first toggle: atomic check-and-remove so concurrent requests can't
  // double-create (the unique index would otherwise surface as a 500).
  auto deleted = col.delete_one(
      make_document(kvp("follower", followerOid), kvp("following", followingOid)));

  const long long deletedCount = deleted ? deleted->deleted_count() : 0;

  if (deletedCount > 0) {
    result.followed = false;
    result.followerCount = col.count_documents(make_document(kvp("following", followingOid)));
    result.followingCount = col.count_documents(make_document(kvp("follower", followerOid)));
    return result;
  }

  try {
    col.insert_one(buildFollowDoc(followerOid, followingOid));
  } catch (const mongocxx::operation_exception& err) {
    // Duplicate from a concurrent follow (E11000) — already in desired state.
    if (err.code().value() != 11000) throw;
  }

  result.followed = true;
  result.followerCount = col.count_documents(make_document(kvp("following", followingOid)));
  result.followingCount = col.count_documents(make_document(kvp("follower", followerOid)));
  return result;
}

bool isFollowing(const std::string& followerId, const std::string& followingId) {
  auto col = pulse::db::collection(kCollection);
  auto doc = col.find_one(
      make_document(kvp("follower", oid(followerId)), kvp("following", oid(followingId))));
  return static_cast<bool>(doc);
}

long long getFollowerCount(const std::string& userId) {
  auto col = pulse::db::collection(kCollection);
  return col.count_documents(make_document(kvp("following", oid(userId))));
}

long long getFollowingCount(const std::string& userId) {
  auto col = pulse::db::collection(kCollection);
  return col.count_documents(make_document(kvp("follower", oid(userId))));
}

std::vector<std::string> getFollowerIds(const std::string& userId) {
  auto col = pulse::db::collection(kCollection);
  mongocxx::options::find opts{};
  opts.projection(make_document(kvp("follower", 1)));

  std::vector<std::string> ids;
  auto cursor = col.find(make_document(kvp("following", oid(userId))), opts);
  for (const auto& view : cursor) {
    auto el = view["follower"];
    if (el && el.type() == bsoncxx::type::k_oid) {
      ids.push_back(oidToHex(el.get_oid().value));
    }
  }
  return ids;
}

std::vector<std::string> getFollowingIds(const std::string& userId) {
  auto col = pulse::db::collection(kCollection);
  mongocxx::options::find opts{};
  opts.projection(make_document(kvp("following", 1)));

  std::vector<std::string> ids;
  auto cursor = col.find(make_document(kvp("follower", oid(userId))), opts);
  for (const auto& view : cursor) {
    auto el = view["following"];
    if (el && el.type() == bsoncxx::type::k_oid) {
      ids.push_back(oidToHex(el.get_oid().value));
    }
  }
  return ids;
}

} // namespace pulse::models::follow
