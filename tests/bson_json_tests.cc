#include "pulse/bson_json.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>

#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void roundTripsNestedJson() {
  Json::Value input(Json::objectValue);
  input["name"] = "pulse";
  input["enabled"] = true;
  input["count"] = Json::Int64{42};
  input["nothing"] = Json::Value(Json::nullValue);
  input["nested"]["score"] = 4.5;
  input["items"].append("first");
  input["items"].append(Json::Int64{-7});
  input["items"].append(false);

  const auto bson = pulse::bsonjson::fromJson(input);
  const Json::Value output = pulse::bsonjson::toJson(bson.view());

  require(output == input, "nested JSON did not survive BSON round trip");
}

void preservesDateMilliseconds() {
  using bsoncxx::builder::basic::kvp;
  using bsoncxx::builder::basic::make_document;

  const auto doc = make_document(
      kvp("positive", bsoncxx::types::b_date{std::chrono::milliseconds{123}}),
      kvp("negative", bsoncxx::types::b_date{std::chrono::milliseconds{-1}}));
  const Json::Value output = pulse::bsonjson::toJson(doc.view());

  require(output["positive"].asString() == "1970-01-01T00:00:00.123Z",
          "positive date milliseconds were lost");
  require(output["negative"].asString() == "1969-12-31T23:59:59.999Z",
          "negative epoch date was formatted incorrectly");
}

void validatesObjectIds() {
  constexpr const char* valid = "507f1f77bcf86cd799439011";
  require(pulse::bsonjson::isValidOid(valid), "valid ObjectId rejected");
  require(!pulse::bsonjson::isValidOid("507f1f77bcf86cd79943901z"),
          "invalid ObjectId accepted");
  const auto parsed = pulse::bsonjson::tryOid(valid);
  require(parsed && pulse::bsonjson::oidToHex(*parsed) == valid,
          "ObjectId did not round trip");
}

void rejectsUnsignedOverflow() {
  Json::Value input(Json::objectValue);
  input["tooLarge"] = Json::Value{std::numeric_limits<Json::UInt64>::max()};
  bool threw = false;
  try {
    (void)pulse::bsonjson::fromJson(input);
  } catch (const std::overflow_error&) {
    threw = true;
  }
  require(threw, "unsigned JSON integer silently overflowed BSON int64");
}

}  // namespace

int main() {
  try {
    roundTripsNestedJson();
    preservesDateMilliseconds();
    validatesObjectIds();
    rejectsUnsignedOverflow();
    std::cout << "pulse_bson_json_tests: passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "pulse_bson_json_tests: " << error.what() << '\n';
    return 1;
  }
}
