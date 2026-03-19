cmake_minimum_required(VERSION 3.20)
project(tracker_backend LANGUAGES CXX)

set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT (UNIX AND NOT APPLE))
    message(FATAL_ERROR "仅支持 Linux")
endif()

add_compile_options(-O2 -g -Wall -Wextra -Wpedantic)

find_package(OpenSSL REQUIRED)

set(TRACKER_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(TRACKER_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(CORE_BACKEND_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../core-backend/include)
set(TRACKER_COMPILE_COMMANDS_LINK ${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json)

file(REMOVE "${TRACKER_COMPILE_COMMANDS_LINK}")
file(CREATE_LINK
    "${CMAKE_BINARY_DIR}/compile_commands.json"
    "${TRACKER_COMPILE_COMMANDS_LINK}"
    SYMBOLIC
)

add_executable(tracker_backend
    ${TRACKER_SRC_DIR}/main.cpp
    ${TRACKER_SRC_DIR}/config.cpp
    ${TRACKER_SRC_DIR}/api.cpp
    ${TRACKER_SRC_DIR}/http.cpp
    ${TRACKER_SRC_DIR}/store.cpp
    ${TRACKER_SRC_DIR}/sync.cpp
    ${TRACKER_SRC_DIR}/ws.cpp
)

target_compile_options(tracker_backend PRIVATE
    $<$<CONFIG:Release>:-UNDEBUG>
    $<$<CONFIG:RelWithDebInfo>:-UNDEBUG>
    $<$<CONFIG:MinSizeRel>:-UNDEBUG>
)

target_compile_definitions(tracker_backend PRIVATE
    BOOST_ERROR_CODE_HEADER_ONLY
)

target_include_directories(tracker_backend PRIVATE
    ${CORE_BACKEND_INCLUDE_DIR}
    ${TRACKER_INCLUDE_DIR}
)

target_link_libraries(tracker_backend PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
)
