import os
import time
import asyncio
import logging
import uuid
import httpx
from pathlib import Path
from typing import Optional
from fastapi import APIRouter, File, Form, HTTPException, Request, Response, UploadFile
from urllib.parse import urlparse

from ..state import get_state, GlobalState, _instances_lock, _instances, DEFAULT_STREAM_URL
from ..models import TokenExchangeRequest, DownloadRequest, NextRequest, QueueRemoveRequest
from ..services.socket import manager, get_next_state_seq
from ..services.downloader import download_video, enqueue_video, build_entry
from ..services.tving import (
    TvingPlaylistError,
    build_tving_entry_from_cookie,
    get_registered_playlist,
)
from ..utils.security import ensure_authorized
from ..utils.payloads import build_state_update_payload, _control_info
from ..dependencies import (
    _create_download_task, _update_download_task, _get_download_task, 
    _cleanup_youtube_assets, get_effective_time
)
from ..config import (
    CONTROL_MODE, CONTROL_TTL_SECONDS
)

router = APIRouter()
logger = logging.getLogger(__name__)
TVING_COOKIE_FILE_PATH = Path(os.getenv("COCOATUBE_TVING_COOKIE_FILE", "/app/cookie.txt"))

def _controller_expired(state: GlobalState, now_ts: float) -> bool:
    if CONTROL_MODE == "open":
        return False
    if not CONTROL_TTL_SECONDS or CONTROL_TTL_SECONDS <= 0:
        return False
    last = state.controller_last_intent_at
    if last is None:
        return False
    return (now_ts - last) > CONTROL_TTL_SECONDS

def _is_intent_allowed(state: GlobalState, origin_client_id: Optional[str], now_ts: float) -> bool:
    if CONTROL_MODE == "open":
        return True
    if CONTROL_MODE != "first":
        return True

    if not origin_client_id:
        return False

    if state.controller_client_id is None or _controller_expired(state, now_ts):
        state.controller_client_id = origin_client_id
        state.controller_acquired_at = now_ts
        state.controller_last_intent_at = now_ts
        return True

    if origin_client_id == state.controller_client_id:
        state.controller_last_intent_at = now_ts
        return True

    return False

async def broadcast_state_update(state: GlobalState, instance_id: str, origin: Optional[dict] = None):
    await manager.broadcast({
        "type": "STATE_UPDATE",
        "seq": get_next_state_seq(),
        "origin": origin,
        "payload": build_state_update_payload(state),
    }, instance_id=instance_id)

async def _advance_queue(state: GlobalState, instance_id: str, origin: Optional[dict] = None, *, cleanup_prev: bool = False):
    prev_meta = dict(state.metadata or {})

    # Helper imports inside function to avoid circular if needed
    from ..dependencies import _is_youtube_meta

    if state.queue:
        next_entry = state.queue.pop(0)
        state.current_video_url = next_entry["url"]
        state.metadata = {
            "id": next_entry.get("id"),
            "path": next_entry.get("path"),
            "source": next_entry.get("source"),
            "title": next_entry.get("title"),
            "duration": next_entry.get("duration"),
            "thumbnail": next_entry.get("thumbnail"),
            "ext": next_entry.get("ext"),
        }
        state.hls_pause_pdt_ms = None
        state.current_time = 0.0
        state.is_playing = True
        state.start_at = time.time()
        state.pause_timestamp = None
    else:
        # Queue is empty - stop VOD playback (live stream is separate)
        state.current_video_url = None
        state.is_playing = False
        state.current_time = 0.0
        state.start_at = None
        state.pause_timestamp = None
        state.metadata = {}
        state.hls_pause_pdt_ms = None

    await broadcast_state_update(state, instance_id, origin)
    if cleanup_prev and _is_youtube_meta(prev_meta):
        asyncio.create_task(asyncio.to_thread(_cleanup_youtube_assets, prev_meta))


@router.post("/discord/token")
async def exchange_discord_token(req: TokenExchangeRequest):
    client_id = os.getenv("DISCORD_CLIENT_ID")
    client_secret = os.getenv("DISCORD_CLIENT_SECRET")
    
    if not client_id or not client_secret:
        raise HTTPException(status_code=500, detail="Server misconfigured (missing Discord credentials)")

    data = {
        "client_id": client_id,
        "client_secret": client_secret,
        "grant_type": "authorization_code",
        "code": req.code,
    }
    
    async with httpx.AsyncClient() as client:
        resp = await client.post("https://discord.com/api/oauth2/token", data=data)
        if resp.is_error:
            logger.error(f"Discord Token Exchange Failed: {resp.text}")
            raise HTTPException(status_code=400, detail="Failed to exchange token")
        
        return resp.json()

