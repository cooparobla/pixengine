cmake_minimum_required(VERSION 3.16)
cmake_policy(VERSION 3.16)

project(pixengine_demo)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(ROOT_DIR_PARENT "${CMAKE_SOURCE_DIR}" DIRECTORY)

# --- Vulkan ---
find_package(Vulkan REQUIRED)

# --- Window system (GLFW) ---
find_package(glfw3 REQUIRED)

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

# --- Sibling library paths (header-only, no build targets required) ---
set(LIBCOOPA_DIR "${ROOT_DIR_PARENT}/libcoopa")
set(GFXCOOPA_DIR "${ROOT_DIR_PARENT}/gfxcoopa")
set(CAML_DIR     "${ROOT_DIR_PARENT}/caml")
set(UICOOPA_DIR  "${ROOT_DIR_PARENT}/uicoopa")

# --- Include directories ---
include_directories(${CMAKE_SOURCE_DIR}/)                  # pixengine/
include_directories(${LIBCOOPA_DIR}/includes/)              # fkYAML, glm, phmap
include_directories(${LIBCOOPA_DIR})                        # coopa/
include_directories(${GFXCOOPA_DIR}/includes/)              # volk, vma, stb (first match wins)
include_directories(${GFXCOOPA_DIR})                        # gfxcoopa/
include_directories(${CAML_DIR}/includes/)                  # caml/ + fkYAML
include_directories(${CAML_DIR})                            # caml.h root
include_directories(${UICOOPA_DIR}/includes/)                # stb_truetype (gfxcoopa's stb wins for stb_image)
include_directories(${UICOOPA_DIR})                          # uicoopa/

# --- Configure and include root directory header ---
configure_file(configuration/root_directory.h.in configuration/root_directory.h)
include_directories(${CMAKE_BINARY_DIR}/configuration)

# --- Vulkan loader (volk compiled once here) ---
add_definitions(-DVOLK_IMPLEMENTATION)

# --- VMA compiled once here ---
add_definitions(-DVMA_IMPLEMENTATION)

# --- stb_truetype / stb_image / stb_image_write: implementation compiled once here ---
add_definitions(-DSTB_TRUETYPE_IMPLEMENTATION)
add_definitions(-DSTB_IMAGE_IMPLEMENTATION)
add_definitions(-DSTB_IMAGE_WRITE_IMPLEMENTATION)

# --- GLM: use Vulkan depth convention [0, 1] instead of OpenGL [-1, 1] ---
add_definitions(-DGLM_FORCE_DEPTH_ZERO_TO_ONE)

# --- Add executables ---
# `pixengine` is the headless suite ctest runs — test.cpp never opens a window,
# so `ctest`/CI never needs a display. `pixengine_demo` is the windowed demo
# that `cplay` runs (it matches this file's project() name).
add_executable(pixengine test.cpp)
target_link_libraries(pixengine
    PRIVATE
    Vulkan::Vulkan
    glfw
    dl
    ${ZSTD_TARGET}
    OpenSSL::Crypto
)

add_executable(pixengine_demo demo.cpp)
target_link_libraries(pixengine_demo
    PRIVATE
    Vulkan::Vulkan
    glfw
    dl
    ${ZSTD_TARGET}
    OpenSSL::Crypto
)

enable_testing()
add_test(NAME pixengine_tests COMMAND pixengine)
