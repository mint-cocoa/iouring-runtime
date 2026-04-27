# webhook_inbox

`webhook_inbox` is a small operations event inbox backed by
`iouring_runtime_web`. It captures GitHub Actions, Argo CD, and homelab service
webhooks, persists them to an append-only JSONL log, and keeps a recent
in-memory index for the UI.

- `GET /` serves a single-page browser UI.
- `GET /healthz` returns `ok`.
- `POST /hooks/github` captures a GitHub webhook.
- `POST /hooks/argocd` captures an Argo CD webhook.
- `POST|PUT|PATCH /hook/:inbox` captures a generic webhook request.
- `POST|PUT|PATCH /hook/:inbox/*path` captures a generic webhook request with a path.
- `GET /api/events?service=<name>&q=<text>&limit=<n>` lists recent events.
- `GET /api/events/:id` returns metadata, headers, and body.
- `DELETE /api/events/:id` removes an event.

`WEBHOOK_INBOX_AUTH_TOKEN` protects API reads and deletes when set. Capture
endpoints remain public so external webhook providers can reach them.

Events are appended to `${WEBHOOK_INBOX_ROOT}/events.jsonl`. The app derives
common fields such as `service`, `event_type`, `status`, `source`, and
`message` without an external JSON library; the raw body and headers are still
stored for inspection.

For local testing:

```bash
cmake -S . -B build-webhook-inbox -DBUILD_WEB=ON -DBUILD_EXAMPLES=ON
cmake --build build-webhook-inbox --target webhook_inbox
WEBHOOK_INBOX_ROOT=/tmp/webhook-inbox WEBHOOK_INBOX_PORT=3000 \
  WEBHOOK_INBOX_STATIC_ROOT=app/examples/web/webhook_inbox/static \
  ./build-webhook-inbox/bin/webhook_inbox
```

Example capture:

```bash
curl -X POST \
  -H 'Content-Type: application/json' \
  -H 'X-GitHub-Event: workflow_run' \
  --data '{"repository":{"full_name":"mint-cocoa/iouring-runtime"},"status":"completed","conclusion":"success"}' \
  http://127.0.0.1:3000/hooks/github
```
