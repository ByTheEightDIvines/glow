cmake_minimum_required(VERSION 3.26)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT DEFINED CMAKE_BUILD_TYPE AND NOT DEFINED ENV{CMAKE_BUILD_TYPE})
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type")
endif ()

project(mastersword_glow VERSION 1.0.1 LANGUAGES CXX)

# The Dusklight version to build against: a release tag (e.g. "v2.0.0") or a commit SHA.
if (NOT DUSKLIGHT_VERSION)
    set(DUSKLIGHT_VERSION "v2.0.3")
endif ()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchDusklight.cmake")
add_subdirectory("${DUSKLIGHT_DIR}/sdk" dusklight-sdk EXCLUDE_FROM_ALL)

configure_file(mod.json.in "${CMAKE_CURRENT_BINARY_DIR}/mod.json" @ONLY NEWLINE_STYLE LF)

add_mod(mastersword_glow
        FEATURES game
        SOURCES src/mod.cpp
        MOD_JSON "${CMAKE_CURRENT_BINARY_DIR}/mod.json"
)

# MSVC reports __cplusplus as 199711L unless told otherwise, which makes the Dolphin
# compatibility headers redefine nullptr and breaks the standard library.
if (MSVC)
    target_compile_options(mastersword_glow PRIVATE /Zc:__cplusplus)
endif ()
