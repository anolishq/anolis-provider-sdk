cmake_minimum_required(VERSION 3.20)

option(ANOLIS_PROVIDER_SDK_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" OFF)

project(anolis_provider_sdk VERSION 0.1.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(CTest)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Warnings.cmake)

# ThreadSanitizer (TSAN) — data-race detection. The SDK is the home for the
# providers' spine; the lane is forward-looking for the planned worker-thread
# i2c-serialization convergence. Deps are rebuilt with TSAN via x64-linux-tsan.
option(ENABLE_TSAN "Enable ThreadSanitizer for data race detection" OFF)

if(ENABLE_TSAN)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        if(NOT WIN32)
            add_compile_options(-fsanitize=thread -g -fno-omit-frame-pointer)
            add_link_options(-fsanitize=thread)
            message(STATUS "ThreadSanitizer (TSAN) enabled for data race detection")
        else()
            message(WARNING "ThreadSanitizer not supported on Windows (Linux/macOS only)")
        endif()
    else()
        message(WARNING "ThreadSanitizer requested but compiler not supported: ${CMAKE_CXX_COMPILER_ID}")
    endif()
# Fallback: detect TSAN triplet automatically.
elseif(DEFINED VCPKG_TARGET_TRIPLET AND VCPKG_TARGET_TRIPLET MATCHES "tsan")
    if(NOT WIN32)
        message(STATUS "ThreadSanitizer auto-detected via triplet: ${VCPKG_TARGET_TRIPLET}")
        add_compile_options(-fsanitize=thread -g -fno-omit-frame-pointer)
        add_link_options(-fsanitize=thread)
    endif()
endif()

if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(_provider_triplet "${VCPKG_TARGET_TRIPLET}")
    if(NOT _provider_triplet OR _provider_triplet MATCHES "windows")
        if(NOT _provider_triplet MATCHES "mingw")
            message(FATAL_ERROR
                "Unsupported Windows toolchain/triplet combination:\n"
                "  compiler: ${CMAKE_CXX_COMPILER_ID}\n"
                "  VCPKG_TARGET_TRIPLET: '${_provider_triplet}'\n"
                "Use MSVC presets (`dev-windows-debug`, `dev-windows-release`),\n"
                "or select an explicit MinGW triplet (e.g. x64-mingw-dynamic)."
            )
        endif()
    endif()
endif()

find_package(Protobuf REQUIRED)
find_package(Threads REQUIRED)

# --- ADPP proto re-export -------------------------------------------------
# The SDK is the SINGLE declarant of the anolis-protocol pin + the protobuf/vcpkg
# baseline for the fleet. Providers stop pinning the proto once they consume the
# SDK; they link anolis::adpp_proto instead. (Block lifted from the providers'
# byte-identical proto setup so the generated layout + protocol.pb.h shim match.)
include(FetchContent)
FetchContent_Declare(
    anolis_protocol
    URL https://github.com/anolishq/anolis-protocol/releases/download/v1.6.0/anolis-protocol-1.6.0-source.tar.gz
    URL_HASH SHA256=80b3db19d5249ae5f921a28873055c9a73e5579a3cab7eaa59bc75d39fedcec4
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(anolis_protocol)
set(ADPP_PROTO_IMPORT_ROOT "${anolis_protocol_SOURCE_DIR}/proto")
set(ADPP_PROTO_V1_DIR "${ADPP_PROTO_IMPORT_ROOT}/anolis/deviceprovider/v1")
set(ADPP_PROTO_ENVELOPE "${ADPP_PROTO_V1_DIR}/envelope.proto")

if(NOT EXISTS "${ADPP_PROTO_ENVELOPE}")
    message(FATAL_ERROR
        "ADPP proto not found at: ${ADPP_PROTO_ENVELOPE}\n"
        "FetchContent source dir: ${anolis_protocol_SOURCE_DIR}\n"
        "Set FETCHCONTENT_SOURCE_DIR_ANOLIS_PROTOCOL to use a local clone."
    )
endif()

file(GLOB ADPP_PROTOS "${ADPP_PROTO_V1_DIR}/*.proto")
if(NOT ADPP_PROTOS)
    message(FATAL_ERROR
        "No ADPP v1 proto files found under: ${ADPP_PROTO_V1_DIR}\n"
        "Expected split protocol layout from anolis-protocol PR #1."
    )
endif()

add_library(adpp_proto OBJECT ${ADPP_PROTOS})
target_link_libraries(adpp_proto PUBLIC protobuf::libprotobuf)
target_include_directories(adpp_proto SYSTEM PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")

protobuf_generate(
    TARGET adpp_proto
    LANGUAGE cpp
    IMPORT_DIRS "${ADPP_PROTO_IMPORT_ROOT}"
    PROTOC_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}"
)

# Compatibility shim: keep existing includes of "protocol.pb.h" working after proto split.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/protocol.pb.h"
"#pragma once\n"
"#include \"anolis/deviceprovider/v1/call.pb.h\"\n"
"#include \"anolis/deviceprovider/v1/envelope.pb.h\"\n"
"#include \"anolis/deviceprovider/v1/handshake.pb.h\"\n"
"#include \"anolis/deviceprovider/v1/health.pb.h\"\n"
"#include \"anolis/deviceprovider/v1/inventory.pb.h\"\n"
"#include \"anolis/deviceprovider/v1/readiness.pb.h\"\n"
"#include \"anolis/deviceprovider/v1/status.pb.h\"\n"
"#include \"anolis/deviceprovider/v1/telemetry.pb.h\"\n"
"#include \"anolis/deviceprovider/v1/types.pb.h\"\n"
"#include \"anolis/deviceprovider/v1/value.pb.h\"\n")

add_library(anolis::adpp_proto ALIAS adpp_proto)

# --- Generated version header ---------------------------------------------
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/anolis/provider_sdk/version.hpp"
    @ONLY
)

