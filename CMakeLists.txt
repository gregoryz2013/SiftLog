cmake_minimum_required(VERSION 3.25)
project(SiftLog VERSION 0.5.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/O2 /W4)
else()
    add_compile_options(-O3 -Wall -Wextra -Wno-unused-parameter)
endif()

find_package(Threads REQUIRED)
include(FetchContent)

# 1. nlohmann_json
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)

# 2. cpp-httplib
set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "" FORCE)
set(HTTPLIB_REQUIRE_ZLIB OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.18.3
    GIT_SHALLOW TRUE
)

# 3. zstd
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG v1.5.6
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR build/cmake
)

message(STATUS "Fetching dependencies...")
FetchContent_MakeAvailable(nlohmann_json httplib zstd)

set(SIFTLOG_SOURCES
    src/main.cpp
    src/storage/log_storage.cpp
    src/storage/s3_client.cpp
    src/search/query_parser.cpp
    src/alerting/alert_manager.cpp
    src/api/api_server.cpp
    src/security/pii_masker.cpp
    src/enterprise/license_manager.cpp
    src/enterprise/updater.cpp
    src/enterprise/audit_ledger.cpp
    src/enterprise/noise_filter.cpp
    src/enterprise/rca_engine.cpp
)

add_executable(siftlog ${SIFTLOG_SOURCES})

target_include_directories(siftlog PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(siftlog PRIVATE
    nlohmann_json::nlohmann_json
    httplib::httplib
    libzstd_static
    Threads::Threads
)

if(WIN32)
    target_link_libraries(siftlog PRIVATE ws2_32 crypt32 winhttp)
    if(MINGW)
        target_link_options(siftlog PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
endif()
