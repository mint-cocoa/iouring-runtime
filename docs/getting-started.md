# Getting Started

This guide gets you from a fresh checkout to a running server.

## Requirements

- Linux with `io_uring` support
- CMake 3.16 or newer
- C++23 compiler
- `pkg-config`
- `liburing`
- OpenSSL when building `RuntimeProxy`
- Protobuf and `protoc` when building the game examples

## Build The Core Runtime

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

The default build includes:

- `iouring_runtime::Runtime`
- `iouring_runtime::RuntimeObservability`
- `iouring_runtime::RuntimeMedia`
- core examples
- Activity server
- tests, unless `-DBUILD_TESTS=OFF`

## Build Optional Modules

```bash
cmake -S . -B build-all \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_WEB=ON \
  -DBUILD_PROXY=ON \
  -DBUILD_GAME=ON \
  -DBUILD_EXAMPLES=ON
cmake --build build-all -j$(nproc)
```

Optional modules:

| Option | Target | Use |
| --- | --- | --- |
| `-DBUILD_WEB=ON` | `iouring_runtime_web::RuntimeWeb` | HTTP server, router, request, response |
| `-DBUILD_PROXY=ON` | `iouring_runtime_proxy::RuntimeProxy` | TCP reverse proxy and optional TLS termination |
| `-DBUILD_GAME=ON` | `iouring_runtime_game::RuntimeGame` | packet sessions, players, rooms |

## Run A Core TCP Echo Server

```bash
cmake -S . -B build-core \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_EXAMPLES=ON \
  -DBUILD_TESTS=OFF
cmake --build build-core --target core_echo -j$(nproc)
CORE_ECHO_PORT=19090 ./build-core/bin/core_echo
```

Try it:

```bash
printf 'hello\n' | nc 127.0.0.1 19090
```

## Run An HTTP Server

```bash
cmake -S . -B build-web \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_WEB=ON \
  -DBUILD_EXAMPLES=ON \
  -DBUILD_TESTS=OFF
cmake --build build-web --target hello_http -j$(nproc)
HELLO_HTTP_PORT=8080 ./build-web/bin/hello_http
```

Try it:

```bash
curl http://127.0.0.1:8080/
curl http://127.0.0.1:8080/hello/cocoa?lang=ko
curl http://127.0.0.1:8080/health
```

## Run A TCP Reverse Proxy

Start an upstream server first. For example:

```bash
python3 -m http.server 8080
```

In another terminal:

```bash
cmake -S . -B build-proxy \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_PROXY=ON \
  -DBUILD_EXAMPLES=ON \
  -DBUILD_TESTS=OFF
cmake --build build-proxy --target tcp_reverse_proxy -j$(nproc)
TCP_PROXY_LISTEN_HOST=127.0.0.1 \
TCP_PROXY_LISTEN_PORT=18080 \
TCP_PROXY_UPSTREAM_HOST=127.0.0.1 \
TCP_PROXY_UPSTREAM_PORT=8080 \
./build-proxy/bin/tcp_reverse_proxy
```

Try it:

```bash
curl http://127.0.0.1:18080/
```

## Run Tests

```bash
cmake -S . -B build-tests \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_WEB=ON \
  -DBUILD_PROXY=ON \
  -DBUILD_GAME=ON \
  -DBUILD_TESTS=ON
cmake --build build-tests -j$(nproc)
ctest --test-dir build-tests --output-on-failure
```

## Next

- Copy CMake and C++ snippets from `docs/usage-examples.md`.
- Check public target and type names in `docs/api-reference.md`.
- Use `docs/runtime-guide.md` for a lower-level TCP server walkthrough.
- Use `docs/proxy-guide.md` when deploying the proxy with TLS or systemd.