# --- The SDK library ------------------------------------------------------
# STATIC (not header-only): the spine — transport, run-loop, handler skeleton,
# config validators, logging — is real compiled code. It carries the header-only
# DeviceAdapter<HandleT> framework on its PUBLIC include interface. Consumed as
# source/FetchContent so TSAN/static/arm64 rebuild protobuf in-tree.
add_library(anolis_provider_sdk STATIC
    src/anolis/provider_sdk/version.cpp
    src/anolis/provider_sdk/transport.cpp
    src/anolis/provider_sdk/logging.cpp
    src/anolis/provider_sdk/handlers.cpp
    src/anolis/provider_sdk/runtime.cpp
)
add_library(anolis::provider_sdk ALIAS anolis_provider_sdk)

# Public headers live under src/ with the fleet-wide `-I src` convention (the same
# layout as anolis-provider-{ezo,sim,bread}); the SDK is consumed from source so
# there is no public/private install boundary to separate. Generated headers
# (version.hpp) come from the build tree.
target_include_directories(anolis_provider_sdk PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
    "$<INSTALL_INTERFACE:src>"
)
target_link_libraries(anolis_provider_sdk PUBLIC
    anolis::adpp_proto
    protobuf::libprotobuf
    Threads::Threads
)
target_compile_features(anolis_provider_sdk PUBLIC cxx_std_20)
anolis_provider_sdk_apply_warnings(anolis_provider_sdk)

if(WIN32)
    target_compile_definitions(anolis_provider_sdk PUBLIC _CRT_SECURE_NO_WARNINGS)
endif()

# Build the SDK's own test suite only when it is the top-level project — not when
# a provider consumes it via FetchContent/add_subdirectory (where BUILD_TESTING is
# the consumer's, and pulling in the SDK's gtest tree would be wrong). Consumers
# can still force it on with -DANOLIS_PROVIDER_SDK_BUILD_TESTS=ON.
# (CMAKE_SOURCE_DIR == CMAKE_CURRENT_SOURCE_DIR is the pre-3.21 PROJECT_IS_TOP_LEVEL.)
if(NOT DEFINED ANOLIS_PROVIDER_SDK_BUILD_TESTS)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(ANOLIS_PROVIDER_SDK_BUILD_TESTS ON)
    else()
        set(ANOLIS_PROVIDER_SDK_BUILD_TESTS OFF)
    endif()
endif()

if(BUILD_TESTING AND ANOLIS_PROVIDER_SDK_BUILD_TESTS)
    find_package(GTest CONFIG REQUIRED)
    add_subdirectory(tests/unit)
endif()
