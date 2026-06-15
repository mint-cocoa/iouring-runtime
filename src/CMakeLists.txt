add_library(observability STATIC
    observability/Logging.cpp
)
add_library(iouring::observability ALIAS observability)

target_include_directories(observability
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${spdlog_SOURCE_DIR}/include
)

if(IORING_TRACY)
    target_link_libraries(observability PUBLIC Tracy::TracyClient)
endif()

target_compile_options(observability PRIVATE -Wall -Wextra -Wpedantic)

add_library(runtime STATIC
    core/RecvBuffer.cpp
    core/SendBuffer.cpp
    core/SendQueue.cpp
    os/unix/CpuAffinity.cpp
    event/MmapGuard.cpp
    event/RingBuffer.cpp
    event/RingEvent.cpp
    event/IoRing.cpp
    event/GlobalQueue.cpp
    event/JobQueue.cpp
    event/JobTimer.cpp
    event/Worker.cpp
    event/WorkerPool.cpp
    net/SocketHandle.cpp
    net/SessionDetail.cpp
    net/SessionManager.cpp
    net/Session.cpp
    net/SessionSend.cpp
    net/SessionOps.cpp
    net/Listener.cpp
    net/ListenerOps.cpp
)
add_library(iouring::runtime ALIAS runtime)

target_include_directories(runtime
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/os/unix
)

target_link_libraries(runtime
    PUBLIC
        observability
        PkgConfig::URING
        Threads::Threads
)

target_compile_options(runtime PRIVATE -Wall -Wextra -Wpedantic)

add_library(core INTERFACE)
add_library(iouring::core ALIAS core)
target_link_libraries(core INTERFACE runtime)

add_library(event INTERFACE)
add_library(iouring::event ALIAS event)
target_link_libraries(event INTERFACE runtime)

add_library(net INTERFACE)
add_library(iouring::net ALIAS net)
target_link_libraries(net INTERFACE runtime)

set(_iouring_install_targets
    core
    event
    net
    runtime
    observability
)

if(BUILD_MEDIA)
    add_library(media STATIC
        media/Hls.cpp
    )
    add_library(iouring::media ALIAS media)

    target_include_directories(media
        PUBLIC
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )

    target_compile_options(media PRIVATE -Wall -Wextra -Wpedantic)
    list(APPEND _iouring_install_targets media)
endif()

if(BUILD_HTTP)
    add_library(http STATIC
        http/HttpMethod.cpp
        http/HttpParser.cpp
        http/HttpRequest.cpp
        http/HttpResponse.cpp
        http/HttpStatus.cpp
        http/RadixTree.cpp
        http/Router.cpp
        http/RouterDeferred.cpp
        http/HttpSession.cpp
        http/HttpSessionFile.cpp
        http/HttpSessionRecv.cpp
        http/HttpSessionRequest.cpp
        http/HttpSessionStream.cpp
        http/HttpSessionTimeout.cpp
        http/WebServer.cpp
        http/WebServerWorker.cpp
    )
    add_library(iouring::http ALIAS http)

    target_include_directories(http
        PUBLIC
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/os/unix
            ${llhttp_SOURCE_DIR}/include
    )

    target_link_libraries(http
        PUBLIC
            runtime
        PRIVATE
            llhttp_static
    )

    target_compile_options(http PRIVATE -Wall -Wextra -Wpedantic)
    list(APPEND _iouring_install_targets http llhttp_static)
endif()

if(BUILD_STREAM)
    add_library(stream STATIC
        stream/AcmeChallengeSession.cpp
        stream/AcmeHttpChallengeServer.cpp
        stream/DownstreamTlsContext.cpp
        stream/ProxyBridge.cpp
        stream/ProxyCommon.cpp
        stream/ProxyConnector.cpp
        stream/ProxySessions.cpp
        stream/TcpProxyServer.cpp
        stream/TcpProxyServerWorker.cpp
    )
    add_library(iouring::stream ALIAS stream)

    target_include_directories(stream
        PUBLIC
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/os/unix
    )

    target_link_libraries(stream
        PUBLIC
            runtime
            OpenSSL::SSL
            OpenSSL::Crypto
    )

    target_compile_options(stream PRIVATE -Wall -Wextra -Wpedantic)
    list(APPEND _iouring_install_targets stream)
endif()

if(BUILD_GAME)
    add_library(game STATIC
        game/PacketSession.cpp
        game/PlayerRegistry.cpp
        game/Room.cpp
        game/RoomManager.cpp
    )
    add_library(iouring::game ALIAS game)

    target_include_directories(game
        PUBLIC
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )

    target_link_libraries(game PUBLIC runtime)

    target_compile_options(game PRIVATE -Wall -Wextra -Wpedantic)
    list(APPEND _iouring_install_targets game)
endif()

install(
    TARGETS ${_iouring_install_targets}
    EXPORT iouringTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
    DIRECTORY "${PROJECT_SOURCE_DIR}/include/iouring"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

if(BUILD_HTTP)
    install(
        FILES "${llhttp_SOURCE_DIR}/include/llhttp.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )
endif()
