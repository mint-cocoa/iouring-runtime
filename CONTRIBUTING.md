# Contributing

Thanks for helping improve `iouring-runtime`.

## Before You Start

- Keep changes scoped to the runtime layer.
- Do not add HTTP, WebSocket, storage, or app-domain concerns here.
- Prefer small patches that keep the public API stable.

## Local Setup

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON -DBUILD_EXAMPLES=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

## What Good Contributions Look Like

- Runtime contract clarifications
- Lifecycle, timeout, and shutdown fixes
- Backpressure and queue-behavior improvements
- New runtime-only examples
- Tests that lock down tricky edge cases
- Packaging and installation fixes

## What Does Not Belong Here

- HTTP routing
- WebSocket protocol logic
- ORM or storage integrations
- Game or application-specific handlers

Those should live in higher-level libraries built on top of the runtime.

## Patch Guidelines

- Keep headers and implementation ASCII unless a file already uses Unicode.
- Prefer `iouring_runtime/core/...` include paths in examples, tests, and new code.
- Add tests for behavior changes, especially around shutdown, pending I/O, and timeouts.
- Avoid breaking the installed package surface without updating docs and smoke tests.

## Pull Request Checklist

- Build succeeds
- Relevant tests pass
- Docs or comments are updated when behavior changes
- New public API is reflected in `docs/api-reference.md` when needed
