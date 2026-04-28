cmake_minimum_required(VERSION 3.16)

find_package(OpenSSL REQUIRED)

add_executable(activity_server
    src/ActivityHttp.cpp
    src/ActivityHub.cpp
    src/ActivitySession.cpp
    src/ActivityUtils.cpp
    src/main.cpp
)

target_link_libraries(activity_server PRIVATE
    iouring_runtime::Runtime
    iouring_runtime::RuntimeMedia
    OpenSSL::Crypto
)

target_compile_features(activity_server PRIVATE cxx_std_23)
target_compile_options(activity_server PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS activity_server RUNTIME DESTINATION bin)
