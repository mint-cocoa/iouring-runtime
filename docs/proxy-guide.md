# Proxy Guide

`iouring::stream` is an optional layer-4 TCP reverse proxy
built on the core runtime.

It can:

- accept downstream TCP connections
- open one upstream TCP connection per downstream session
- forward bytes in both directions
- apply inactivity timeouts and send-queue backpressure
- terminate downstream TLS with OpenSSL
- route TLS traffic by SNI
- run a separate ACME HTTP-01 challenge listener for `certbot`
- write runtime metrics snapshots

The stream proxy path does not parse HTTP traffic. Host routing uses TLS SNI.
The example binary can also compose a separate ACME HTTP-01 server, similar to
keeping Nginx `stream` and `http` responsibilities in different blocks.

## Build And Run

Start an upstream:

```bash
python3 -m http.server 8080
```

Build the proxy example:

```bash
cmake -S . -B build-proxy \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_STREAM=ON \
  -DBUILD_TESTS=OFF
cmake --build build-proxy -j$(nproc)
cmake --install build-proxy --prefix /tmp/iouring-install

cmake -S ../iouring-runtime-examples -B ../iouring-runtime-examples/build-proxy \
  -DCMAKE_PREFIX_PATH=/tmp/iouring-install \
  -DBUILD_CORE_EXAMPLES=OFF \
  -DBUILD_HTTP_EXAMPLES=OFF \
  -DBUILD_GAME_EXAMPLES=OFF \
  -DBUILD_ACTIVITY_EXAMPLES=OFF
cmake --build ../iouring-runtime-examples/build-proxy --target tcp_reverse_proxy -j$(nproc)
```

Run:

```bash
TCP_PROXY_LISTEN_HOST=127.0.0.1 \
TCP_PROXY_LISTEN_PORT=18080 \
TCP_PROXY_UPSTREAM_HOST=127.0.0.1 \
TCP_PROXY_UPSTREAM_PORT=8080 \
../iouring-runtime-examples/build-proxy/bin/tcp_reverse_proxy
```

Try it:

```bash
curl http://127.0.0.1:18080/
```

## Use From CMake

```cmake
find_package(iouring_proxy CONFIG REQUIRED)

add_executable(my_proxy src/main.cpp)
target_link_libraries(my_proxy PRIVATE
    iouring::stream
)
target_compile_features(my_proxy PRIVATE cxx_std_23)
```

```cpp
#include <iouring/stream/TcpProxyServer.h>
```

Minimal server:

```cpp
int main() {
    iouring::stream::TcpProxyConfig config;
    config.listen_host = "0.0.0.0";
    config.listen_port = 18080;
    config.upstream_host = "127.0.0.1";
    config.upstream_port = 8080;
    config.worker_count = 1;

    iouring::stream::TcpProxyServer server(config);
    iouring::stream::TcpProxyServer::InstallStopSignalHandlers();

    server.Start();
    iouring::stream::TcpProxyServer::WaitForStopSignal(
        std::chrono::milliseconds{100});
    server.Stop();
}
```

## Example Environment

The example binary reads configuration from environment variables:

```bash
TCP_PROXY_LISTEN_HOST=0.0.0.0
TCP_PROXY_LISTEN_PORT=18080
TCP_PROXY_UPSTREAM_HOST=127.0.0.1
TCP_PROXY_UPSTREAM_PORT=8080
TCP_PROXY_WORKERS=4
TCP_PROXY_LOG_LEVEL=info
```

SNI routes:

```bash
TCP_PROXY_UPSTREAM_ROUTES='app.example.com=127.0.0.1:3001,*.example.com=127.0.0.1:3002'
```

TLS termination:

```bash
TCP_PROXY_TLS_CERT_FILE=/etc/letsencrypt/live/example.com/fullchain.pem
TCP_PROXY_TLS_KEY_FILE=/etc/letsencrypt/live/example.com/privkey.pem
```

ACME HTTP-01 responder:

```bash
TCP_PROXY_CERTBOT_CHALLENGE_HOST=0.0.0.0
TCP_PROXY_CERTBOT_CHALLENGE_PORT=80
TCP_PROXY_CERTBOT_CHALLENGE_WEBROOT=/var/lib/letsencrypt
```

The ACME responder is implemented by `AcmeHttpChallengeServer`; the example
starts it next to `TcpProxyServer` when the challenge port and webroot are set.

Metrics snapshot:

```bash
TCP_PROXY_METRICS_FILE=/run/iouring-runtime/tcp_reverse_proxy.metrics.json
TCP_PROXY_METRICS_INTERVAL_MS=1000
```

## TLS Reload

When TLS is enabled, `TcpProxyServer::ReloadDownstreamTlsContext()` swaps in a
new OpenSSL context for new sessions. Existing TLS sessions keep their old
context.

The example binary maps `SIGHUP` to reload:

```bash
kill -HUP <proxy-pid>
```

Under systemd, the provided unit maps `systemctl reload` to the same signal.

## Systemd Deployment

Deployment assets live in:

```text
iouring-runtime-examples/stream/tcp_reverse_proxy/deploy/
```

Install the binary and service files:

```bash
sudo install -D -m 0755 build-proxy/bin/tcp_reverse_proxy /usr/local/bin/tcp_reverse_proxy
sudo install -D -m 0644 iouring-runtime-examples/stream/tcp_reverse_proxy/deploy/tcp_reverse_proxy.service \
  /etc/systemd/system/tcp_reverse_proxy.service
sudo install -D -m 0644 iouring-runtime-examples/stream/tcp_reverse_proxy/deploy/tcp_reverse_proxy.env.example \
  /etc/iouring-runtime/tcp_reverse_proxy.env
```

Create runtime directories and account:

```bash
sudo useradd --system --home /var/lib/iouring-runtime --shell /usr/sbin/nologin tcp-proxy
sudo install -d -o root -g root -m 0755 /etc/iouring-runtime
sudo install -d -o root -g root -m 0755 /var/lib/letsencrypt
```

Edit:

```text
/etc/iouring-runtime/tcp_reverse_proxy.env
```

Start:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now tcp_reverse_proxy.service
```

## Certbot Flow

Install helper scripts:

```bash
sudo install -D -m 0755 iouring-runtime-examples/stream/tcp_reverse_proxy/deploy/certbot-issue.sh \
  /usr/local/libexec/iouring-runtime/certbot-issue.sh
sudo install -D -m 0755 iouring-runtime-examples/stream/tcp_reverse_proxy/deploy/certbot-renew-hook.sh \
  /etc/letsencrypt/renewal-hooks/deploy/tcp_reverse_proxy-reload.sh
```

First issuance:

```bash
sudo /usr/local/libexec/iouring-runtime/certbot-issue.sh \
  /etc/iouring-runtime/tcp_reverse_proxy.env
```

After certificates exist, set:

```text
TCP_PROXY_TLS_CERT_FILE=/etc/letsencrypt/live/<domain>/fullchain.pem
TCP_PROXY_TLS_KEY_FILE=/etc/letsencrypt/live/<domain>/privkey.pem
```

Then restart once:

```bash
sudo systemctl restart tcp_reverse_proxy.service
```

Future renewals reload the service through the deploy hook.

## Related Examples

These web examples are useful proxy upstreams:

- `iouring-runtime-examples/http/status_server/`
- `iouring-runtime-examples/http/speedtest_server/`
- `iouring-runtime-examples/http/file_store_server/`
- `iouring-runtime-examples/http/dropapp/`
- `iouring-runtime-examples/http/webhook_inbox/`
