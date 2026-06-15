cmake_minimum_required(VERSION 3.16)
project(iouring VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(FetchContent)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.15.1
)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(spdlog)

find_package(PkgConfig REQUIRED)
pkg_check_modules(URING REQUIRED IMPORTED_TARGET liburing)
find_package(Threads REQUIRED)

option(BUILD_HTTP "Build the optional HTTP module" OFF)
option(BUILD_STREAM "Build the optional TCP stream proxy module" OFF)
option(BUILD_GAME "Build the optional multiplayer game packet module" OFF)
option(BUILD_MEDIA "Build the optional media helpers module" OFF)
option(BUILD_BENCHMARKS "Build benchmark-only servers and clients" OFF)
option(BUILD_TESTS "Build runtime tests" ON)

if(BUILD_HTTP)
    FetchContent_Declare(
        llhttp
        GIT_REPOSITORY https://github.com/nodejs/llhttp.git
        GIT_TAG        release/v9.3.1
        GIT_SHALLOW    TRUE
    )
    set(LLHTTP_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
    set(LLHTTP_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF)
    FetchContent_MakeAvailable(llhttp)
endif()

if(BUILD_STREAM)
    find_package(OpenSSL REQUIRED)
endif()

option(IORING_TRACY "Enable Tracy Profiler" OFF)
if(IORING_TRACY)
    FetchContent_Declare(
        tracy
        GIT_REPOSITORY https://github.com/wolfpld/tracy.git
        GIT_TAG        v0.13.1
        GIT_SHALLOW    TRUE
    )
    set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
    set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(tracy)
endif()

set(IORING_SANITIZER "off" CACHE STRING
    "Sanitizer to enable (tsan|asan|ubsan|asan+ubsan|off)")
if(NOT IORING_SANITIZER STREQUAL "off")
    set(SAN_COMMON_FLAGS -fno-omit-frame-pointer -g)

    if(IORING_SANITIZER STREQUAL "tsan")
        list(APPEND SAN_COMMON_FLAGS -fsanitize=thread)
    elseif(IORING_SANITIZER STREQUAL "asan")
        list(APPEND SAN_COMMON_FLAGS -fsanitize=address)
    elseif(IORING_SANITIZER STREQUAL "ubsan")
        list(APPEND SAN_COMMON_FLAGS -fsanitize=undefined)
    elseif(IORING_SANITIZER STREQUAL "asan+ubsan")
        list(APPEND SAN_COMMON_FLAGS -fsanitize=address,undefined)
    else()
        message(FATAL_ERROR "Unknown sanitizer: ${IORING_SANITIZER}")
    endif()

    add_compile_options(${SAN_COMMON_FLAGS})
    add_link_options(${SAN_COMMON_FLAGS})
    message(STATUS "Sanitizer enabled: ${IORING_SANITIZER}")
endif()

add_subdirectory(src)

if(BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(BUILD_TESTS)
    enable_testing()

    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.0
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_subdirectory(tests)
endif()

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/iouringConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/iouringConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/iouring"
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/iouringConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
)

install(
    EXPORT iouringTargets
    FILE iouringTargets.cmake
    NAMESPACE iouring::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/iouring"
)

install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/iouringConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/iouringConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/iouring"
)
