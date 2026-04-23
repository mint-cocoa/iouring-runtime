# API Reference

The installable package exports one public target:

- `iouring_runtime::Runtime`

Stable public headers are grouped under:

- `iouring_runtime/core/...`

Main public types:

- `iouring_runtime::core::ring::IoRing`
- `iouring_runtime::core::io::Listener`
- `iouring_runtime::core::io::Session`
- `iouring_runtime::core::buffer::SendBuffer`
- `iouring_runtime::core::buffer::RecvBuffer`
- `iouring_runtime::core::buffer::SendQueue`
- `iouring_runtime::core::job::GlobalQueue`
- `iouring_runtime::core::job::JobQueue`
- `iouring_runtime::core::job::JobTimer`

This repository does not export higher-level protocol or application layers.

When `BUILD_WEB=ON`, an additional package is produced:

- `iouring_runtime_web::RuntimeWeb`
- public headers under `iouring_runtime/web/...`