@router.post("/download")
async def download_endpoint(request: DownloadRequest):
    stream = (request.stream or "file").lower()
    if stream not in {"file", "hls"}:
        raise HTTPException(status_code=400, detail="stream must be 'file' or 'hls'")

    # Use instance_id from request, default to "default" for backwards compatibility
    instance_id = request.instance_id or "default"
    
    task_id = f"dl_{uuid.uuid4().hex[:12]}"
    task = _create_download_task(task_id, request.url)
    
    async def background_download():
        try:
            state = get_state(instance_id)
            _update_download_task(task_id, status="downloading")
            
            await manager.broadcast({
                "type": "DOWNLOAD_PROGRESS",
                "task_id": task_id,
                "status": "downloading",
                "url": request.url,
            }, instance_id=instance_id)
            
            entry = await asyncio.to_thread(enqueue_video, state, request.url, stream)
            
            _update_download_task(task_id, status="completed", entry=entry, progress=100)
            
            autostart = False
            if not state.current_video_url and not state.is_playing:
                state.current_video_url = entry["url"]
                state.metadata = {
                    "id": entry.get("id"),
                    "path": entry.get("path"),
                    "source": entry.get("source"),
                    "title": entry.get("title"),
                    "duration": entry.get("duration"),
                    "thumbnail": entry.get("thumbnail"),
                    "ext": entry.get("ext"),
                }
                state.is_playing = True
                state.current_time = 0.0
                state.start_at = time.time()
                state.pause_timestamp = None
                autostart = True
                if state.queue and state.queue[-1]["id"] == entry["id"]:
                    state.queue.pop()

            await broadcast_state_update(state, instance_id)
            
            await manager.broadcast({
                "type": "DOWNLOAD_COMPLETE",
                "task_id": task_id,
                "entry": entry,
                "autostart": autostart,
            }, instance_id=instance_id)
            
        except Exception as exc:
            logger.error(f"Background download error: {exc}")
            _update_download_task(task_id, status="failed", error=str(exc))
            await manager.broadcast({
                "type": "DOWNLOAD_FAILED",
                "task_id": task_id,
                "error": str(exc),
            }, instance_id=instance_id)
    
    asyncio.create_task(background_download())
    
    return {"status": "pending", "task_id": task_id, "message": "Download started in background"}

@router.get("/download/status/{task_id}")
async def download_status(task_id: str):
    task = _get_download_task(task_id)
    if not task:
        raise HTTPException(status_code=404, detail="Task not found")
    return task

@router.get("/tving/playlist/{playlist_id}.m3u8")
async def tving_playlist(playlist_id: str):
    playlist = get_registered_playlist(playlist_id)
    if not playlist:
        raise HTTPException(status_code=404, detail="TVING playlist not found or expired")
    return Response(
        content=playlist["manifest"],
        media_type="application/vnd.apple.mpegurl",
        headers={"Cache-Control": "no-store"},
    )

@router.post("/tving/cookie-play")
async def tving_cookie_play(
    cookie_file: Optional[UploadFile] = File(None),
    media_code: str = Form("C51850"),
    instance_id: str = Form("default"),
    use_cookie_file: bool = Form(False),
):
    try:
        if use_cookie_file:
            cookie_path = TVING_COOKIE_FILE_PATH
            if not cookie_path.exists():
                local_cookie_path = Path.cwd() / "cookie.txt"
                cookie_path = local_cookie_path if local_cookie_path.exists() else cookie_path
            if not cookie_path.exists():
                raise TvingPlaylistError(
                    "Server cookie.txt was not found.",
                    status=404,
                    details={"path": str(TVING_COOKIE_FILE_PATH)},
                )
            raw_cookie = cookie_path.read_bytes()
        else:
            if cookie_file is None:
                raise TvingPlaylistError("Cookie file is required.", status=400)
            raw_cookie = await cookie_file.read()

        if len(raw_cookie) > 1024 * 1024:
            raise TvingPlaylistError("Cookie file is too large.", status=400)

        cookie_text = raw_cookie.decode("utf-8", errors="replace")
        entry, registered = await build_tving_entry_from_cookie(cookie_text, media_code)
    except TvingPlaylistError as exc:
        detail = {"message": str(exc), "details": exc.details} if exc.details else str(exc)
        logger.warning(f"TVING cookie play failed: {detail}")
        raise HTTPException(status_code=exc.status, detail=detail)
    except Exception as exc:
        logger.error(f"TVING cookie play error: {exc}")
        raise HTTPException(status_code=500, detail=str(exc))

    state = get_state(instance_id or "default")
    state.current_video_url = entry["url"]
    state.metadata = {
        "id": entry.get("id"),
        "path": entry.get("path"),
        "source": entry.get("source"),
        "title": entry.get("title"),
        "duration": entry.get("duration"),
        "thumbnail": entry.get("thumbnail"),
        "ext": entry.get("ext"),
        "stream_type": entry.get("stream_type"),
        "media_code": entry.get("media_code"),
        "audio_url": entry.get("audio_url"),
        "video_url": entry.get("video_url"),
    }
    state.hls_pause_pdt_ms = None
    state.is_playing = True
    state.current_time = 0.0
    state.start_at = time.time()
    state.pause_timestamp = None

    await broadcast_state_update(state, instance_id or "default")
    return {
        "status": "success",
        "entry": entry,
        "autostart": True,
        "playlist": {
            "id": registered["id"],
            "url": entry["url"],
            "has_audio": bool(registered.get("audio_url")),
            "video_url": registered.get("video_url"),
            "audio_url": registered.get("audio_url"),
        },
    }

