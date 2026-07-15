// bson_json.cc — BSON <-> JSON conversion implementation.
#include "pulse/bson_json.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <chrono>
#include <cctype>
#include <ctime>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace pulse::bsonjson {

using bsoncxx::type;
namespace bld = bsoncxx::builder::basic;

namespace {

std::string iso8601FromMillis(int64_t millis) {
  // C++ division truncates toward zero. Normalize negative epochs so the
  // seconds and millisecond remainder still describe the same instant.
  int64_t seconds = millis / 1000;
  int64_t remainder = millis % 1000;
  if (remainder < 0) {
    remainder += 1000;
    --seconds;
  }

  const std::time_t t = static_cast<std::time_t>(seconds);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif

  char date[24]{};
  std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &tm);
  char result[32]{};
  std::snprintf(result, sizeof(result), "%s.%03lldZ", date,
                static_cast<long long>(remainder));
  return std::string(result);
}

} // namespace

Json::Value valueToJson(const bsoncxx::types::bson_value::view& v) {
  switch (v.type()) {
    case type::k_string:    return Json::Value(std::string(v.get_string().value));
    case type::k_int32:   return Json::Value(v.get_int32().value);
    case type::k_int64:   return Json::Value(static_cast<Json::Int64>(v.get_int64().value));
    case type::k_double:  return Json::Value(v.get_double().value);
    case type::k_bool:    return Json::Value(v.get_bool().value);
    case type::k_oid:     return Json::Value(v.get_oid().value.to_string());
    case type::k_date: {
      // ISO-8601 UTC (matches Mongoose Date serialization).
      return Json::Value(iso8601FromMillis(v.get_date().to_int64()));
    }
    case type::k_null:       return Json::Value(Json::nullValue);
    case type::k_document:   return toJson(v.get_document().value);
    case type::k_array:      return toJson(v.get_array().value);
    case type::k_decimal128: return Json::Value(v.get_decimal128().value.to_string());
    default:                 return Json::Value(Json::nullValue);
  }
}

Json::Value toJson(const bsoncxx::document::view& doc) {
  Json::Value out(Json::objectValue);
  for (const auto& el : doc) {
    out[std::string(el.key())] = valueToJson(el.get_value());
  }
  return out;
}

Json::Value toJson(const bsoncxx::array::view& arr) {
  Json::Value out(Json::arrayValue);
  for (const auto& el : arr) out.append(valueToJson(el.get_value()));
  return out;
}

namespace {
void appendJsonArray(bld::sub_array& sub, const Json::Value& v);

int64_t checkedUnsigned(const Json::Value& value) {
  const auto unsignedValue = value.asUInt64();
  if (unsignedValue > static_cast<Json::UInt64>(
                          std::numeric_limits<int64_t>::max())) {
    throw std::overflow_error("JSON unsigned integer exceeds BSON int64 range");
  }
  return static_cast<int64_t>(unsignedValue);
}

// basic::document and basic::sub_document both expose append(). Keeping this
// helper generic makes the top-level conversion type-safe.
template <typename DocumentBuilder>
void appendValue(DocumentBuilder& sub, const std::string& key, const Json::Value& v) {
  switch (v.type()) {
    case Json::nullValue:   sub.append(bld::kvp(key, bsoncxx::types::b_null{})); break;
    case Json::intValue:    sub.append(bld::kvp(key, static_cast<int64_t>(v.asInt64()))); break;
    case Json::uintValue:   sub.append(bld::kvp(key, checkedUnsigned(v))); break;
    case Json::realValue:   sub.append(bld::kvp(key, v.asDouble())); break;
    case Json::stringValue: sub.append(bld::kvp(key, v.asString())); break;
    case Json::booleanValue:sub.append(bld::kvp(key, v.asBool())); break;
    case Json::arrayValue:
      sub.append(bld::kvp(key, [&](bld::sub_array a) { for (const auto& e : v) appendJsonArray(a, e); }));
      break;
    case Json::objectValue:
      sub.append(bld::kvp(key, [&](bld::sub_document d) {
        for (const auto& k : v.getMemberNames()) appendValue(d, k, v[k]);
      }));
      break;
  }
}

void appendJsonArray(bld::sub_array& a, const Json::Value& v) {
  switch (v.type()) {
    case Json::nullValue:   a.append(bsoncxx::types::b_null{}); break;
    case Json::intValue:    a.append(static_cast<int64_t>(v.asInt64())); break;
    case Json::uintValue:   a.append(checkedUnsigned(v)); break;
    case Json::realValue:   a.append(v.asDouble()); break;
    case Json::stringValue: a.append(v.asString()); break;
    case Json::booleanValue:a.append(v.asBool()); break;
    case Json::arrayValue:  a.append([&](bld::sub_array sa){ for (const auto& e : v) appendJsonArray(sa, e); }); break;
    case Json::objectValue: a.append([&](bld::sub_document d){ for (const auto& k : v.getMemberNames()) appendValue(d, k, v[k]); }); break;
  }
}
} // namespace

bsoncxx::document::value fromJson(const Json::Value& v) {
  bld::document doc;
  if (v.isObject()) {
    for (const auto& k : v.getMemberNames()) {
      appendValue(doc, k, v[k]);
    }
  }
  return doc.extract();
}

bool isValidOid(const std::string& hex) {
  if (hex.size() != 24) return false;
  for (char c : hex) if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
  return true;
}

bsoncxx::oid oid(const std::string& hex) { return bsoncxx::oid{hex}; }

std::optional<bsoncxx::oid> tryOid(const std::string& hex) {
  if (!isValidOid(hex)) return std::nullopt;
  try { return bsoncxx::oid{hex}; } catch (...) { return std::nullopt; }
}

std::string oidToHex(const bsoncxx::oid& id) { return id.to_string(); }

long long nowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string nowIso8601() {
  return iso8601FromMillis(nowMillis());
}

} // namespace pulse::bsonjson
