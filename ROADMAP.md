# Roadmap

## v0.1

- Stabilize `IoRing`, `Listener`, and `Session`
- Keep the repo runtime-only
- Verify `find_package(iouring_runtime)` install flow
- Ship runtime examples and focused lifecycle tests

## v0.2

- Tighten runtime contract documentation
- Add stronger shutdown, drain, and slow-client coverage
- Improve counters and diagnostics for runtime state
- Clarify the remaining namespace transition story around `iouring_runtime::core::...`

## v0.3

- Strengthen packaging and CI
- Add more runtime-only examples for packet-style protocols
- Clarify extension points for higher-level protocol libraries
- Revisit namespace and header layout for a cleaner long-term public API