@router.post("/play")
async def play_endpoint(request: DownloadRequest):
    try:
        state = get_state("default")
        stream = (request.stream or "file").lower()
        if stream not in {"file", "hls"}:
            raise HTTPException(status_code=400, detail="stream must be 'file' or 'hls'")

        video_url, file_url, hls_url, info = await asyncio.to_thread(download_video, request.url, stream=stream)
        entry = build_entry(video_url, info, file_url=file_url, hls_url=hls_url, stream=stream)

        state.current_video_url = entry["url"]
        state.metadata = {
            "id": entry.get("id"),
            "path": entry.get("path"),
            "source": entry.get("source"),
            "title": entry.get("title"),
            "duration": entry.get("duration"),
            "thumbnail": entry.get("thumbnail"),
            "ext": entry.get("ext"),
        }
        state.is_playing = True
        state.current_time = 0.0
        state.start_at = time.time()
        state.pause_timestamp = None

        await broadcast_state_update(state, "default")
        return {"status": "success", "entry": entry, "queue": state.queue, "autostart": True}
    except Exception as exc:
        logger.error(f"Play error: {exc}")
        raise HTTPException(status_code=500, detail=str(exc))

@router.post("/next")
async def next_endpoint(request: NextRequest):
    instance_id = request.instance_id or "default"
    state = get_state(instance_id)
    now_ts = time.time()
    if not _is_intent_allowed(state, request.client_id, now_ts):
        raise HTTPException(status_code=403, detail={
            "type": "CONTROL_DENIED",
            "controller_client_id": state.controller_client_id,
            "control": _control_info(state),
        })

    origin = {"client_id": request.client_id, "seq": None} if request.client_id else None
    await _advance_queue(state, instance_id, origin=origin, cleanup_prev=True)
    return {"status": "success", "queue": state.queue}

@router.post("/queue/remove")
async def queue_remove_endpoint(request: QueueRemoveRequest):
    instance_id = request.instance_id or "default"
    state = get_state(instance_id)
    now_ts = time.time()
    if not _is_intent_allowed(state, request.client_id, now_ts):
        raise HTTPException(status_code=403, detail={
            "type": "CONTROL_DENIED",
            "controller_client_id": state.controller_client_id,
            "control": _control_info(state),
        })

    before = len(state.queue)
    removed = None
    new_queue: list[dict] = []
    for item in state.queue:
        if removed is None and item.get("id") == request.id:
            removed = item
            continue
        new_queue.append(item)
    state.queue = new_queue

    if removed is None and before:
        raise HTTPException(status_code=404, detail="Queue item not found")

    origin = {"client_id": request.client_id, "seq": None} if request.client_id else None
    await broadcast_state_update(state, instance_id, origin=origin)
    return {"status": "success", "removed": removed, "queue": state.queue}

@router.get("/queue")
async def get_queue(request: Request, instance_id: str = "default"):
    await ensure_authorized(request, instance_id)
    state = get_state(instance_id)
    return {"queue": state.queue}

@router.get("/thumb")
async def proxy_thumbnail(target: str):
    target = (target or "").strip()
    if not target:
        raise HTTPException(status_code=400, detail="target is required")
    parsed = urlparse(target)
    if parsed.scheme not in {"http", "https"}:
        raise HTTPException(status_code=400, detail="Only http(s) targets supported")

    try:
        async with httpx.AsyncClient(timeout=10, follow_redirects=True) as client:
            resp = await client.get(target, headers={
                "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"
            })

            if resp.status_code >= 400:
                logger.warning(f"Thumbnail upstream error: {resp.status_code} for {target}")
                raise HTTPException(status_code=resp.status_code, detail="Thumbnail upstream error")

            headers = {}
            content_type = resp.headers.get("content-type")
            if content_type:
                headers["Content-Type"] = content_type
            return Response(content=resp.content, status_code=resp.status_code, headers=headers)
    except httpx.RequestError as exc:
        logger.error(f"Thumbnail fetch failed for {target}: {exc}")
        raise HTTPException(status_code=502, detail=f"Thumbnail fetch failed: {exc}")
    except Exception as exc:
        logger.error(f"Thumbnail proxy error for {target}: {exc}")
        raise HTTPException(status_code=500, detail=f"Thumbnail proxy error: {exc}")
