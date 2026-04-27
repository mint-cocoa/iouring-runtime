# File Store Server

This example is a small file store backed by `iouring_runtime_web`.

- `GET /` serves a minimal browser UI.
- `GET /api/files` lists stored files.
- `PUT /api/files/*path` streams upload chunks directly to a `.part` file.
- `GET /files/*path` downloads files in chunks.
- `DELETE /api/files/*path` removes files.

Uploads use the web module's streaming request-body route support, so request
bodies do not accumulate in `HttpRequest::body`.

For local testing:

```bash
cmake -S . -B build-proxy -DBUILD_WEB=ON -DBUILD_EXAMPLES=ON
cmake --build build-proxy --target file_store_server
FILE_STORE_ROOT=/tmp/iouring-runtime-files FILE_STORE_PORT=3012 \
  ./build-proxy/bin/file_store_server
```

Then open `http://127.0.0.1:3012/`.

For systemd deployment:

```bash
sudo install -D -m 0755 build-proxy/bin/file_store_server /usr/local/bin/file_store_server
sudo install -d -o tcp-proxy -g tcp-proxy -m 0755 /var/lib/iouring-runtime/files
sudo install -D -m 0644 app/examples/web/file_store_server/deploy/file_store_server.service \
  /etc/systemd/system/file_store_server.service
sudo install -D -m 0644 app/examples/web/file_store_server/deploy/file_store_server.env.example \
  /etc/iouring-runtime/file_store_server.env
sudo systemctl daemon-reload
sudo systemctl enable --now file_store_server.service
```

To expose it through the reverse proxy, add the route to
`TCP_PROXY_UPSTREAM_ROUTES`:

```text
files.mintcocoa.cc=127.0.0.1:3012
```

Then restart the proxy:

```bash
sudo systemctl restart tcp_reverse_proxy.service
```

If `FILE_STORE_AUTH_TOKEN` is set in
`/etc/iouring-runtime/file_store_server.env`, upload and delete requests must
send `Authorization: Bearer <token>`.
