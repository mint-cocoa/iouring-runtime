add_executable(epoll_echo_server
    epoll_echo_server.cpp
)
target_compile_features(epoll_echo_server PRIVATE cxx_std_23)
target_link_libraries(epoll_echo_server PRIVATE Threads::Threads)
target_include_directories(epoll_echo_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(epoll_echo_server PRIVATE -Wall -Wextra -Wpedantic)

add_executable(epoll_http_server
    epoll_http_server.cpp
)
target_compile_features(epoll_http_server PRIVATE cxx_std_23)
target_link_libraries(epoll_http_server PRIVATE Threads::Threads)
target_include_directories(epoll_http_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(epoll_http_server PRIVATE -Wall -Wextra -Wpedantic)

add_executable(epoll_tcp_proxy
    epoll_tcp_proxy.cpp
)
target_compile_features(epoll_tcp_proxy PRIVATE cxx_std_23)
target_link_libraries(epoll_tcp_proxy PRIVATE Threads::Threads)
target_include_directories(epoll_tcp_proxy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(epoll_tcp_proxy PRIVATE -Wall -Wextra -Wpedantic)

add_executable(epoll_game_fanout_server
    epoll_game_fanout_server.cpp
)
target_compile_features(epoll_game_fanout_server PRIVATE cxx_std_23)
target_link_libraries(epoll_game_fanout_server PRIVATE Threads::Threads)
target_include_directories(epoll_game_fanout_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(epoll_game_fanout_server PRIVATE -Wall -Wextra -Wpedantic)

add_executable(iouring_game_fanout_server
    iouring_game_fanout_server.cpp
)
target_compile_features(iouring_game_fanout_server PRIVATE cxx_std_23)
target_link_libraries(iouring_game_fanout_server PRIVATE runtime Threads::Threads)
target_include_directories(iouring_game_fanout_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(iouring_game_fanout_server PRIVATE -Wall -Wextra -Wpedantic)

add_executable(runtime_echo_bench
    runtime_echo_bench.cpp
)
target_compile_features(runtime_echo_bench PRIVATE cxx_std_23)
target_link_libraries(runtime_echo_bench PRIVATE Threads::Threads)
target_include_directories(runtime_echo_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(runtime_echo_bench PRIVATE -Wall -Wextra -Wpedantic)

add_executable(runtime_game_fanout_bench
    runtime_game_fanout_bench.cpp
)
target_compile_features(runtime_game_fanout_bench PRIVATE cxx_std_23)
target_link_libraries(runtime_game_fanout_bench PRIVATE Threads::Threads)
target_include_directories(runtime_game_fanout_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(runtime_game_fanout_bench PRIVATE -Wall -Wextra -Wpedantic)
