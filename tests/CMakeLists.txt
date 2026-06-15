add_executable(test_mpsc_queue core/test_mpsc_queue.cpp)
target_link_libraries(test_mpsc_queue PRIVATE runtime GTest::gtest_main)
add_test(NAME test_mpsc_queue COMMAND test_mpsc_queue)

add_executable(test_observability_logging observability/test_logging.cpp)
target_link_libraries(test_observability_logging PRIVATE observability GTest::gtest_main)
add_test(NAME test_observability_logging COMMAND test_observability_logging)

if(BUILD_MEDIA)
    add_executable(test_media_hls media/test_hls.cpp)
    target_link_libraries(test_media_hls PRIVATE media GTest::gtest_main)
    add_test(NAME test_media_hls COMMAND test_media_hls)
endif()

add_executable(test_session_lifecycle io/test_session_lifecycle.cpp)
target_link_libraries(test_session_lifecycle PRIVATE runtime GTest::gtest_main)
add_test(NAME test_session_lifecycle COMMAND test_session_lifecycle)

add_executable(test_session_partial_send io/test_session_partial_send.cpp)
target_link_libraries(test_session_partial_send PRIVATE runtime GTest::gtest_main)
add_test(NAME test_session_partial_send COMMAND test_session_partial_send)

add_executable(test_listener_accept_flags io/test_listener_accept_flags.cpp)
target_link_libraries(test_listener_accept_flags PRIVATE runtime GTest::gtest_main)
add_test(NAME test_listener_accept_flags COMMAND test_listener_accept_flags)

add_executable(test_cross_thread_flow io/test_cross_thread_flow.cpp)
target_link_libraries(test_cross_thread_flow PRIVATE runtime GTest::gtest_main)
add_test(NAME test_cross_thread_flow COMMAND test_cross_thread_flow)

add_executable(test_worker io/test_worker.cpp)
target_link_libraries(test_worker PRIVATE runtime GTest::gtest_main)
add_test(NAME test_worker COMMAND test_worker)

add_executable(test_job_queue job/test_job_queue.cpp)
target_link_libraries(test_job_queue PRIVATE runtime GTest::gtest_main)
add_test(NAME test_job_queue COMMAND test_job_queue)

add_executable(test_poll_add ring/test_poll_add.cpp)
target_link_libraries(test_poll_add PRIVATE runtime GTest::gtest_main)
add_test(NAME test_poll_add COMMAND test_poll_add)

add_executable(test_timeout ring/test_timeout.cpp)
target_link_libraries(test_timeout PRIVATE runtime GTest::gtest_main)
add_test(NAME test_timeout COMMAND test_timeout)

add_executable(test_write ring/test_write.cpp)
target_link_libraries(test_write PRIVATE runtime GTest::gtest_main)
add_test(NAME test_write COMMAND test_write)

if(BUILD_HTTP)
    add_executable(test_http_parser web/test_http_parser.cpp)
    target_link_libraries(test_http_parser PRIVATE iouring::http GTest::gtest_main)
    add_test(NAME test_http_parser COMMAND test_http_parser)

    add_executable(test_http_parser_limits web/test_http_parser_limits.cpp)
    target_link_libraries(test_http_parser_limits PRIVATE iouring::http GTest::gtest_main)
    add_test(NAME test_http_parser_limits COMMAND test_http_parser_limits)

    add_executable(test_router web/test_router.cpp)
    target_link_libraries(test_router PRIVATE iouring::http GTest::gtest_main)
    add_test(NAME test_router COMMAND test_router)

    add_executable(test_http_response_framing web/test_http_response_framing.cpp)
    target_link_libraries(test_http_response_framing PRIVATE iouring::http GTest::gtest_main)
    add_test(NAME test_http_response_framing COMMAND test_http_response_framing)

    add_executable(test_http_server_timeouts web/test_http_server_timeouts.cpp)
    target_link_libraries(test_http_server_timeouts PRIVATE iouring::http GTest::gtest_main)
    add_test(NAME test_http_server_timeouts COMMAND test_http_server_timeouts)

    add_executable(test_web_server_signal web/test_web_server_signal.cpp)
    target_link_libraries(test_web_server_signal PRIVATE iouring::http GTest::gtest_main)
    add_test(NAME test_web_server_signal COMMAND test_web_server_signal)
endif()

if(BUILD_STREAM)
    add_executable(test_tcp_proxy proxy/test_tcp_proxy.cpp)
    target_link_libraries(test_tcp_proxy PRIVATE iouring::stream GTest::gtest_main)
    add_test(NAME test_tcp_proxy COMMAND test_tcp_proxy)
endif()

if(BUILD_GAME)
    add_executable(test_packet_builder game/test_packet_builder.cpp)
    target_link_libraries(test_packet_builder PRIVATE iouring::game GTest::gtest_main)
    add_test(NAME test_packet_builder COMMAND test_packet_builder)

    add_executable(test_packet_session game/test_packet_session.cpp)
    target_link_libraries(test_packet_session PRIVATE iouring::game GTest::gtest_main)
    add_test(NAME test_packet_session COMMAND test_packet_session)

    add_executable(test_player_registry game/test_player_registry.cpp)
    target_link_libraries(test_player_registry PRIVATE iouring::game GTest::gtest_main)
    add_test(NAME test_player_registry COMMAND test_player_registry)

    add_executable(test_room game/test_room.cpp)
    target_link_libraries(test_room PRIVATE iouring::game GTest::gtest_main)
    add_test(NAME test_room COMMAND test_room)

    add_executable(test_room_manager game/test_room_manager.cpp)
    target_link_libraries(test_room_manager PRIVATE iouring::game GTest::gtest_main)
    add_test(NAME test_room_manager COMMAND test_room_manager)

endif()
