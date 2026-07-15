cmake_minimum_required(VERSION 3.20)
project(AetherProxy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
FetchContent_MakeAvailable(datachannel)

# Fetch libqrencode
FetchContent_Declare(
    qrencode
    GIT_REPOSITORY https://github.com/fukuchi/libqrencode.git
    GIT_TAG v4.1.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(qrencode)

# Fetch nlohmann_json
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.2/json.tar.xz
)
FetchContent_MakeAvailable(json)

# Add executable
add_executable(aetherproxy
    src/host/main.cpp
    src/common/io_source.hpp
    src/common/pty_source.cpp
    src/common/stdin_source.cpp
    src/common/event_queue.cpp
    src/common/room_code.cpp
    src/common/qr_code.cpp
    src/common/signaling_client.cpp
    src/common/webrtc_session.cpp
    src/host/http_server.cpp
    src/host/session_registry.cpp
    src/client/native_client.cpp
    src/common/config.cpp
)

target_include_directories(aetherproxy PRIVATE src ${qrencode_SOURCE_DIR})

# Link dependencies
target_link_libraries(aetherproxy PRIVATE datachannel qrencode util nlohmann_json::nlohmann_json)

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

# Static build support options
option(STATIC_BUILD "Build static binary" OFF)
if(STATIC_BUILD)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
    set(BUILD_SHARED_LIBS OFF)
    target_link_options(aetherproxy PRIVATE -static)
endif()
