cmake_minimum_required(VERSION 3.16)
cmake_policy(VERSION 3.16)

project(pixengine_demo)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(ROOT_DIR_PARENT "${CMAKE_SOURCE_DIR}" DIRECTORY)

# --- OpenSSL (required by caml) ---
find_package(OpenSSL REQUIRED)

# --- Zstd (required by caml) ---
find_package(PkgConfig QUIET)
if (PKG_CONFIG_FOUND)
    pkg_check_modules(ZSTD QUIET libzstd)
endif()

if (ZSTD_FOUND)
    set(ZSTD_TARGET ${ZSTD_LIBRARIES})
    include_directories(${ZSTD_INCLUDE_DIRS})
else()
    include(FetchContent)
    FetchContent_Declare(
        zstd
        GIT_REPOSITORY https://github.com/facebook/zstd.git
        GIT_TAG        v1.5.6
        SOURCE_SUBDIR  build/cmake
    )
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_STATIC   ON  CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(zstd)
    set(ZSTD_TARGET libzstd_static)
    include_directories(${zstd_SOURCE_DIR}/lib)
endif()

# --- Sibling library paths ---
set(CAML_DIR     "${ROOT_DIR_PARENT}/caml")
set(UICOOPA_DIR  "${ROOT_DIR_PARENT}/uicoopa")
set(GFXCOOPA_DIR "${ROOT_DIR_PARENT}/gfxcoopa")
if(NOT TARGET coopa::gfx)
    add_subdirectory(${GFXCOOPA_DIR} ${CMAKE_CURRENT_BINARY_DIR}/gfxcoopa-build)
endif()

# --- Include directories ---
# uicoopa is still consumed header-only (it exports no CMake target of its
# own to link) -- everything Vulkan/windowing-related now comes transitively
# through coopa::gfx below instead of being re-derived here.
include_directories(${CMAKE_SOURCE_DIR}/)                    # pixengine/
include_directories(${CAML_DIR}/includes/)                   # caml/ + fkYAML
include_directories(${CAML_DIR})                             # caml.h root
include_directories(${UICOOPA_DIR}/includes/)                 # stb_truetype
include_directories(${UICOOPA_DIR})                           # uicoopa/

# --- Configure and include root directory header ---
configure_file(configuration/root_directory.h.in configuration/root_directory.h)
include_directories(${CMAKE_BINARY_DIR}/configuration)

# --- stb_truetype: implementation compiled once here (stb_image/_write now
# live in gfxcoopa's single implementation TU) ---
add_definitions(-DSTB_TRUETYPE_IMPLEMENTATION)

# --- Add executables ---
# `pixengine` is the headless suite ctest runs — test.cpp never opens a window,
# so `ctest`/CI never needs a display. `pixengine_demo` is the windowed demo
# that `cplay` runs (it matches this file's project() name).
add_executable(pixengine test.cpp)
target_link_libraries(pixengine PRIVATE coopa::gfx ${ZSTD_TARGET} OpenSSL::Crypto)

add_executable(pixengine_demo demo.cpp)
target_link_libraries(pixengine_demo PRIVATE coopa::gfx ${ZSTD_TARGET} OpenSSL::Crypto)

enable_testing()
add_test(NAME pixengine_tests COMMAND pixengine)

# --- Vulkan/GLFW leak gate ---
# pixengine names zero Vk*/VK_*/vk*/GLFW*/glfw* symbols anywhere (verified:
# the cleanest of the four migrated consumers). ON by default: this repo is
# done, and the gate's job is making sure it can't regress.
option(GFX_LEAK_CHECK "Fail the build if consumer source names raw Vulkan/GLFW symbols" ON)
if(GFX_LEAK_CHECK)
    add_custom_target(pixengine_leak_check ALL
        COMMAND ${GFXCOOPA_DIR}/tools/check_no_vulkan.sh ${CMAKE_SOURCE_DIR})
    add_dependencies(pixengine pixengine_leak_check)
    add_dependencies(pixengine_demo pixengine_leak_check)
    add_test(NAME pixengine_no_vulkan COMMAND ${GFXCOOPA_DIR}/tools/check_no_vulkan.sh ${CMAKE_SOURCE_DIR})
endif()
