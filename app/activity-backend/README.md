# Activity Backend

FastAPI backend for the Discord Activity video room. This is the first migration
step from `mint-cocoa/youtube-backend` into `mint-cocoa/iouring-runtime`.

The app intentionally keeps the existing API surface stable:

- `GET /ws` WebSocket for playback state and anonymous chat.
- `POST /api/download` and queue control endpoints.
- `GET /api/queue`.
- `GET /proxy/hls` and related proxy helpers.
- `GET /hls/*` for local HLS files.

## Local Run

```bash
python -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt
uvicorn backend.app.main:app --host 127.0.0.1 --port 8000 --no-access-log
```

## Docker

```bash
docker build -t activity-backend app/activity-backend
docker run --rm -p 8000:8000 activity-backend
```

If TVING cookie playback is used, mount a cookie file read-only:

```bash
docker run --rm -p 8000:8000 \
  -v "$PWD/cookie.txt:/app/cookie.txt:ro" \
  activity-backend
```

## Production Compose

The production compose file keeps the migrated backend on the existing
`youtube-backend_default` Docker network with the `backend` alias, so the current
frontend Nginx container can continue proxying `/api`, `/ws`, `/hls`, and
`/proxy` without frontend changes.

Required host files and directories:

```bash
sudo install -d -m 0750 /etc/iouring-runtime
sudo install -d -m 0750 /var/lib/iouring-runtime/activity-backend/downloads
sudo install -m 0600 activity-backend.env /etc/iouring-runtime/activity-backend.env
sudo install -m 0640 cookie.txt /etc/iouring-runtime/activity-backend.cookie.txt
```

Deploy:

```bash
cd app/activity-backend
docker compose up -d --build backend
```

## Reverse Proxy

The intended cutover target is a dedicated backend route, for example:

```text
activity-api.mintcocoa.cc=127.0.0.1:8010
```

The frontend can then point at:

```env
VITE_API_BASE=https://activity-api.mintcocoa.cc
VITE_WS_BASE=wss://activity-api.mintcocoa.cc
```
