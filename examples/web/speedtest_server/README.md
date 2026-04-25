# OpenSpeedTest on iouring_runtime_web

This example serves the OpenSpeedTest UI with `iouring_runtime_web` and handles
the speed-test endpoints directly in C++:

- `GET /downloading` streams the configured download payload.
- `HEAD /downloading` returns the same `Content-Length` without a body.
- `GET /upload` supports OpenSpeedTest ping probes.
- `POST /upload` reads upload traffic and discards the body in the HTTP parser.

The bundled UI files under `public/` come from
<https://github.com/openspeedtest/Speed-Test> and retain the upstream MIT
license notice in `public/License.md`. The large upstream `downloading` file is
not vendored; this server generates the payload at request time.

For local testing:

```bash
cmake -S . -B build-proxy -DBUILD_WEB=ON -DBUILD_EXAMPLES=ON
cmake --build build-proxy --target speedtest_server
SPEEDTEST_HOST=127.0.0.1 SPEEDTEST_PORT=3011 ./build-proxy/bin/speedtest_server
```

Then open `http://127.0.0.1:3011/`.
