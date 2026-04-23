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
set(IORUNTIME_WEB_PACKAGE_NAME iouring_runtime_web)

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

option(BUILD_WEB "Build the optional HTTP web module" OFF)

if(BUILD_WEB)
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

if(BUILD_WEB)
    add_library(RuntimeWeb STATIC
        src/web/HttpParser.cpp
        src/web/HttpResponse.cpp
        src/web/RadixTree.cpp
        src/web/Router.cpp
        src/web/HttpSession.cpp
        src/web/WebServer.cpp
    )

    add_library(${IORUNTIME_WEB_PACKAGE_NAME}::RuntimeWeb ALIAS RuntimeWeb)

    target_include_directories(RuntimeWeb
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${llhttp_SOURCE_DIR}/include
            ${spdlog_SOURCE_DIR}/include
    )

    target_link_libraries(RuntimeWeb
        PUBLIC
            Runtime
        PRIVATE
            llhttp_static
    )

    target_compile_options(RuntimeWeb PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(BUILD_EXAMPLES "Build runtime examples" ON)
set(_EXAMPLE_DIRS
    examples/runtime/core_echo
    examples/runtime/core_idle_echo
)
if(BUILD_WEB)
    list(APPEND _EXAMPLE_DIRS
        examples/web/hello_http
    )
endif()
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

if(BUILD_WEB)
    install(
        TARGETS RuntimeWeb llhttp_static
        EXPORT ${IORUNTIME_WEB_PACKAGE_NAME}Targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
endif()

install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/iouring_runtime/core"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/iouring_runtime"
)

if(BUILD_WEB)
    install(
        DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/iouring_runtime/web"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/iouring_runtime"
    )
endif()

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/iouring_runtimeConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/iouring_runtimeConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${IORUNTIME_PACKAGE_NAME}"
)

if(BUILD_WEB)
    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/iouring_runtime_webConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/iouring_runtime_webConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${IORUNTIME_WEB_PACKAGE_NAME}"
    )
endif()

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/iouring_runtimeConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
)

if(BUILD_WEB)
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/iouring_runtime_webConfigVersion.cmake"
        VERSION "${PROJECT_VERSION}"
        COMPATIBILITY SameMajorVersion
    )
endif()

install(
    EXPORT ${IORUNTIME_PACKAGE_NAME}Targets
    FILE "${IORUNTIME_PACKAGE_NAME}Targets.cmake"
    NAMESPACE ${IORUNTIME_PACKAGE_NAME}::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${IORUNTIME_PACKAGE_NAME}"
)

if(BUILD_WEB)
    install(
        EXPORT ${IORUNTIME_WEB_PACKAGE_NAME}Targets
        FILE "${IORUNTIME_WEB_PACKAGE_NAME}Targets.cmake"
        NAMESPACE ${IORUNTIME_WEB_PACKAGE_NAME}::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${IORUNTIME_WEB_PACKAGE_NAME}"
    )
endif()

install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/iouring_runtimeConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/iouring_runtimeConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${IORUNTIME_PACKAGE_NAME}"
)

if(BUILD_WEB)
    install(
        FILES
            "${CMAKE_CURRENT_BINARY_DIR}/iouring_runtime_webConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/iouring_runtime_webConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${IORUNTIME_WEB_PACKAGE_NAME}"
    )

    install(
        FILES
            "${llhttp_SOURCE_DIR}/include/llhttp.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )
endif()
