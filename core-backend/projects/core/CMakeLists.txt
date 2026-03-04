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
    add_compile_definitions(PROFILE_MODE TRACY_ENABLE TRACY_NO_SYSTEM_TRACING TRACY_NO_SAMPLING TRACY_TIMER_FALLBACK)
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

# libcurl (for curl rpc transport)
find_package(CURL REQUIRED)

# Arrow (vendored under packages/arrow)
set(ARROW_DIR ${PACKAGES_DIR}/arrow)
set(ARROW_INCLUDE_DIR ${ARROW_DIR}/include)
if(WIN32)
    set(ARROW_LIBRARY ${ARROW_DIR}/lib/arrow.lib)
else()
    set(ARROW_LIBRARY ${ARROW_DIR}/lib/libarrow.so)
endif()

if(NOT EXISTS ${ARROW_INCLUDE_DIR}/arrow/api.h)
    message(FATAL_ERROR "未找到 Arrow 头文件: ${ARROW_INCLUDE_DIR}/arrow/api.h")
endif()
if(NOT EXISTS ${ARROW_LIBRARY})
    message(FATAL_ERROR "未找到 Arrow 库文件: ${ARROW_LIBRARY}")
endif()

# ONNX Runtime (vendored under packages/onnxruntime)
set(ONNXRUNTIME_DIR ${PACKAGES_DIR}/onnxruntime)
if(WIN32)
    set(ONNXRUNTIME_LIBRARY ${ONNXRUNTIME_DIR}/onnxruntime.lib)
else()
    set(ONNXRUNTIME_LIBRARY ${ONNXRUNTIME_DIR}/libonnxruntime.so)
endif()
if(NOT EXISTS ${ONNXRUNTIME_LIBRARY})
    message(FATAL_ERROR "未找到 ONNX Runtime 库文件: ${ONNXRUNTIME_LIBRARY}")
endif()

# 源文件
set(SOURCES
    ${SRC_DIR}/main.cpp
    ${SRC_DIR}/api/api_session.cpp
    ${SRC_DIR}/core/database.cpp
    ${SRC_DIR}/infra/rpc_client.cpp
    ${SRC_DIR}/infra/rpc_transport_beast.cpp
    ${SRC_DIR}/infra/rpc_transport_curl.cpp
    ${SRC_DIR}/stage0/stage0_sync.cpp
    ${SRC_DIR}/stage0/stage0_sync_http.cpp
    ${SRC_DIR}/stage0/stage0_sync_persist.cpp
    ${SRC_DIR}/stage0/stage0_tag.cpp
    ${SRC_DIR}/stage1/stage1_sync.cpp
    ${SRC_DIR}/stage1/event_decode.cpp
    ${SRC_DIR}/stage2/stage2_utils.cpp
    ${SRC_DIR}/stage2/stage2_models.cpp
    ${SRC_DIR}/stage2/stage2_builder.cpp
    ${SRC_DIR}/stage2/stage2_builder_classify.cpp
    ${SRC_DIR}/stage2/stage2_builder_phases.cpp
    ${SRC_DIR}/stage2/stage2_sync.cpp
    ${SRC_DIR}/stage3/stage3_sync_schema.cpp
    ${SRC_DIR}/stage3/stage3_sync_runtime.cpp
    ${SRC_DIR}/stage3/stage3_sync_calc.cpp
    ${SRC_DIR}/stage3/stage3_sync_query.cpp
)

if(PROFILE_MODE)
    list(APPEND SOURCES ${PACKAGES_DIR}/tracy/public/TracyClient.cpp)
endif()

add_executable(core ${SOURCES})

target_include_directories(core PRIVATE
    ${SRC_DIR}
    ${PACKAGES_DIR}/duckdb
    ${ARROW_INCLUDE_DIR}
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
        CURL::libcurl
        ${ARROW_LIBRARY}
        ${ONNXRUNTIME_LIBRARY}
    )
    if(PROFILE_MODE)
        target_link_libraries(core PRIVATE ws2_32 dbghelp)
    endif()
else()
    target_link_libraries(core PRIVATE
        ${PACKAGES_DIR}/duckdb/libduckdb.so
        OpenSSL::SSL
        OpenSSL::Crypto
        CURL::libcurl
        ${ARROW_LIBRARY}
        ${ONNXRUNTIME_LIBRARY}
        pthread
        dl
    )
    set_target_properties(core PROPERTIES
        BUILD_RPATH "${PACKAGES_DIR}/duckdb;${ARROW_DIR}/lib;${ONNXRUNTIME_DIR}"
        INSTALL_RPATH "${PACKAGES_DIR}/duckdb;${ARROW_DIR}/lib;${ONNXRUNTIME_DIR}"
    )
endif()
