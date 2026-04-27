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
set(IORUNTIME_OBSERVABILITY_PACKAGE_NAME iouring_runtime_observability)
set(IORUNTIME_WEB_PACKAGE_NAME iouring_runtime_web)
set(IORUNTIME_PROXY_PACKAGE_NAME iouring_runtime_proxy)
set(IORUNTIME_GAME_PACKAGE_NAME iouring_runtime_game)

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
option(BUILD_PROXY "Build the optional TCP proxy module" OFF)
option(BUILD_GAME "Build the optional multiplayer game packet module" OFF)

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

add_library(RuntimeObservability STATIC
    src/modules/observability/Logging.cpp
)

add_library(${IORUNTIME_OBSERVABILITY_PACKAGE_NAME}::RuntimeObservability ALIAS RuntimeObservability)
add_library(iouring::RuntimeObservability ALIAS RuntimeObservability)

target_include_directories(RuntimeObservability
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${spdlog_SOURCE_DIR}/include
)

if(IORUNTIME_TRACY)
    target_link_libraries(RuntimeObservability PUBLIC Tracy::TracyClient)
endif()

target_compile_options(RuntimeObservability PRIVATE -Wall -Wextra -Wpedantic)

add_library(Runtime STATIC
    src/runtime/common/CpuAffinity.cpp
    src/runtime/ring/RingBuffer.cpp
    src/runtime/ring/IoRing.cpp
    src/runtime/io/Session.cpp
    src/runtime/io/SessionSend.cpp
    src/runtime/io/Listener.cpp
    src/runtime/job/GlobalQueue.cpp
    src/runtime/job/JobQueue.cpp
    src/runtime/job/JobTimer.cpp
)

add_library(${IORUNTIME_PACKAGE_NAME}::Runtime ALIAS Runtime)
add_library(iouring::Runtime ALIAS Runtime)

target_include_directories(Runtime
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime
)

target_link_libraries(Runtime
    PUBLIC
        RuntimeObservability
        PkgConfig::URING
        Threads::Threads
)

