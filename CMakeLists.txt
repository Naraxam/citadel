cmake_minimum_required(VERSION 3.25)
project(CitadelMTG VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CITADEL_DIST "Self-contained distributable build (static MSVC runtime)" OFF)
if(CITADEL_DIST AND MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

include(FetchContent)

FetchContent_Declare(
    miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG        3.0.2
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(miniz)
if(NOT miniz_POPULATED)
    FetchContent_Populate(miniz)
endif()

if(NOT EXISTS "${miniz_SOURCE_DIR}/miniz_export.h")
    file(WRITE "${miniz_SOURCE_DIR}/miniz_export.h"
        "#pragma once\n"
        "#ifndef MINIZ_EXPORT\n#  define MINIZ_EXPORT\n#endif\n"
        "#ifndef MINIZ_NO_EXPORT\n#  define MINIZ_NO_EXPORT\n#endif\n"
        "#ifndef MINIZ_DEPRECATED\n#  define MINIZ_DEPRECATED\n#endif\n"
        "#ifndef MINIZ_DEPRECATED_EXPORT\n#  define MINIZ_DEPRECATED_EXPORT\n#endif\n"
        "#ifndef MINIZ_DEPRECATED_NO_EXPORT\n#  define MINIZ_DEPRECATED_NO_EXPORT\n#endif\n"
    )
endif()

file(GLOB miniz_SOURCES "${miniz_SOURCE_DIR}/*.c")
add_library(miniz STATIC ${miniz_SOURCES})
target_include_directories(miniz PUBLIC ${miniz_SOURCE_DIR})
# Suppress warnings from miniz
if(MSVC)
    target_compile_options(miniz PRIVATE /W0)
    # Match the rest of the project's CRT. 
    if(NOT CITADEL_DIST)
        target_compile_options(miniz PRIVATE
            $<$<CONFIG:Debug>:/MDd>
            $<$<CONFIG:Release>:/MD>
            $<$<CONFIG:RelWithDebInfo>:/MD>
            $<$<CONFIG:MinSizeRel>:/MD>)
    endif()
else()
    target_compile_options(miniz PRIVATE -w)
endif()

# ── SFML 2.6.x 
FetchContent_Declare(
    SFML
    GIT_REPOSITORY https://github.com/SFML/SFML.git
    GIT_TAG        2.6.1
    GIT_SHALLOW    TRUE
)
# Build audio for in-game sound effects; network remains disabled
set(SFML_BUILD_AUDIO   ON  CACHE BOOL "" FORCE)
set(SFML_BUILD_NETWORK OFF CACHE BOOL "" FORCE)
set(SFML_BUILD_DOC     OFF CACHE BOOL "" FORCE)
set(SFML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SFML)

# ── nlohmann/json (header-only, for ComboDatabase) 
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install    OFF CACHE BOOL "" FORCE)
set(JSON_CI         OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

# ── pybind11 (Python training backend) 
find_package(Python3 COMPONENTS Interpreter Development QUIET)
if(Python3_FOUND)
    FetchContent_Declare(
        pybind11
        GIT_REPOSITORY https://github.com/pybind/pybind11.git
        GIT_TAG        v2.13.6
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(pybind11)
    set(CITADEL_SIM_ENABLED TRUE CACHE BOOL "Build citadel_sim Python extension")
else()
    set(CITADEL_SIM_ENABLED FALSE CACHE BOOL "Build citadel_sim Python extension")
endif()

add_subdirectory(src)

# ── Release build 
# cmake --preset windows-release && cmake --build build/release --config Release
