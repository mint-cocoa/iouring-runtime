import time
import os
import shutil
import threading
import glob
from typing import Optional
from pathlib import Path
from .state import GlobalState
from .config import HLS_TARGET_LATENCY_SECONDS, YOUTUBE_DIR, HLS_DIR

# Background download task tracking
_download_tasks: dict[str, dict] = {}
_download_tasks_lock = threading.Lock()

def _create_download_task(task_id: str, url: str) -> dict:
    """Create a new download task entry."""
    task = {
        "id": task_id,
        "url": url,
        "status": "pending",  # pending, downloading, transcoding, completed, failed
        "progress": 0,
        "error": None,
        "entry": None,
        "created_at": time.time(),
    }
    with _download_tasks_lock:
        _download_tasks[task_id] = task
    return task

def _update_download_task(task_id: str, **kwargs):
    """Update a download task."""
    with _download_tasks_lock:
        if task_id in _download_tasks:
            _download_tasks[task_id].update(kwargs)

def _get_download_task(task_id: str) -> Optional[dict]:
    """Get a download task by ID."""
    with _download_tasks_lock:
        return _download_tasks.get(task_id, {}).copy() if task_id in _download_tasks else None

def get_effective_time(state: GlobalState) -> float:
    # Calculate playback time using start_at when playing to keep clients in sync.
    if not state.is_playing:
        return state.current_time or 0.0
    
    # If paused, start_at is usually None or invalid, but we rely on current_time
    if state.start_at is None:
        return state.current_time or 0.0

    elapsed = time.time() - state.start_at
    return max(0.0, (state.current_time or 0.0) + elapsed)

def _is_hls_now(state: GlobalState) -> bool:
    """HLS 스트림인지 확인 (PDT 동기화 활성화 여부 결정)"""
    url = state.current_video_url or ""
    source = (state.metadata or {}).get("source", "")
    
    # HLS 스트림 조건:
    # 1. URL이 .m3u8으로 끝나거나
    # 2. metadata.source가 'hls'인 경우
    if ".m3u8" in url.lower():
        return True
    if source == "hls":
        return True
    
    return False

def _hls_target_pdt_ms(state: GlobalState, server_now: float) -> int:
    return int((server_now - HLS_TARGET_LATENCY_SECONDS) * 1000)

def _is_youtube_meta(meta: dict) -> bool:
    return meta and meta.get("source") == "youtube" and meta.get("id")

def _cleanup_youtube_assets(meta: dict):
    # Delete file/folder associated with the previous YouTube video to save space
    # 1. Main media file (mp4/mkv/etc) in downloads/local/youtube/<id>.*
    # 2. HLS directory in downloads/hls/<id>/
    
    video_id = meta.get("id")
    if not video_id:
        return

    # 1. Remove associated files in YOUTUBE_DIR
    for p in glob.glob(os.path.join(YOUTUBE_DIR, f"{video_id}.*")):
        try:
            os.remove(p)
            # Try removing info json sidecars too
            info = f"{p}.info.json"
            if os.path.exists(info):
                os.remove(info)
        except Exception:
            pass

    # 2. Remove HLS directory
    try:
        out_dir = Path(HLS_DIR).resolve() / video_id
        if out_dir.is_dir():
            for p in out_dir.glob("*"):
                try:
                    p.unlink()
                except Exception:
                    pass
            try:
                out_dir.rmdir()
            except Exception:
                pass
    except Exception:
        pass
