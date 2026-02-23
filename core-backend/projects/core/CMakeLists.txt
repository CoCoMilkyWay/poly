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

# ============================================================================
# Build Mode Options
# ============================================================================
option(PROFILE_MODE "Profile mode: enable Tracy profiling" OFF)

# 编译选项
if(PROFILE_MODE)
    message(STATUS "Build mode: PROFILE (tracy instrumentation)")
    add_compile_definitions(PROFILE_MODE TRACY_ENABLE TRACY_NO_SYSTEM_TRACING TRACY_NO_SAMPLING)
    if(WIN32)
        add_compile_options(/O2 /Zi -march=native)
    else()
        add_compile_options(-O2 -g -march=native)
    endif()
else()
    message(STATUS "Build mode: PRODUCTION")
    add_compile_definitions(NDEBUG)
    if(WIN32)
        add_compile_options(/O2 -march=native)
    else()
        add_compile_options(-O2 -march=native)
    endif()
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

# Arrow (for Feather file writing)
find_package(Arrow REQUIRED)
message(STATUS "Found Arrow: ${Arrow_VERSION}")

# 源文件
set(SOURCES
    ${SRC_DIR}/main.cpp
    ${SRC_DIR}/stage2/event_build_classify.cpp
    ${SRC_DIR}/stage2/event_build_phases.cpp
)

if(PROFILE_MODE)
    list(APPEND SOURCES ${PACKAGES_DIR}/tracy/public/TracyClient.cpp)
endif()

add_executable(core ${SOURCES})

target_include_directories(core PRIVATE
    ${SRC_DIR}
    ${PACKAGES_DIR}/duckdb
    ${PACKAGES_DIR}/tracy/public/tracy
    ${INCLUDE_DIR}
    ${INCLUDE_DIR}/package
)

# clangd: copy compile_commands.json to project dir after each build
add_custom_command(TARGET core POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_BINARY_DIR}/compile_commands.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json"
    VERBATIM
)

# 链接库
if(WIN32)
    target_link_libraries(core PRIVATE
        ${PACKAGES_DIR}/duckdb/duckdb.lib
        OpenSSL::SSL
        OpenSSL::Crypto
        Arrow::arrow_shared
    )
    if(PROFILE_MODE)
        target_link_libraries(core PRIVATE ws2_32 dbghelp)
    endif()
else()
    target_link_libraries(core PRIVATE
        ${PACKAGES_DIR}/duckdb/libduckdb.so
        OpenSSL::SSL
        OpenSSL::Crypto
        Arrow::arrow_shared
        pthread
        dl
    )
    set_target_properties(core PROPERTIES
        BUILD_RPATH ${PACKAGES_DIR}/duckdb
        INSTALL_RPATH ${PACKAGES_DIR}/duckdb
    )
endif()
