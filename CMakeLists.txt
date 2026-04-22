cmake_minimum_required(VERSION 3.16)
project(iouring_runtime VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(FetchContent)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

set(IORUNTIME_PACKAGE_NAME iouring_runtime)

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

option(IORUNTIME_TRACY "Enable Tracy Profiler" OFF)
if(IORUNTIME_TRACY)
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

set(IORUNTIME_SANITIZER "off" CACHE STRING
    "Sanitizer to enable (tsan|asan|ubsan|asan+ubsan|off)")
if(NOT IORUNTIME_SANITIZER STREQUAL "off")
    set(SAN_COMMON_FLAGS -fno-omit-frame-pointer -g)

    if(IORUNTIME_SANITIZER STREQUAL "tsan")
        list(APPEND SAN_COMMON_FLAGS -fsanitize=thread)
    elseif(IORUNTIME_SANITIZER STREQUAL "asan")
        list(APPEND SAN_COMMON_FLAGS -fsanitize=address)
    elseif(IORUNTIME_SANITIZER STREQUAL "ubsan")
        list(APPEND SAN_COMMON_FLAGS -fsanitize=undefined)
    elseif(IORUNTIME_SANITIZER STREQUAL "asan+ubsan")
        list(APPEND SAN_COMMON_FLAGS -fsanitize=address,undefined)
    else()
        message(FATAL_ERROR "Unknown sanitizer: ${IORUNTIME_SANITIZER}")
    endif()

    add_compile_options(${SAN_COMMON_FLAGS})
    add_link_options(${SAN_COMMON_FLAGS})
    message(STATUS "Sanitizer enabled: ${IORUNTIME_SANITIZER}")
endif()

add_library(Runtime STATIC
    src/ring/RingBuffer.cpp
    src/ring/IoRing.cpp
    src/io/Session.cpp
    src/io/Listener.cpp
    src/job/GlobalQueue.cpp
    src/job/JobQueue.cpp
    src/job/JobTimer.cpp
)

add_library(${IORUNTIME_PACKAGE_NAME}::Runtime ALIAS Runtime)
add_library(iouring::Runtime ALIAS Runtime)

target_include_directories(Runtime
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${spdlog_SOURCE_DIR}/include
)

target_link_libraries(Runtime
    PUBLIC
        PkgConfig::URING
        Threads::Threads
)

if(IORUNTIME_TRACY)
    target_link_libraries(Runtime PUBLIC Tracy::TracyClient)
endif()

target_compile_options(Runtime PRIVATE -Wall -Wextra -Wpedantic)

option(BUILD_EXAMPLES "Build runtime examples" ON)
set(_EXAMPLE_DIRS
    examples/runtime/core_echo
    examples/runtime/core_idle_echo
)
if(BUILD_EXAMPLES)
    foreach(_dir IN LISTS _EXAMPLE_DIRS)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_dir}/CMakeLists.txt")
            add_subdirectory(${_dir})
        endif()
    endforeach()
endif()

option(BUILD_TESTS "Build runtime tests" ON)
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

install(
    TARGETS Runtime
    EXPORT ${IORUNTIME_PACKAGE_NAME}Targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/iouring_runtimeConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/iouring_runtimeConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${IORUNTIME_PACKAGE_NAME}"
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/iouring_runtimeConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
)

install(
    EXPORT ${IORUNTIME_PACKAGE_NAME}Targets
    FILE "${IORUNTIME_PACKAGE_NAME}Targets.cmake"
    NAMESPACE ${IORUNTIME_PACKAGE_NAME}::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${IORUNTIME_PACKAGE_NAME}"
)

install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/iouring_runtimeConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/iouring_runtimeConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${IORUNTIME_PACKAGE_NAME}"
)
