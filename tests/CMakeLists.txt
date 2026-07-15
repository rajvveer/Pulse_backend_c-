add_executable(pulse_bson_json_tests
  bson_json_tests.cc
  ${PROJECT_SOURCE_DIR}/src/db/bson_json.cc
)

target_include_directories(pulse_bson_json_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(pulse_bson_json_tests PRIVATE
  ${PULSE_BSONCXX_TARGET}
)

if(PULSE_JSONCPP_TARGET)
  target_link_libraries(pulse_bson_json_tests PRIVATE ${PULSE_JSONCPP_TARGET})
else()
  target_link_libraries(pulse_bson_json_tests PRIVATE Drogon::Drogon)
endif()

add_test(NAME pulse_bson_json_tests COMMAND pulse_bson_json_tests)
