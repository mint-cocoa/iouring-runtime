# Getting Started

`iouring-runtime` is the low-level runtime substrate for servers that want to
build directly on `io_uring`.

## Public target

- `iouring_runtime::Runtime`

## Public headers

- `iouring_runtime/core/...`

## First examples

- `examples/runtime/core_echo/`
- `examples/runtime/core_idle_echo/`

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Minimal usage shape

1. create an `IoRing`
2. create a `BufferPool`
3. define a `Session` subclass
4. provide a `SessionFactory`
5. construct a `Listener`
6. drive `Dispatch()` in a loop

See `docs/runtime-guide.md` for a fuller explanation.
