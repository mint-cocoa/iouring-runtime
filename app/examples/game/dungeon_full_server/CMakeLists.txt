set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")
file(GLOB PROTO_FILES "${PROTO_DIR}/*.proto")

set(PROTO_SRCS)
set(PROTO_HDRS)
foreach(_proto IN LISTS PROTO_FILES)
    get_filename_component(_proto_name "${_proto}" NAME_WE)
    set(_proto_src "${CMAKE_CURRENT_BINARY_DIR}/${_proto_name}.pb.cc")
    set(_proto_hdr "${CMAKE_CURRENT_BINARY_DIR}/${_proto_name}.pb.h")
    add_custom_command(
        OUTPUT "${_proto_src}" "${_proto_hdr}"
        COMMAND ${Protobuf_PROTOC_EXECUTABLE}
            --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
            -I "${PROTO_DIR}"
            "${_proto}"
        DEPENDS "${_proto}"
        VERBATIM
    )
    list(APPEND PROTO_SRCS "${_proto_src}")
    list(APPEND PROTO_HDRS "${_proto_hdr}")
endforeach()

add_executable(dungeon_full_server
    src/main.cpp
    src/net/io_worker.cpp
    src/net/io_worker_pool.cpp
    src/net/outbound_mailbox.cpp
    src/game/player_context.cpp
    src/net/game_session.cpp
    src/game/room.cpp
    src/game/room_manager.cpp
    src/game/dungeon_generator.cpp
    src/game/bot_manager.cpp
    src/game/projectile_manager.cpp
    src/handler/auth_handler.cpp
    src/handler/char_handler.cpp
    src/handler/room_handler.cpp
    src/handler/game_handler.cpp
    src/system/combat_system.cpp
    src/system/chat_system.cpp
    src/system/party_system.cpp
    src/handler/social_handler.cpp
    src/db/memory_db_service.cpp
    src/db/sqlite_db_service.cpp
    vendor/sqlite3.c
    ${PROTO_SRCS}
)

target_include_directories(dungeon_full_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(dungeon_full_server PRIVATE
    RuntimeGame
    spdlog::spdlog
    protobuf::libprotobuf
)

target_compile_features(dungeon_full_server PRIVATE cxx_std_23)

add_executable(dungeon_protocol_probe
    tools/protocol_probe.cpp
    ${PROTO_SRCS}
)

target_include_directories(dungeon_protocol_probe PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(dungeon_protocol_probe PRIVATE
    protobuf::libprotobuf
)

target_compile_features(dungeon_protocol_probe PRIVATE cxx_std_23)
