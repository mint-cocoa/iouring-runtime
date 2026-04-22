add_executable(test_mpsc_queue core/test_mpsc_queue.cpp)
target_link_libraries(test_mpsc_queue PRIVATE Runtime GTest::gtest_main)
add_test(NAME test_mpsc_queue COMMAND test_mpsc_queue)

add_executable(test_session_lifecycle io/test_session_lifecycle.cpp)
target_link_libraries(test_session_lifecycle PRIVATE Runtime GTest::gtest_main)
add_test(NAME test_session_lifecycle COMMAND test_session_lifecycle)

add_executable(test_session_partial_send io/test_session_partial_send.cpp)
target_link_libraries(test_session_partial_send PRIVATE Runtime GTest::gtest_main)
add_test(NAME test_session_partial_send COMMAND test_session_partial_send)

add_executable(test_session_inactivity io/test_session_inactivity.cpp)
target_link_libraries(test_session_inactivity PRIVATE Runtime GTest::gtest_main)
add_test(NAME test_session_inactivity COMMAND test_session_inactivity)

add_executable(test_session_remote_addr io/test_session_remote_addr.cpp)
target_link_libraries(test_session_remote_addr PRIVATE Runtime GTest::gtest_main)
add_test(NAME test_session_remote_addr COMMAND test_session_remote_addr)

add_executable(test_listener_accept_flags io/test_listener_accept_flags.cpp)
target_link_libraries(test_listener_accept_flags PRIVATE Runtime GTest::gtest_main)
add_test(NAME test_listener_accept_flags COMMAND test_listener_accept_flags)

add_executable(test_cross_thread_flow io/test_cross_thread_flow.cpp)
target_link_libraries(test_cross_thread_flow PRIVATE Runtime GTest::gtest_main)
add_test(NAME test_cross_thread_flow COMMAND test_cross_thread_flow)

add_executable(test_job_queue job/test_job_queue.cpp)
target_link_libraries(test_job_queue PRIVATE Runtime GTest::gtest_main)
add_test(NAME test_job_queue COMMAND test_job_queue)

add_executable(test_poll_add ring/test_poll_add.cpp)
target_link_libraries(test_poll_add PRIVATE Runtime GTest::gtest_main)
add_test(NAME test_poll_add COMMAND test_poll_add)

add_executable(test_timeout ring/test_timeout.cpp)
target_link_libraries(test_timeout PRIVATE Runtime GTest::gtest_main)
add_test(NAME test_timeout COMMAND test_timeout)
