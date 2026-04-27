import os
import logging
import asyncio
from fastapi import FastAPI, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.staticfiles import StaticFiles
from fastapi.responses import StreamingResponse
from starlette.status import HTTP_206_PARTIAL_CONTENT, HTTP_416_REQUESTED_RANGE_NOT_SATISFIABLE
import mimetypes

from .routers import api, websocket, proxy
from .utils.security import access_guard_middleware
from .config import DOWNLOAD_DIR, LOCAL_DIR
from .routers.api import broadcast_state_update, get_state, get_effective_time, _advance_queue

# Setup Logging
logging.basicConfig(level=logging.INFO)
logging.getLogger("httpx").setLevel(logging.WARNING)
logger = logging.getLogger(__name__)

app = FastAPI()

# Middleware
app.middleware("http")(access_guard_middleware)
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
    expose_headers=["Content-Length", "Content-Range", "Content-Type", "Accept-Ranges"],
)

# Static Mounts
app.mount("/files", StaticFiles(directory=DOWNLOAD_DIR), name="files")
app.mount("/local", StaticFiles(directory=LOCAL_DIR), name="local")

# Routers
app.include_router(api.router, prefix="/api")
app.include_router(websocket.router)
app.include_router(proxy.router, prefix="/proxy")

# Static HLS endpoints (simple file serving from disk)
from fastapi.responses import FileResponse
from fastapi import HTTPException
from pathlib import Path
from .config import HLS_DIR

def hls_range_stream(file_path: str, request: Request):
    """HLS 파일을 Range 요청 지원으로 스트리밍"""
    file_size = os.path.getsize(file_path)
    range_header = request.headers.get("Range")
    
    headers = {
        "Accept-Ranges": "bytes",
        "Content-Encoding": "identity",
    }
    
    # MIME 타입 및 캐시 설정
    suffix = Path(file_path).suffix.lower()
    if suffix == ".m3u8":
        media_type = "application/vnd.apple.mpegurl"
        headers["Cache-Control"] = "no-cache, no-store"
    elif suffix == ".ts":
        media_type = "video/mp2t"
        headers["Cache-Control"] = "public, max-age=3600"
    elif suffix == ".mp4":
        media_type = "video/mp4"
        headers["Cache-Control"] = "public, max-age=3600"
    elif suffix == ".m4s":
        media_type = "video/iso.segment"
        headers["Cache-Control"] = "public, max-age=3600"
    else:
        media_type = "application/octet-stream"
        headers["Cache-Control"] = "no-cache"
    
    headers["Content-Type"] = media_type
    
    start, end, status_code = 0, file_size - 1, 200
    
    if range_header:
        # Range: bytes=0-1023 파싱
        try:
            range_str = range_header.replace("bytes=", "").split("-")
            start = int(range_str[0]) if range_str[0] else 0
            end = int(range_str[1]) if range_str[1] else file_size - 1
            if start >= file_size or end < start:
                raise HTTPException(HTTP_416_REQUESTED_RANGE_NOT_SATISFIABLE)
        except:
            raise HTTPException(HTTP_416_REQUESTED_RANGE_NOT_SATISFIABLE)
        
        headers["Content-Range"] = f"bytes {start}-{end}/{file_size}"
        headers["Content-Length"] = str(end - start + 1)
        status_code = HTTP_206_PARTIAL_CONTENT
    
    def iter_file():
        with open(file_path, "rb") as f:
            f.seek(start)
            while True:
                pos = f.tell()
                if pos > end:
                    break
                chunk_size = min(65536, end - pos + 1)  # 64KB 청크 (8KB -> 64KB 증량)
                data = f.read(chunk_size)
                if not data:
                    break
                yield data
    
    headers["Content-Length"] = str(end - start + 1) if range_header else str(file_size)
    return StreamingResponse(iter_file(), status_code=status_code, headers=headers)


@app.get("/hls/{video_id}/{hls_path:path}")
async def hls_endpoint(video_id: str, hls_path: str, request: Request):
    root = Path(HLS_DIR).resolve()
    target = (root / video_id / hls_path).resolve()

    # 경로 이스케이프 보안 강화 (Path Traversal 방지)
    # is_relative_to needs Python 3.9+
    try:
        if not target.is_relative_to(root) or not target.is_file():
             raise HTTPException(status_code=404, detail="Not found")
    except AttributeError:
        # Fallback for Python < 3.9
        if root not in target.parents or not target.is_file():
            raise HTTPException(status_code=404, detail="Not found")

    return hls_range_stream(str(target), request)


# Startup Tasks (Heartbeat)
async def _heartbeat_loop():
    interval = 2.0
    end_threshold = 0.5
    last_end_for_media_id = None
    
    # Import internals
    from .state import _instances, _instances_lock
    import time
    
    while True:
        try:
            with _instances_lock:
                active_instances = list(_instances.items())
                
            for instance_id, state in active_instances:
                if state.is_playing and state.current_video_url:
                    meta = state.metadata or {}
                    media_id = meta.get("id")
                    dur = meta.get("duration") or 0
                    try:
                        dur = float(dur)
                    except:
                        dur = 0
                    
                    if dur and dur > 0:
                        effective = get_effective_time(state)
                        if effective >= max(0.0, dur - end_threshold):
                            if media_id and last_end_for_media_id == media_id:
                                pass
                            else:
                                last_end_for_media_id = media_id
                                await _advance_queue(state, instance_id, origin=None, cleanup_prev=True)
                                
                await broadcast_state_update(state, instance_id, origin=None)
                
        except Exception as exc:
            logger.warning(f"Heartbeat error: {exc}")
        await asyncio.sleep(interval)

@app.on_event("startup")
async def startup_tasks():
    asyncio.create_task(_heartbeat_loop())

@app.on_event("shutdown")
async def shutdown_tasks():
    from .routers.proxy import close_http_client
    await close_http_client()