target_compile_options(Runtime PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_WEB)
    add_library(RuntimeWeb STATIC
        src/modules/web/HttpParser.cpp
        src/modules/web/HttpResponse.cpp
        src/modules/web/RadixTree.cpp
        src/modules/web/Router.cpp
        src/modules/web/RouterDeferred.cpp
        src/modules/web/HttpSession.cpp
        src/modules/web/HttpSessionFile.cpp
        src/modules/web/HttpSessionRecv.cpp
        src/modules/web/HttpSessionRequest.cpp
        src/modules/web/HttpSessionStream.cpp
        src/modules/web/HttpSessionTimeout.cpp
        src/modules/web/WebServer.cpp
        src/modules/web/WebServerWorker.cpp
    )

    add_library(${IORUNTIME_WEB_PACKAGE_NAME}::RuntimeWeb ALIAS RuntimeWeb)

    target_include_directories(RuntimeWeb
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime
            ${llhttp_SOURCE_DIR}/include
    )

    target_link_libraries(RuntimeWeb
        PUBLIC
            Runtime
        PRIVATE
            llhttp_static
    )

    target_compile_options(RuntimeWeb PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_PROXY)
    find_package(OpenSSL REQUIRED)

    add_library(RuntimeProxy STATIC
        src/modules/proxy/AcmeChallengeSession.cpp
        src/modules/proxy/DownstreamTlsContext.cpp
        src/modules/proxy/ProxyBridge.cpp
        src/modules/proxy/ProxyCommon.cpp
        src/modules/proxy/ProxyConnector.cpp
        src/modules/proxy/ProxySessions.cpp
        src/modules/proxy/TcpProxyServer.cpp
        src/modules/proxy/TcpProxyServerWorker.cpp
    )

    add_library(${IORUNTIME_PROXY_PACKAGE_NAME}::RuntimeProxy ALIAS RuntimeProxy)

    target_include_directories(RuntimeProxy
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime
    )

    target_link_libraries(RuntimeProxy
        PUBLIC
            Runtime
            OpenSSL::SSL
            OpenSSL::Crypto
    )

    target_compile_options(RuntimeProxy PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_GAME)
    add_library(RuntimeGame STATIC
        src/modules/game/PacketSession.cpp
        src/modules/game/PlayerRegistry.cpp
        src/modules/game/Room.cpp
        src/modules/game/RoomManager.cpp
    )

    add_library(${IORUNTIME_GAME_PACKAGE_NAME}::RuntimeGame ALIAS RuntimeGame)

    target_include_directories(RuntimeGame
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )

    target_link_libraries(RuntimeGame
        PUBLIC
            Runtime
    )

    target_compile_options(RuntimeGame PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(BUILD_EXAMPLES "Build runtime examples" ON)
set(_EXAMPLE_DIRS
    examples/runtime/core_echo
    examples/runtime/core_idle_echo
)
if(BUILD_WEB)
    list(APPEND _EXAMPLE_DIRS
        examples/web/file_store_server
        examples/web/dropapp
        examples/web/hello_http
        examples/web/speedtest_server
        examples/web/status_server
        examples/web/webhook_inbox
    )
endif()
if(BUILD_PROXY)
    list(APPEND _EXAMPLE_DIRS
        examples/proxy/tcp_reverse_proxy
    )
endif()
if(BUILD_GAME)
    find_package(Protobuf QUIET)
    if(Protobuf_FOUND AND Protobuf_PROTOC_EXECUTABLE)
        list(APPEND _EXAMPLE_DIRS
            examples/game/dungeon_packet_echo
            examples/game/dungeon_full_server
        )
    else()
        message(WARNING
            "Protobuf library or protoc was not found; "
            "skipping examples/game/dungeon_packet_echo")
    endif()
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
    TARGETS Runtime RuntimeObservability
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

if(BUILD_PROXY)
    install(
        TARGETS RuntimeProxy
        EXPORT ${IORUNTIME_PROXY_PACKAGE_NAME}Targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
endif()

if(BUILD_GAME)
    install(
        TARGETS RuntimeGame
        EXPORT ${IORUNTIME_GAME_PACKAGE_NAME}Targets
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

install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/iouring_runtime/observability"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/iouring_runtime"
)

if(BUILD_WEB)
    install(
        DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/iouring_runtime/web"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/iouring_runtime"
    )
endif()

if(BUILD_PROXY)
    install(
        DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/iouring_runtime/proxy"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/iouring_runtime"
    )
endif()

if(BUILD_GAME)
    install(
        DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/iouring_runtime/game"
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

if(BUILD_PROXY)
    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/iouring_runtime_proxyConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/iouring_runtime_proxyConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${IORUNTIME_PROXY_PACKAGE_NAME}"
    )
endif()

if(BUILD_GAME)
    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/iouring_runtime_gameConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/iouring_runtime_gameConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${IORUNTIME_GAME_PACKAGE_NAME}"
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

if(BUILD_PROXY)
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/iouring_runtime_proxyConfigVersion.cmake"
        VERSION "${PROJECT_VERSION}"
        COMPATIBILITY SameMajorVersion
    )
endif()

if(BUILD_GAME)
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/iouring_runtime_gameConfigVersion.cmake"
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

if(BUILD_PROXY)
    install(
        EXPORT ${IORUNTIME_PROXY_PACKAGE_NAME}Targets
        FILE "${IORUNTIME_PROXY_PACKAGE_NAME}Targets.cmake"
        NAMESPACE ${IORUNTIME_PROXY_PACKAGE_NAME}::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${IORUNTIME_PROXY_PACKAGE_NAME}"
    )
endif()

if(BUILD_GAME)
    install(
        EXPORT ${IORUNTIME_GAME_PACKAGE_NAME}Targets
        FILE "${IORUNTIME_GAME_PACKAGE_NAME}Targets.cmake"
        NAMESPACE ${IORUNTIME_GAME_PACKAGE_NAME}::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${IORUNTIME_GAME_PACKAGE_NAME}"
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

if(BUILD_PROXY)
    install(
        FILES
            "${CMAKE_CURRENT_BINARY_DIR}/iouring_runtime_proxyConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/iouring_runtime_proxyConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${IORUNTIME_PROXY_PACKAGE_NAME}"
    )
endif()

if(BUILD_GAME)
    install(
        FILES
            "${CMAKE_CURRENT_BINARY_DIR}/iouring_runtime_gameConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/iouring_runtime_gameConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${IORUNTIME_GAME_PACKAGE_NAME}"
    )
endif()
