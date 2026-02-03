cmake_minimum_required(VERSION 3.16)
project(syncer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# nlohmann_json
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# DuckDB - 根据平台选择
if(WIN32)
    set(DUCKDB_URL "https://github.com/duckdb/duckdb/releases/download/v1.1.3/libduckdb-windows-amd64.zip")
elseif(APPLE)
    set(DUCKDB_URL "https://github.com/duckdb/duckdb/releases/download/v1.1.3/libduckdb-osx-universal.zip")
else()
    set(DUCKDB_URL "https://github.com/duckdb/duckdb/releases/download/v1.1.3/libduckdb-linux-amd64.zip")
endif()

FetchContent_Declare(
    duckdb
    URL ${DUCKDB_URL}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(duckdb)

# 查找 CURL
find_package(CURL REQUIRED)

# 源文件
set(SOURCES
    src/main.cpp
)

add_executable(syncer ${SOURCES})

target_include_directories(syncer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${duckdb_SOURCE_DIR}
    ${CURL_INCLUDE_DIRS}
)

# 链接库
if(WIN32)
    target_link_libraries(syncer PRIVATE
        ${duckdb_SOURCE_DIR}/duckdb.lib
        ${CURL_LIBRARIES}
        nlohmann_json::nlohmann_json
    )
    # 复制 DuckDB DLL 到输出目录
    add_custom_command(TARGET syncer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${duckdb_SOURCE_DIR}/duckdb.dll"
        $<TARGET_FILE_DIR:syncer>
    )
else()
    target_link_libraries(syncer PRIVATE
        ${duckdb_SOURCE_DIR}/libduckdb.so
        ${CURL_LIBRARIES}
        nlohmann_json::nlohmann_json
    )
endif()
