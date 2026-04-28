# Core Runtime Guide

Use `iouring_runtime::Runtime` when you want a custom TCP protocol instead of
HTTP, proxying, or the game packet helpers.

## Build The Example

```bash
cmake -S . -B build-core \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_EXAMPLES=ON \
  -DBUILD_TESTS=OFF
cmake --build build-core --target core_echo core_idle_echo -j$(nproc)
```

Run:

```bash
CORE_ECHO_PORT=19090 ./build-core/bin/core_echo
```

Try it:

```bash
printf 'hello\n' | nc 127.0.0.1 19090
```

## Link The Runtime

```cmake
find_package(iouring_runtime CONFIG REQUIRED)

add_executable(my_tcp_server src/main.cpp)
target_link_libraries(my_tcp_server PRIVATE
    iouring_runtime::Runtime
)
target_compile_features(my_tcp_server PRIVATE cxx_std_23)
```

## Include What You Use

```cpp
#include <iouring_runtime/core/IoRing.h>
#include <iouring_runtime/core/Listener.h>
#include <iouring_runtime/core/SendBuffer.h>
#include <iouring_runtime/core/Session.h>
#include <iouring_runtime/core/Types.h>
```

## Define A Session

A `Session` owns socket I/O, recv registration, send queue draining, timeout
hooks, and disconnect state. Your subclass implements protocol behavior.

```cpp
class EchoSession final : public iouring_runtime::core::io::Session {
public:
    using Session::Session;

private:
    void OnRecv(std::span<const std::byte> data) override {
        auto result = Pool().Allocate(static_cast<std::uint32_t>(data.size()));
        if (!result) {
            Disconnect();
            return;
        }

        auto buffer = std::move(*result);
        std::memcpy(buffer->Writable().data(), data.data(), data.size());
        buffer->Commit(static_cast<std::uint32_t>(data.size()));

        if (!Send(std::move(buffer)).has_value()) {
            Disconnect();
        }
    }

    void OnConnected() override {
        std::cout << "client connected: " << RemoteAddr() << "\n";
    }

    void OnDisconnected() override {
        std::cout << "client disconnected\n";
    }
};
```

## Create A Listener

`Listener` accepts sockets and creates sessions through a factory.

```cpp
iouring_runtime::core::io::SessionFactory factory =
    [](int fd,
       iouring_runtime::core::ring::IoRing& ring,
       iouring_runtime::core::buffer::BufferPool& pool,
       iouring_runtime::core::ContextId)
        -> iouring_runtime::core::io::SessionRef {
    return std::make_shared<EchoSession>(fd, ring, pool);
};

iouring_runtime::core::Address address{
    .host = "0.0.0.0",
    .port = 19090,
};

auto listener = std::make_shared<iouring_runtime::core::io::Listener>(
    *ring, pool, address, std::move(factory), 0);

auto started = listener->Start();
if (!started) {
    return 1;
}
```

## Drive The Ring

The core runtime is explicit: your process owns the event loop.

```cpp
while (!stop_requested.load(std::memory_order_relaxed)) {
    ring->Dispatch(std::chrono::milliseconds{10});
    ring->ProcessPostedTasks();
}

listener->Stop();
```

If you use `GlobalQueue`, drain posted jobs inside the loop:

```cpp
while (auto* queue = global_queue.TryPop()) {
    queue->Execute();
}
```

## Add Idle Timeouts

Use `core_idle_echo` as the reference for inactivity handling:

```bash
CORE_IDLE_ECHO_PORT=19091 \
CORE_IDLE_TIMEOUT_MS=5000 \
./build-core/bin/core_idle_echo
```

Useful hooks:

- `OnTimeoutTick(...)`
- `HasPendingAppWork()`
- `DisconnectAfterFlush()`

## When To Use A Higher Module

Use core directly for binary protocols, experiments, and custom transport
behavior. Use a higher module when the protocol layer already matches the app:

- HTTP app: `iouring_runtime_web::RuntimeWeb`
- TCP reverse proxy: `iouring_runtime_proxy::RuntimeProxy`
- packet game server: `iouring_runtime_game::RuntimeGame`

See `docs/usage-examples.md` for those snippets.
