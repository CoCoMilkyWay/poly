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

find_package(Boost REQUIRED COMPONENTS system thread)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json 3.2.0 REQUIRED)

set(TRACKER_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(TRACKER_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
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
    ${TRACKER_SRC_DIR}/http_client.cpp
    ${TRACKER_SRC_DIR}/json_store.cpp
    ${TRACKER_SRC_DIR}/tracker_service.cpp
    ${TRACKER_SRC_DIR}/http_server.cpp
)

target_include_directories(tracker_backend PRIVATE
    ${TRACKER_INCLUDE_DIR}
)

target_link_libraries(tracker_backend PRIVATE
    Boost::system
    Boost::thread
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    pthread
)
