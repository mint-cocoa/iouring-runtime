# webhook_inbox

`webhook_inbox` is a small HTTP webhook capture app backed by
`iouring_runtime_web`.

- `GET /` serves a single-page browser UI.
- `GET /healthz` returns `ok`.
- `POST|PUT|PATCH /hook/:inbox` captures a webhook request.
- `POST|PUT|PATCH /hook/:inbox/*path` captures a webhook request with a path.
- `GET /api/events?inbox=<name>` lists non-expired events.
- `GET /api/events/:id` returns metadata, headers, and body.
- `DELETE /api/events/:id` removes an event.

`WEBHOOK_INBOX_AUTH_TOKEN` protects API reads and deletes when set. Capture
endpoints remain public so external webhook providers can reach them.

For local testing:

```bash
cmake -S . -B build-webhook-inbox -DBUILD_WEB=ON -DBUILD_EXAMPLES=ON
cmake --build build-webhook-inbox --target webhook_inbox
WEBHOOK_INBOX_ROOT=/tmp/webhook-inbox WEBHOOK_INBOX_PORT=3000 \
  WEBHOOK_INBOX_STATIC_ROOT=examples/web/webhook_inbox/static \
  ./build-webhook-inbox/bin/webhook_inbox
```

Example capture:

```bash
curl -X POST \
  -H 'Content-Type: application/json' \
  --data '{"event":"ping"}' \
  http://127.0.0.1:3000/hook/default
```
