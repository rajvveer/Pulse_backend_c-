// bson_json.hpp — conversion between BSON (mongocxx) and JSON (JsonCpp).
//
// The Node code passed Mongo documents straight through res.json(); here we
// convert bsoncxx views to Json::Value for the API layer, and build BSON from
// Json::Value for queries/inserts. ObjectId is rendered as its 24-char hex
// string (matching how Mongoose serializes _id), and dates as ISO-8601 strings.
#pragma once
#include <string>
#include <json/json.h>
#include <bsoncxx/document/view.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/array/view.hpp>
#include <bsoncxx/types.hpp>
// mongo-cxx-driver 4.x split forward-decls (-fwd.hpp, pulled in by <bsoncxx/types.hpp>)
// from full definitions. bson_value::view is used by-value below, so the FULL
// definition headers are required, not just the forward declaration.
#include <bsoncxx/types/bson_value/view.hpp>
#include <bsoncxx/types/bson_value/value.hpp>
#include <bsoncxx/oid.hpp>
#include <optional>

namespace pulse::bsonjson {

// BSON document/array view -> Json::Value.
Json::Value toJson(const bsoncxx::document::view& doc);
Json::Value toJson(const bsoncxx::array::view& arr);
Json::Value valueToJson(const bsoncxx::types::bson_value::view& v);

// Json::Value -> BSON document value. Strings that look like a 24-hex ObjectId
// under keys named "_id"/ending in "Id"/"author"/etc. are NOT auto-coerced here;
// use oid() explicitly where an ObjectId is required.
bsoncxx::document::value fromJson(const Json::Value& v);

// Helpers for ObjectId handling.
bool isValidOid(const std::string& hex);
bsoncxx::oid oid(const std::string& hex);                 // throws if invalid
std::optional<bsoncxx::oid> tryOid(const std::string& hex);
std::string oidToHex(const bsoncxx::oid& id);

// Convenience: an ISO-8601 UTC timestamp for "now" (createdAt/updatedAt).
std::string nowIso8601();
long long nowMillis();

} // namespace pulse::bsonjson
