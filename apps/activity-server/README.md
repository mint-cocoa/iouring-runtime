# Activity Server

C++ io_uring backend for the Discord Activity video room.

This is the migration target that replaces the temporary FastAPI
`apps/activity-backend` service. The first C++ cut supports the core runtime
surface used by the current frontend:

- `GET /healthz`
- `GET /api/queue`
- `POST /api/download`
- `GET /api/download/status/{task_id}`
- `POST /api/next`
- `POST /api/queue/remove`
- `GET /hls/{video_id}/{path}`
- `GET /ws` WebSocket with `HELLO`, anonymous `CHAT_MESSAGE`, and state updates

`yt-dlp` and `ffmpeg` are executed as subprocesses for media ingest and HLS
packaging. TVING cookie playback and the generic `/proxy/*` helpers still need
to be ported before the Python backend can be deleted completely.

## Local Run

```bash
cmake -S . -B build-activity-server -G Ninja \
  -DBUILD_TESTS=OFF \
  -DBUILD_WEB=OFF \
  -DBUILD_PROXY=OFF \
  -DBUILD_GAME=OFF \
  -DBUILD_EXAMPLES=OFF \
  -DBUILD_ACTIVITY_SERVER=ON
cmake --build build-activity-server --target activity_server

ACTIVITY_PORT=8010 \
ACTIVITY_DOWNLOAD_DIR=/tmp/iouring-activity-server/downloads \
./build-activity-server/bin/activity_server
```

## Docker

```bash
docker build -f apps/activity-server/Dockerfile -t iouring-runtime-activity-server:main .
docker run --rm -p 8010:8000 iouring-runtime-activity-server:main
```

## Production Compose

The compose file keeps the backend attached to the existing
`youtube-backend_default` network with the `backend` alias, so the current
frontend Nginx container can continue proxying to `http://backend:8000`.

```bash
sudo install -d -m 0750 /var/lib/iouring-runtime/activity-server/downloads
sudo chown -R 10002:10002 /var/lib/iouring-runtime/activity-server
cd apps/activity-server
sudo docker compose up -d --build backend
```
