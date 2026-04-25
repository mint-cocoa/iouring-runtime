# dropapp

`dropapp` is a TTL file drop app backed by `iouring_runtime_web`.

- `GET /` serves a single-page browser UI.
- `GET /healthz` returns `ok`.
- `GET /api/files` lists non-expired files.
- `POST /api/files` streams a raw request body to storage. The original
  filename is read from `X-Dropapp-Filename`.
- `GET /d/:id/*filename` downloads a stored file.
- `DELETE /api/files/:id` removes a stored file.

Uploads and deletes require `Authorization: Bearer <token>` when
`DROPAPP_AUTH_TOKEN` is set. Download links are public to anyone who knows the
URL.

For local testing:

```bash
cmake -S . -B build-dropapp -DBUILD_WEB=ON -DBUILD_EXAMPLES=ON -DBUILD_TESTS=ON
cmake --build build-dropapp --target dropapp
DROPAPP_ROOT=/tmp/dropapp DROPAPP_PORT=3000 ./build-dropapp/bin/dropapp
```

Then open `http://127.0.0.1:3000/`.

Example upload:

```bash
curl -X POST \
  -H 'X-Dropapp-Filename: example.txt' \
  --data-binary @example.txt \
  http://127.0.0.1:3000/api/files
```
