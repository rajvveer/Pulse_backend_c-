// bookmark.cc — C++ port of src/models/Bookmark.js. See bookmark.hpp for the
// schema (ground truth) this mirrors 1:1.
#include "pulse/models/bookmark.hpp"

#include "pulse/db.hpp"
#include "pulse/bson_json.hpp"
#include "pulse/logger.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <mongocxx/options/index.hpp>
#include <mongocxx/exception/exception.hpp>

#include <string>

namespace pulse::models::bookmark {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;

void ensureIndexes() {
  try {
    auto col = pulse::db::collection(kCollection);

    // { user: 1 }  — from `index: true` on the `user` field.
    col.create_index(make_document(kvp("user", 1)));

    // { user: 1, itemId: 1, itemType: 1 }, { unique: true }
    //   "One bookmark per user per item"
    {
      mongocxx::options::index opts{};
      opts.unique(true);
      col.create_index(
          make_document(kvp("user", 1), kvp("itemId", 1), kvp("itemType", 1)),
          opts);
    }

    // { user: 1, itemType: 1, createdAt: -1 }
    col.create_index(make_document(kvp("user", 1), kvp("itemType", 1), kvp("createdAt", -1)));

    pulse::log::info("\xF0\x9F\x93\x9D Ensured indexes for collection '{}'", kCollection);
  } catch (const std::exception& e) {
    pulse::log::error("Failed ensuring indexes for '{}': {}", kCollection, e.what());
    throw;
  }
}

bool isValidItemType(const std::string& itemType) {
  return itemType == kItemTypePost || itemType == kItemTypeReel;
}

Json::Value applyDefaults(Json::Value doc) {
  // The Bookmark schema declares no field-level defaults. `timestamps: true`
  // populates createdAt/updatedAt on insert — set them when absent so an insert
  // through this layer matches Mongoose's behaviour.
  const std::string now = pulse::bsonjson::nowIso8601();
  if (!doc.isMember("createdAt") || doc["createdAt"].isNull())
    doc["createdAt"] = now;
  if (!doc.isMember("updatedAt") || doc["updatedAt"].isNull())
    doc["updatedAt"] = now;
  return doc;
}

Json::Value sanitizeForOutput(Json::Value doc) {
  // Mongoose's default toJSON drops the internal version key; the Bookmark
  // schema defines no toJSON transform and marks no field `select:false`.
  doc.removeMember("__v");
  return doc;
}

} // namespace pulse::models::bookmark
