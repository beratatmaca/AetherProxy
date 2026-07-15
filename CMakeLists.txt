cmake_minimum_required(VERSION 3.20)
project(AetherProxy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Static build. Must be set before dependencies are fetched.
option(STATIC_BUILD "Build static binary" OFF)
if(STATIC_BUILD)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
    set(BUILD_SHARED_LIBS OFF)
endif()

# Include FetchContent for dependencies
include(FetchContent)

# Fetch libdatachannel
FetchContent_Declare(
    datachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG v0.21.0
    GIT_SHALLOW TRUE
)
set(NO_WEBSOCKETS OFF CACHE INTERNAL "")
set(NO_TESTS ON CACHE INTERNAL "")
set(NO_MEDIA ON CACHE INTERNAL "")
set(NO_EXAMPLES ON CACHE INTERNAL "")
set(BUILD_TESTING OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(datachannel)

# Fetch libqrencode
FetchContent_Declare(
    qrencode
    GIT_REPOSITORY https://github.com/fukuchi/libqrencode.git
    GIT_TAG v4.1.1
    GIT_SHALLOW TRUE
)
set(WITH_TOOLS OFF CACHE INTERNAL "")
set(WITH_TESTS OFF CACHE INTERNAL "")
set(WITHOUT_PNG ON CACHE INTERNAL "")
FetchContent_MakeAvailable(qrencode)

# libdatachannel already vendors and exports nlohmann_json
if(NOT TARGET nlohmann_json)
    FetchContent_Declare(
        json
        URL https://github.com/nlohmann/json/releases/download/v3.11.2/json.tar.xz
    )
    FetchContent_MakeAvailable(json)
endif()

# Add executable
add_executable(aetherproxy
    src/host/main.cpp
    src/common/io_source.hpp
    src/common/pty_source.cpp
    src/common/stdin_source.cpp
    src/common/event_queue.cpp
    src/common/room_code.cpp
    src/common/qr_code.cpp
    src/common/banner.cpp
    src/common/raw_tty.cpp
    src/common/signaling_client.cpp
    src/common/webrtc_session.cpp
    src/host/http_server.cpp
    src/host/session_registry.cpp
    src/client/native_client.cpp
    src/common/config.cpp
)

file(READ ${CMAKE_CURRENT_SOURCE_DIR}/VERSION AETHER_VERSION)
string(STRIP "${AETHER_VERSION}" AETHER_VERSION)
target_compile_definitions(aetherproxy PRIVATE AETHER_VERSION="${AETHER_VERSION}")

target_include_directories(aetherproxy PRIVATE src ${qrencode_SOURCE_DIR})

# Link dependencies. libdatachannel ships a separate static target.
if(STATIC_BUILD)
    target_link_libraries(aetherproxy PRIVATE datachannel-static qrencode nlohmann_json::nlohmann_json)
else()
    target_link_libraries(aetherproxy PRIVATE datachannel qrencode nlohmann_json::nlohmann_json)
endif()

# musl folds forkpty into libc. Link libutil only where it exists.
find_library(UTIL_LIB util)
if(UTIL_LIB)
    target_link_libraries(aetherproxy PRIVATE util)
endif()

# Asset embedding custom command
add_custom_target(embed_assets
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed-assets.sh
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Compiling web assets into C++ header"
)
add_dependencies(aetherproxy embed_assets)

# Debug build: AddressSanitizer
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(aetherproxy PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(aetherproxy PRIVATE -fsanitize=address)
endif()

# Tsan build: ThreadSanitizer (mutually exclusive with ASan)
if(CMAKE_BUILD_TYPE STREQUAL "Tsan")
    target_compile_options(aetherproxy PRIVATE -fsanitize=thread -fno-omit-frame-pointer -g)
    target_link_options(aetherproxy PRIVATE -fsanitize=thread)
endif()

# Static link flag. The option itself is declared near the top.
if(STATIC_BUILD)
    target_link_options(aetherproxy PRIVATE -static)
endif()

# ################ developer tool targets ################

file(GLOB_RECURSE AETHER_ALL_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp
)
list(REMOVE_ITEM AETHER_ALL_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/common/embedded_assets.hpp)
file(GLOB_RECURSE AETHER_CPP_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

# Format sources in place
find_program(CLANG_FORMAT_BIN clang-format)
if(CLANG_FORMAT_BIN)
    add_custom_target(format
        COMMAND ${CLANG_FORMAT_BIN} -i ${AETHER_ALL_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Formatting sources with clang-format"
    )
    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT_BIN} --dry-run --Werror ${AETHER_ALL_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Checking formatting with clang-format"
    )
endif()

# Static analysis via compile_commands.json
find_program(CLANG_TIDY_BIN clang-tidy)
if(CLANG_TIDY_BIN)
    add_custom_target(lint
        COMMAND ${CLANG_TIDY_BIN} -p ${CMAKE_BINARY_DIR} ${AETHER_CPP_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Linting sources with clang-tidy"
    )
endif()

# API docs from doxygen header comments
find_package(Doxygen QUIET)
if(DOXYGEN_FOUND)
    set(DOXYGEN_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/docs)
    set(DOXYGEN_EXCLUDE_PATTERNS embedded_assets.hpp)
    set(DOXYGEN_EXTRACT_ALL YES)
    set(DOXYGEN_GENERATE_LATEX NO)
    doxygen_add_docs(docs
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        COMMENT "Generating API docs with Doxygen"
    )
endif()

# Dispatch CI on GitHub Actions
find_program(GH_BIN gh)
if(GH_BIN)
    add_custom_target(ci
        COMMAND ${GH_BIN} workflow run ci.yml
        COMMAND ${CMAKE_COMMAND} -E echo "CI dispatched. Watch with: gh run watch"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        USES_TERMINAL
        COMMENT "Dispatching GitHub Actions CI workflow"
    )
endif()

# Playwright integration suite
add_custom_target(run-tests
    COMMAND ${CMAKE_COMMAND} -E env BINARY=$<TARGET_FILE:aetherproxy>
            bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/run-tests.sh
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    USES_TERMINAL
    COMMENT "Running integration tests"
)
add_dependencies(run-tests aetherproxy)
