# Getting Started

This guide gets you from a fresh checkout to a running server.

## Requirements

- Linux with `io_uring` support
- CMake 3.16 or newer
- C++23 compiler
- `pkg-config`
- `liburing`
- OpenSSL when building `iouring::stream`
- Protobuf and `protoc` when building game examples in `iouring-runtime-examples`

## Build The Core runtime

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

The default build includes:

- `iouring::runtime`
- `iouring::observability`
- optional modules only when their `BUILD_*` options are enabled
- tests, unless `-DBUILD_TESTS=OFF`

## Build Optional Modules

```bash
cmake -S . -B build-all \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_HTTP=ON \
  -DBUILD_STREAM=ON \
  -DBUILD_GAME=ON \
  -DBUILD_MEDIA=ON
cmake --build build-all -j$(nproc)
```

Optional modules:

| Option | Target | Use |
| --- | --- | --- |
| `-DBUILD_HTTP=ON` | `iouring::http` | HTTP server, router, request, response |
| `-DBUILD_STREAM=ON` | `iouring::stream` | TCP reverse proxy and optional TLS termination |
| `-DBUILD_GAME=ON` | `iouring::game` | packet sessions, players, rooms |

## Build The Examples Repository

```bash
cmake -S . -B build-runtime \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_HTTP=ON \
  -DBUILD_STREAM=ON \
  -DBUILD_GAME=ON \
  -DBUILD_MEDIA=ON
cmake --build build-runtime -j$(nproc)
cmake --install build-runtime --prefix /tmp/iouring-install

cmake -S ../iouring-runtime-examples -B ../iouring-runtime-examples/build \
  -DCMAKE_PREFIX_PATH=/tmp/iouring-install
cmake --build ../iouring-runtime-examples/build --target core_echo hello_http -j$(nproc)

CORE_ECHO_PORT=19090 ../iouring-runtime-examples/build/bin/core_echo
HELLO_HTTP_PORT=8080 ../iouring-runtime-examples/build/bin/hello_http
```

## Run Tests

```bash
cmake -S . -B build-tests \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_HTTP=ON \
  -DBUILD_STREAM=ON \
  -DBUILD_GAME=ON \
  -DBUILD_TESTS=ON
cmake --build build-tests -j$(nproc)
ctest --test-dir build-tests --output-on-failure
```

## Next

- Copy CMake and C++ snippets from `docs/usage-examples.md`.
- Use the `iouring-runtime-examples` repository for runnable applications.
- Check public target and type names in `docs/api-reference.md`.
- Use `docs/runtime-guide.md` for a lower-level TCP server walkthrough.
- Use `docs/proxy-guide.md` when deploying the proxy with TLS or systemd.
