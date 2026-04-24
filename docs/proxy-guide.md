# Proxy Guide

`iouring_runtime_proxy::RuntimeProxy` is an optional TCP reverse proxy module
built on top of the core `io_uring` runtime.

## Scope

- accepts downstream TCP connections
- can terminate downstream TLS with OpenSSL
- can serve ACME HTTP-01 challenge files from a webroot for `certbot`
- opens an upstream TCP connection per downstream session
- can choose an upstream by downstream TLS SNI when routes are configured
- forwards bytes bidirectionally between downstream and upstream sockets
- applies inactivity timeouts and send-queue backpressure to both sides

This module is a layer-4 stream proxy. It does not parse HTTP beyond the
optional ACME HTTP-01 challenge responder. Host-based routing uses TLS SNI,
so it requires downstream TLS termination and clients that send SNI.

## Build

```bash
cmake -S . -B build-proxy -DCMAKE_BUILD_TYPE=Release -DBUILD_PROXY=ON
cmake --build build-proxy -j$(nproc)
```

## Public Target

- `iouring_runtime_proxy::RuntimeProxy`

## Public Headers

- `iouring_runtime/proxy/TcpProxyServer.h`

## Example

The repository includes `examples/proxy/tcp_reverse_proxy/`.

```bash
TCP_PROXY_LISTEN_HOST=0.0.0.0 \
TCP_PROXY_LISTEN_PORT=18080 \
TCP_PROXY_UPSTREAM_HOST=127.0.0.1 \
TCP_PROXY_UPSTREAM_PORT=8080 \
TCP_PROXY_UPSTREAM_ROUTES='app.example.com=127.0.0.1:3001,*.example.com=127.0.0.1:3002' \
TCP_PROXY_TLS_CERT_FILE=/etc/letsencrypt/live/example.com/fullchain.pem \
TCP_PROXY_TLS_KEY_FILE=/etc/letsencrypt/live/example.com/privkey.pem \
TCP_PROXY_CERTBOT_CHALLENGE_PORT=80 \
TCP_PROXY_CERTBOT_CHALLENGE_WEBROOT=/var/lib/letsencrypt \
./build-proxy/bin/tcp_reverse_proxy
```

Deployment assets for the example live under
`examples/proxy/tcp_reverse_proxy/deploy/`.

## Configuration Highlights

- `listen_host` / `listen_port`: proxy bind address
- `upstream_host` / `upstream_port`: per-connection upstream target
- `upstream_routes`: optional SNI hostname routes. The example binary reads
  these from `TCP_PROXY_UPSTREAM_ROUTES` as
  `hostname=host:port,*.example.com=host:port`; first match wins and the
  default upstream is used when no route matches.
- `downstream_tls.certificate_chain_file`: PEM certificate chain for TLS termination
- `downstream_tls.private_key_file`: PEM private key for TLS termination
- `certbot.challenge_host`: bind address for HTTP-01 challenge responses
- `certbot.challenge_port`: bind port for HTTP-01 challenge responses, usually `80`
- `certbot.challenge_webroot`: directory passed to `certbot --webroot-path`
- `timeouts.connect`: upstream connect deadline
- `timeouts.inactivity`: idle deadline for both downstream and upstream
- `pending_connect_buffer_limit`: bytes buffered while upstream connect is in flight

## Zero-Downtime TLS Reload

When downstream TLS is enabled, the proxy can swap in a new certificate context
without dropping existing sessions. Existing TLS sessions keep using the old
OpenSSL context they were created with, while new sessions use the reloaded
certificate.

The example binary listens for `SIGHUP` and calls
`ReloadDownstreamTlsContext()` when it receives the signal.

If you run it under `systemd`, the provided unit maps `systemctl reload` to
that same `SIGHUP`.

## Production Layout

One straightforward layout for a single-domain deployment is:

