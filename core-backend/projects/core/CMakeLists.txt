cmake_minimum_required(VERSION 3.16)
project(core LANGUAGES C CXX)

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 平台检查
if(NOT (UNIX AND NOT APPLE))
    message(FATAL_ERROR "仅支持 Linux")
endif()

# ============================================================================
# Build Mode Options
# ============================================================================
option(PROFILE_MODE "Profile mode: enable Tracy profiling" OFF)

# 编译选项
if(PROFILE_MODE)
    message(STATUS "Build mode: PROFILE (tracy instrumentation)")
    add_compile_definitions(PROFILE_MODE TRACY_ENABLE TRACY_NO_SYSTEM_TRACING TRACY_NO_SAMPLING TRACY_TIMER_FALLBACK)
    add_compile_options(-O2 -g -march=native)
else()
    message(STATUS "Build mode: PRODUCTION")
    add_compile_definitions(NDEBUG)
    add_compile_options(-O2 -march=native)
endif()

# 本地路径
set(PACKAGES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../packages)
set(INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../src)

# OpenSSL (for boost::asio::ssl)
find_package(OpenSSL QUIET)
if(NOT OpenSSL_FOUND)
    message(FATAL_ERROR "未找到 OpenSSL\n"
        "  Ubuntu/Debian: sudo apt install libssl-dev\n"
        "  Fedora/RHEL:   sudo dnf install openssl-devel")
endif()

# libcurl (for curl rpc transport)
find_package(CURL REQUIRED)

# Arrow (vendored under packages/arrow)
set(ARROW_DIR ${PACKAGES_DIR}/arrow)
set(ARROW_INCLUDE_DIR ${ARROW_DIR}/include)
set(ARROW_LIBRARY ${ARROW_DIR}/lib/libarrow.so)

if(NOT EXISTS ${ARROW_INCLUDE_DIR}/arrow/api.h)
    message(FATAL_ERROR "未找到 Arrow 头文件: ${ARROW_INCLUDE_DIR}/arrow/api.h")
endif()
if(NOT EXISTS ${ARROW_LIBRARY})
    message(FATAL_ERROR "未找到 Arrow 库文件: ${ARROW_LIBRARY}")
endif()

# ONNX Runtime: CMake 配置阶段自动选择, 优先 GPU, 其次 CPU
set(ONNXRUNTIME_GPU_DIR ${PACKAGES_DIR}/onnxruntime-gpu)
set(ONNXRUNTIME_CPU_DIR ${PACKAGES_DIR}/onnxruntime-cpu)
if(EXISTS ${ONNXRUNTIME_GPU_DIR}/libonnxruntime.so)
    set(ONNXRUNTIME_DIR ${ONNXRUNTIME_GPU_DIR})
    set(ONNXRUNTIME_MODE "gpu")
elseif(EXISTS ${ONNXRUNTIME_CPU_DIR}/libonnxruntime.so)
    set(ONNXRUNTIME_DIR ${ONNXRUNTIME_CPU_DIR})
    set(ONNXRUNTIME_MODE "cpu")
else()
    message(FATAL_ERROR
        "未找到 ONNX Runtime 库文件，请提供以下任一目录:\n"
        "  - ${ONNXRUNTIME_GPU_DIR}/libonnxruntime.so\n"
        "  - ${ONNXRUNTIME_CPU_DIR}/libonnxruntime.so")
endif()
set(ONNXRUNTIME_LIBRARY ${ONNXRUNTIME_DIR}/libonnxruntime.so)
message(STATUS "ONNX Runtime mode: ${ONNXRUNTIME_MODE} (${ONNXRUNTIME_LIBRARY})")

# 源文件
set(SOURCES
    ${SRC_DIR}/main.cpp
    ${SRC_DIR}/api/api_session.cpp
    ${SRC_DIR}/core/database.cpp
    ${SRC_DIR}/infra/rpc_client.cpp
    ${SRC_DIR}/infra/rpc_transport_beast.cpp
    ${SRC_DIR}/infra/rpc_transport_curl.cpp
    ${SRC_DIR}/stage0/stage0_query_sync.cpp
    ${SRC_DIR}/stage0/stage0_query_sync_http.cpp
    ${SRC_DIR}/stage0/stage0_query_sync_persist.cpp
    ${SRC_DIR}/stage0/stage0_query_mem.cpp
    ${SRC_DIR}/stage0/stage0_mem.cpp
    ${SRC_DIR}/stage0/stage0_tag_sync.cpp
    ${SRC_DIR}/stage0/stage0_tag_mem.cpp
    ${SRC_DIR}/stage0/stage0_tag.cpp
    ${SRC_DIR}/stage0/stage0_tagger_mem.cpp
    ${SRC_DIR}/stage1/stage1_sync.cpp
    ${SRC_DIR}/stage1/stage1_mem.cpp
    ${SRC_DIR}/stage1/event_decode.cpp
    ${SRC_DIR}/stage2/stage2_utils.cpp
    ${SRC_DIR}/stage2/stage2_models.cpp
    ${SRC_DIR}/stage2/stage2_builder.cpp
    ${SRC_DIR}/stage2/stage2_builder_restore.cpp
    ${SRC_DIR}/stage2/stage2_mem.cpp
    ${SRC_DIR}/stage2/stage2_builder_classify.cpp
    ${SRC_DIR}/stage2/stage2_builder_phases.cpp
    ${SRC_DIR}/stage2/stage2_sync.cpp
    ${SRC_DIR}/stage2/stage2_sync_mem.cpp
    ${SRC_DIR}/stage3/stage3_store.cpp
    ${SRC_DIR}/stage3/stage3_trade.cpp
    ${SRC_DIR}/stage3/stage3_feature.cpp
    ${SRC_DIR}/stage3/stage3_sharpe.cpp
    ${SRC_DIR}/stage3/stage3_query.cpp
    ${SRC_DIR}/stage3/stage3_filter.cpp
    ${SRC_DIR}/stage3/stage3_sync.cpp
    ${SRC_DIR}/stage3/stage3_sync_service.cpp
)

if(PROFILE_MODE)
    list(APPEND SOURCES ${PACKAGES_DIR}/tracy/public/TracyClient.cpp)
endif()

add_executable(core ${SOURCES})

if(ONNXRUNTIME_MODE STREQUAL "gpu")
    target_compile_definitions(core PRIVATE POLY_ORT_LIB_GPU=1)
else()
    target_compile_definitions(core PRIVATE POLY_ORT_LIB_CPU=1)
endif()

target_include_directories(core PRIVATE
    ${SRC_DIR}
    ${PACKAGES_DIR}/duckdb
    ${PACKAGES_DIR}/rocksdb/include
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
target_link_libraries(core PRIVATE
    ${PACKAGES_DIR}/duckdb/libduckdb.so
    ${PACKAGES_DIR}/rocksdb/lib/librocksdb.a
    ${PACKAGES_DIR}/rocksdb/lib/libz.a
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
