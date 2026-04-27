cmake_minimum_required(VERSION 3.16)

add_executable(webhook_inbox
    src/main.cpp
)

target_link_libraries(webhook_inbox PRIVATE
    iouring_runtime_web::RuntimeWeb
)

target_compile_features(webhook_inbox PRIVATE cxx_std_23)
target_compile_options(webhook_inbox PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS webhook_inbox RUNTIME DESTINATION bin)
install(DIRECTORY static/ DESTINATION share/webhook-inbox/static)