- proxy binary: `/usr/local/bin/tcp_reverse_proxy`
- systemd unit: `/etc/systemd/system/tcp_reverse_proxy.service`
- environment file: `/etc/iouring-runtime/tcp_reverse_proxy.env`
- ACME webroot: `/var/lib/letsencrypt`
- live certificate files: `/etc/letsencrypt/live/<domain>/`

Install the example binary and deployment files:

```bash
sudo install -D -m 0755 build-proxy/bin/tcp_reverse_proxy /usr/local/bin/tcp_reverse_proxy
sudo install -D -m 0644 examples/proxy/tcp_reverse_proxy/deploy/tcp_reverse_proxy.service \
  /etc/systemd/system/tcp_reverse_proxy.service
sudo install -D -m 0644 examples/proxy/tcp_reverse_proxy/deploy/tcp_reverse_proxy.env.example \
  /etc/iouring-runtime/tcp_reverse_proxy.env
sudo install -D -m 0755 examples/proxy/tcp_reverse_proxy/deploy/certbot-issue.sh \
  /usr/local/libexec/iouring-runtime/certbot-issue.sh
sudo install -D -m 0755 examples/proxy/tcp_reverse_proxy/deploy/certbot-renew-hook.sh \
  /usr/local/libexec/iouring-runtime/certbot-renew-hook.sh
```

The provided unit runs as `tcp-proxy` and grants only
`CAP_NET_BIND_SERVICE`, so create that account before enabling the service:

```bash
sudo useradd --system --home /var/lib/iouring-runtime --shell /usr/sbin/nologin tcp-proxy
sudo install -d -o root -g root -m 0755 /var/lib/letsencrypt
sudo install -d -o root -g root -m 0755 /etc/iouring-runtime
```

Then edit `/etc/iouring-runtime/tcp_reverse_proxy.env` for your domain,
upstream port, and certificate paths. For the very first issuance, leave
`TCP_PROXY_TLS_CERT_FILE` and `TCP_PROXY_TLS_KEY_FILE` empty so the service can
start before the certificate exists. Until those paths are filled and the
service is restarted, the main listener is not terminating TLS yet.

## Certbot

For Let's Encrypt and `certbot`, point the proxy at the live certificate files:

- `/etc/letsencrypt/live/<domain>/fullchain.pem`
- `/etc/letsencrypt/live/<domain>/privkey.pem`

To let this proxy answer HTTP-01 challenges directly, configure:

- `TCP_PROXY_CERTBOT_CHALLENGE_HOST=0.0.0.0`
- `TCP_PROXY_CERTBOT_CHALLENGE_PORT=80`
- `TCP_PROXY_CERTBOT_CHALLENGE_WEBROOT=/var/lib/letsencrypt`

Start and enable the service before issuing the first certificate so port `80`
is already serving `/.well-known/acme-challenge/*`:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now tcp_reverse_proxy.service
```

Issue the first certificate with the provided helper:

```bash
sudo /usr/local/libexec/iouring-runtime/certbot-issue.sh /etc/iouring-runtime/tcp_reverse_proxy.env
```

After the first certificate is issued, set these two values in
`/etc/iouring-runtime/tcp_reverse_proxy.env`:

- `TCP_PROXY_TLS_CERT_FILE=/etc/letsencrypt/live/<domain>/fullchain.pem`
- `TCP_PROXY_TLS_KEY_FILE=/etc/letsencrypt/live/<domain>/privkey.pem`

Then restart once to turn on downstream TLS:

```bash
sudo systemctl restart tcp_reverse_proxy.service
```

Hook renewals into `systemctl reload`:

```bash
sudo install -D -m 0755 examples/proxy/tcp_reverse_proxy/deploy/certbot-renew-hook.sh \
  /etc/letsencrypt/renewal-hooks/deploy/tcp_reverse_proxy-reload.sh
```

After each renewal, `certbot` runs that deploy hook, which calls
`systemctl reload tcp_reverse_proxy.service`. That sends `SIGHUP`, reloads
the OpenSSL context, and keeps existing TLS sessions alive while new
connections start using the renewed certificate.
