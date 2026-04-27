set(DUNGEON_PACKET_ECHO_PROTO_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/proto")
file(GLOB DUNGEON_PACKET_ECHO_PROTO_FILES
    "${DUNGEON_PACKET_ECHO_PROTO_DIR}/*.proto")

set(DUNGEON_PACKET_ECHO_PROTO_SRCS)
set(DUNGEON_PACKET_ECHO_PROTO_HDRS)
foreach(_proto IN LISTS DUNGEON_PACKET_ECHO_PROTO_FILES)
    get_filename_component(_proto_name "${_proto}" NAME_WE)
    set(_proto_src "${CMAKE_CURRENT_BINARY_DIR}/${_proto_name}.pb.cc")
    set(_proto_hdr "${CMAKE_CURRENT_BINARY_DIR}/${_proto_name}.pb.h")
    add_custom_command(
        OUTPUT "${_proto_src}" "${_proto_hdr}"
        COMMAND ${Protobuf_PROTOC_EXECUTABLE}
            --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
            -I "${DUNGEON_PACKET_ECHO_PROTO_DIR}"
            "${_proto}"
        DEPENDS "${_proto}"
        VERBATIM
    )
    list(APPEND DUNGEON_PACKET_ECHO_PROTO_SRCS "${_proto_src}")
    list(APPEND DUNGEON_PACKET_ECHO_PROTO_HDRS "${_proto_hdr}")
endforeach()

add_executable(dungeon_packet_echo
    src/DungeonGameSession.cpp
    src/DungeonRoom.cpp
    src/main.cpp
    ${DUNGEON_PACKET_ECHO_PROTO_SRCS}
)

target_include_directories(dungeon_packet_echo PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(dungeon_packet_echo PRIVATE
    RuntimeGame
    protobuf::libprotobuf
)

target_compile_features(dungeon_packet_echo PRIVATE cxx_std_23)
