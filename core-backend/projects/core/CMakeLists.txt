cmake_minimum_required(VERSION 3.16)
project(core LANGUAGES C CXX)

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 本地路径
set(PACKAGES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../packages)
set(INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../src)

# OpenSSL (for boost::asio::ssl)
find_package(OpenSSL REQUIRED)

# 源文件
set(SOURCES
    ${SRC_DIR}/main.cpp
)

add_executable(core ${SOURCES})

target_include_directories(core PRIVATE
    ${SRC_DIR}
    ${PACKAGES_DIR}/duckdb
    ${INCLUDE_DIR}
    ${INCLUDE_DIR}/package
)

# 链接库
if(WIN32)
    target_link_libraries(core PRIVATE
        ${PACKAGES_DIR}/duckdb/duckdb.lib
        OpenSSL::SSL
        OpenSSL::Crypto
    )
elseif(APPLE)
    target_link_libraries(core PRIVATE
        ${PACKAGES_DIR}/duckdb/libduckdb.dylib
        OpenSSL::SSL
        OpenSSL::Crypto
    )
else()
    target_link_libraries(core PRIVATE
        -Wl,--whole-archive
        ${PACKAGES_DIR}/duckdb/libduckdb_static.a
        -Wl,--no-whole-archive
        OpenSSL::SSL
        OpenSSL::Crypto
        pthread
        dl
    )
endif()
