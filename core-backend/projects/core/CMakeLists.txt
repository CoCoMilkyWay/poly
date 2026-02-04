cmake_minimum_required(VERSION 3.16)
project(core LANGUAGES C CXX)

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 平台检查
if(NOT (WIN32 OR (UNIX AND NOT APPLE)))
    message(FATAL_ERROR "仅支持 Linux 和 Windows")
endif()

# 本地路径
set(PACKAGES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../packages)
set(INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../src)

# OpenSSL (for boost::asio::ssl)
find_package(OpenSSL QUIET)
if(NOT OpenSSL_FOUND)
    if(WIN32)
        message(FATAL_ERROR "未找到 OpenSSL\n"
            "  Windows: 下载安装 https://slproweb.com/products/Win32OpenSSL.html\n"
            "  或设置 OPENSSL_ROOT_DIR 环境变量")
    else()
        message(FATAL_ERROR "未找到 OpenSSL\n"
            "  Ubuntu/Debian: sudo apt install libssl-dev\n"
            "  Fedora/RHEL:   sudo dnf install openssl-devel")
    endif()
endif()

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
else()
    target_link_libraries(core PRIVATE
        ${PACKAGES_DIR}/duckdb/libduckdb.so
        OpenSSL::SSL
        OpenSSL::Crypto
        pthread
        dl
    )
    set_target_properties(core PROPERTIES
        BUILD_RPATH ${PACKAGES_DIR}/duckdb
        INSTALL_RPATH ${PACKAGES_DIR}/duckdb
    )
endif()
