cmake_minimum_required(VERSION 3.15)
project(duckdb_property_tests CXX)

# Property-based tests for DuckDB using RapidCheck
# (https://github.com/emil-e/rapidcheck).
#
# This is a standalone project that links against an existing DuckDB build
# directory:
#
# cmake -S test/property -B build/property -DDUCKDB_BUILD_DIR=build/relassert
# cmake --build build/property RC_PARAMS="max_success=500"
# build/property/property_test
#
# RapidCheck is fetched via FetchContent unless RAPIDCHECK_SOURCE_DIR points to
# a checkout.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

get_filename_component(DUCKDB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(DUCKDB_BUILD_DIR
    "${DUCKDB_ROOT}/build/relassert"
    CACHE PATH "DuckDB build directory containing src/libduckdb")
set(RAPIDCHECK_SOURCE_DIR
    ""
    CACHE PATH "Local RapidCheck checkout (optional)")
option(
  PROPERTY_TEST_SANITIZE
  "Build with -fsanitize=address,undefined (required when linking a sanitized libduckdb)"
  ON)

if(NOT EXISTS "${DUCKDB_BUILD_DIR}/src")
  message(
    FATAL_ERROR
      "DUCKDB_BUILD_DIR=${DUCKDB_BUILD_DIR} does not look like a DuckDB build directory"
  )
endif()

# Locate the shared library
find_library(
  DUCKDB_LIBRARY
  NAMES duckdb
  PATHS "${DUCKDB_BUILD_DIR}/src"
  NO_DEFAULT_PATH)
if(NOT DUCKDB_LIBRARY)
  message(FATAL_ERROR "Could not find libduckdb in ${DUCKDB_BUILD_DIR}/src")
endif()

# RapidCheck
if(RAPIDCHECK_SOURCE_DIR)
  add_subdirectory("${RAPIDCHECK_SOURCE_DIR}"
                   "${CMAKE_CURRENT_BINARY_DIR}/rapidcheck" EXCLUDE_FROM_ALL)
else()
  include(FetchContent)
  FetchContent_Declare(
    rapidcheck
    GIT_REPOSITORY https://github.com/emil-e/rapidcheck.git
    GIT_TAG master
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(rapidcheck)
  set(RAPIDCHECK_SOURCE_DIR "${rapidcheck_SOURCE_DIR}")
endif()

# RapidCheck's Catch adapter includes <catch2/catch.hpp>; DuckDB vendors Catch
# v2 at third_party/catch/catch.hpp.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/catch_shim/catch2")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/catch_shim/catch2/catch.hpp"
     "#include \"${DUCKDB_ROOT}/third_party/catch/catch.hpp\"\n")

file(GLOB PROPERTY_TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
add_executable(property_test ${PROPERTY_TEST_SOURCES})
target_include_directories(
  property_test
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include"
          "${DUCKDB_ROOT}/src/include"
          "${DUCKDB_ROOT}/third_party/catch"
          "${DUCKDB_ROOT}/third_party/fmt/include"
          "${CMAKE_CURRENT_BINARY_DIR}/catch_shim"
          "${RAPIDCHECK_SOURCE_DIR}/extras/catch/include")
target_link_libraries(property_test PRIVATE rapidcheck "${DUCKDB_LIBRARY}")
target_compile_definitions(property_test
                           PRIVATE DUCKDB_ROOT_DIR="${DUCKDB_ROOT}")
if(PROPERTY_TEST_SANITIZE)
  target_compile_options(property_test PRIVATE -fsanitize=address,undefined
                                               -fno-omit-frame-pointer)
  target_link_options(property_test PRIVATE -fsanitize=address,undefined)
endif()
