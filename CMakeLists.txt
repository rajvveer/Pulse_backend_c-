cmake_minimum_required(VERSION 3.16)

project(pulse_backend_cpp LANGUAGES C CXX VERSION 1.0.0)

# ── Language / build defaults ──
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Exceptions are ON (the algorithm kernels and DB drivers use them).
if(MSVC)
  add_compile_options(/EHsc /utf-8 /bigobj)
  # NOMINMAX: Windows defines min()/max() as macros, which mangle the
  # mongo-cxx-driver v4 headers (e.g. find_options::max()/min() methods) into
  # syntax errors. WIN32_LEAN_AND_MEAN trims the Windows header surface.
  # _CRT_SECURE_NO_WARNINGS quiets the legacy-CRT deprecation noise.
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# ── Dependencies (resolved via vcpkg manifest mode) ──
find_package(Drogon CONFIG REQUIRED)
find_package(mongocxx CONFIG REQUIRED)
find_package(bsoncxx CONFIG REQUIRED)
find_package(redis++ CONFIG REQUIRED)
find_package(jwt-cpp CONFIG REQUIRED)
find_package(jsoncpp CONFIG REQUIRED)   # Json::Value — used throughout (and by Drogon)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(Threads REQUIRED)

# mongo-cxx-driver target names depend on whether the active vcpkg triplet is
# dynamic (Windows) or static (the default x64-linux Docker triplet).
if(TARGET mongo::mongocxx_shared)
  set(PULSE_MONGOCXX_TARGET mongo::mongocxx_shared)
elseif(TARGET mongo::mongocxx_static)
  set(PULSE_MONGOCXX_TARGET mongo::mongocxx_static)
else()
  message(FATAL_ERROR "mongocxx target not found")
endif()

if(TARGET mongo::bsoncxx_shared)
  set(PULSE_BSONCXX_TARGET mongo::bsoncxx_shared)
elseif(TARGET mongo::bsoncxx_static)
  set(PULSE_BSONCXX_TARGET mongo::bsoncxx_static)
else()
  message(FATAL_ERROR "bsoncxx target not found")
endif()

# redis-plus-plus exports either redis++::redis++ (shared) or redis++::redis++_static
# depending on how vcpkg built it. Pick whichever target this install provides.
if(TARGET redis++::redis++)
  set(PULSE_REDISPP_TARGET redis++::redis++)
elseif(TARGET redis++::redis++_static)
  set(PULSE_REDISPP_TARGET redis++::redis++_static)
else()
  message(FATAL_ERROR "redis-plus-plus target not found (expected redis++::redis++ or redis++::redis++_static)")
endif()

# jsoncpp likewise exports jsoncpp_lib (shared) or jsoncpp_static.
if(TARGET JsonCpp::JsonCpp)
  set(PULSE_JSONCPP_TARGET JsonCpp::JsonCpp)
elseif(TARGET jsoncpp_lib)
  set(PULSE_JSONCPP_TARGET jsoncpp_lib)
elseif(TARGET jsoncpp_static)
  set(PULSE_JSONCPP_TARGET jsoncpp_static)
else()
  set(PULSE_JSONCPP_TARGET "")  # Drogon transitively provides jsoncpp in most installs
endif()

# ── Vendored bcrypt (self-contained, no external dependency) ──
# Solar Designer's public-domain crypt_blowfish + the trusch/libbcrypt wrapper,
# compiled here as C into a small static library. This provides our own
# <bcrypt.h> (third_party/bcrypt/bcrypt.h) with the exact bcrypt_gensalt /
# bcrypt_hashpw / bcrypt_checkpw API the services call — no system crypt and no
# OpenSSL needed for password/OTP hashing.
set(PULSE_BCRYPT_DIR ${CMAKE_SOURCE_DIR}/third_party/bcrypt)
add_library(pulse_bcrypt STATIC
  ${PULSE_BCRYPT_DIR}/bcrypt.c
  ${PULSE_BCRYPT_DIR}/crypt_blowfish.c
  ${PULSE_BCRYPT_DIR}/crypt_gensalt.c
  ${PULSE_BCRYPT_DIR}/wrapper.c
)
# These are C sources; ensure they build as C regardless of any global setting.
set_target_properties(pulse_bcrypt PROPERTIES LINKER_LANGUAGE C)
set_source_files_properties(
  ${PULSE_BCRYPT_DIR}/bcrypt.c
  ${PULSE_BCRYPT_DIR}/crypt_blowfish.c
  ${PULSE_BCRYPT_DIR}/crypt_gensalt.c
  ${PULSE_BCRYPT_DIR}/wrapper.c
  PROPERTIES LANGUAGE C
)
# Expose the vendored header to consumers of this target.
target_include_directories(pulse_bcrypt PUBLIC ${PULSE_BCRYPT_DIR})

# ── Sources ──
# Algorithm kernels are pure C++ (copied verbatim from native/src), compiled in.
file(GLOB ALGO_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/algorithms/*.cc)

# Everything else (config, db, models, services, filters, controllers, sockets,
# jobs, utils, app/server bootstrap).
file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/src/config/*.cc
  ${CMAKE_SOURCE_DIR}/src/db/*.cc
  ${CMAKE_SOURCE_DIR}/src/models/*.cc
  ${CMAKE_SOURCE_DIR}/src/services/*.cc
  ${CMAKE_SOURCE_DIR}/src/filters/*.cc
  ${CMAKE_SOURCE_DIR}/src/controllers/*.cc
  ${CMAKE_SOURCE_DIR}/src/sockets/*.cc
  ${CMAKE_SOURCE_DIR}/src/jobs/*.cc
  ${CMAKE_SOURCE_DIR}/src/utils/*.cc
)

add_executable(pulse_backend
  ${CMAKE_SOURCE_DIR}/src/main.cc
  ${ALGO_SOURCES}
  ${APP_SOURCES}
)

target_include_directories(pulse_backend BEFORE PRIVATE
  # IMPORTANT: third_party/bcrypt is listed FIRST (and BEFORE system dirs) so
  # `#include <bcrypt.h>` picks up OUR vendored header, never the Windows SDK's
  # bcrypt.h (BCryptGenRandom et al.).
  ${PULSE_BCRYPT_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/include/pulse   # so the kernels' #include "common.hpp"/"json.hpp" resolve unchanged
)

target_link_libraries(pulse_backend PRIVATE
  Drogon::Drogon
  ${PULSE_MONGOCXX_TARGET}
  ${PULSE_BSONCXX_TARGET}
  ${PULSE_REDISPP_TARGET}
  jwt-cpp::jwt-cpp
  OpenSSL::SSL
  OpenSSL::Crypto
  ZLIB::ZLIB
  spdlog::spdlog
  fmt::fmt
  Threads::Threads
  pulse_bcrypt
)

if(PULSE_JSONCPP_TARGET)
  target_link_libraries(pulse_backend PRIVATE ${PULSE_JSONCPP_TARGET})
endif()

# ── Tests (optional) ──
option(PULSE_BUILD_TESTS "Build unit tests" OFF)
if(PULSE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

install(TARGETS pulse_backend RUNTIME DESTINATION bin)
