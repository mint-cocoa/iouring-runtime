import os
import time
import uuid
import glob
import json
import logging
import threading
import yt_dlp
from pathlib import Path
from typing import Optional, List
from urllib.parse import urlparse, unquote

from ..config import YOUTUBE_DIR, LOCAL_DIR, DOWNLOAD_DIR
from ..state import GlobalState
from ..utils.video import _ensure_hls, _probe_duration_seconds

logger = logging.getLogger(__name__)

# Helper functions for proxying HLS
def _proxy_hls_url(remote_url: str) -> str:
    # Keep the proxied URL same-origin and URL-encode the upstream location.
    # Note: we need to handle imports correctly or move this content
    # For now, simplistic URL encoding
    from urllib.parse import urlencode
    return "/proxy/hls?" + urlencode({"url": remote_url})

def _looks_like_hls_manifest(url: str) -> bool:
    u = (url or "").strip().lower()
    return ".m3u8" in u

def _looks_like_mist_stream(url: str) -> bool:
    """Detect MistServer live stream URLs that should not be downloaded."""
    u = (url or "").strip().lower()
    
    # Known MistServer domains
    mist_domains = ['stream.mintcocoa.cc', 'hls.mintcocoa.cc']
    for domain in mist_domains:
        if domain in u:
            return True
    
    # MistServer protocol paths: /webrtc/, /hls/, /cmaf/, /dash/
    mist_protocols = ['/webrtc/', '/hls/', '/cmaf/', '/dash/', '/rtmp/']
    for proto in mist_protocols:
        if proto in u:
            return True
    # MistServer player page: stream.html or /stream-name.html
    if '.html' in u and ('stream.' in u or '/obs-' in u or '-whip' in u):
        return True
    return False

def _direct_mist_info(url: str) -> dict:
    """Build metadata for a MistServer stream URL."""
    parsed = urlparse(url)
    # Extract stream name from URL path
    path_parts = parsed.path.strip('/').split('/')
    
    # Handle formats like /webrtc/obs-whip or /cmaf/obs-whip/...
    stream_name = "Live Stream"
    if len(path_parts) >= 2:
        stream_name = path_parts[1]  # Second part is usually stream name
    elif len(path_parts) == 1:
        # Handle /obs-whip.html format
        stream_name = path_parts[0].replace('.html', '')
    
    import hashlib
    url_id = hashlib.sha1(url.encode("utf-8")).hexdigest()
    return {
        "id": f"mist_{url_id}",
        "title": stream_name,
        "duration": 0,  # Live stream
        "thumbnail": "",
        "ext": "mist",
        "source": "mist",
    }

def _direct_hls_info(url: str) -> dict:
    parsed = urlparse(url)
    basename = os.path.basename(parsed.path or "")
    title = unquote(basename) if basename else (parsed.netloc or "HLS stream")
    # Stable ID for queue/state and to avoid duplicates when re-entering the same URL.
    import hashlib
    url_id = hashlib.sha1(url.encode("utf-8")).hexdigest()
    return {
        "id": f"hls_{url_id}",
        "title": title,
        "duration": 0,
        "thumbnail": "",
        "ext": "m3u8",
        "source": "hls",
    }

def _resolve_downloaded_path(video_id: str, prepared_filename: str) -> str:
    if prepared_filename and os.path.exists(prepared_filename):
        return prepared_filename
    candidates: List[str] = []
    candidates.extend(glob.glob(os.path.join(YOUTUBE_DIR, f"{video_id}.*")))
    candidates.extend(glob.glob(os.path.join(DOWNLOAD_DIR, f"{video_id}.*")))
    if not candidates:
        return prepared_filename
    candidates.sort(key=lambda p: os.path.getmtime(p), reverse=True)
    return candidates[0]

def _file_url_for_local_path(local_path: str) -> tuple[str, Optional[str]]:
    p = Path(local_path).resolve()
    local_root = Path(LOCAL_DIR).resolve()
    download_root = Path(DOWNLOAD_DIR).resolve()
    if local_root in p.parents:
        rel = p.relative_to(local_root).as_posix()
        return f"/local/{rel}", rel
    if download_root in p.parents:
        rel = p.relative_to(download_root).as_posix()
        return f"/files/{rel}", None
    raise RuntimeError("Downloaded file is outside allowed roots")

def _write_sidecar_info(video_file_path: str, info: dict):
    try:
        sidecar = Path(f"{video_file_path}.info.json")
        payload = {
            "id": info.get("id"),
            "title": info.get("title"),
            "duration": info.get("duration"),
            "thumbnail": info.get("thumbnail"),
            "ext": info.get("ext"),
            "source": info.get("source"),
            "path": info.get("path"), 
        }
        sidecar.write_text(json.dumps(payload, ensure_ascii=False), encoding="utf-8")
    except Exception as exc:
        logger.warning(f"Failed to write sidecar metadata: {exc}")

def build_entry(video_url: str, info: dict, *, file_url: Optional[str] = None, hls_url: Optional[str] = None, stream: Optional[str] = None):
    return {
        "id": info.get("id") or str(uuid.uuid4()),
        "url": video_url,
        "file_url": file_url,
        "hls_url": hls_url,
        "stream": stream,
        "path": info.get("path"),
        "source": info.get("source"),
        "title": info.get("title", "Unknown"),
        "duration": info.get("duration", 0),
        "thumbnail": info.get("thumbnail", ""),
        "ext": info.get("ext"),
    }

from .cache import cache_manager

def download_video(youtube_url: str, stream: str = "file"):
    """
    Download a URL (typically YouTube) and return a playable URL plus metadata.
    If the input looks like a direct HLS manifest (.m3u8), skip downloading and
    return it directly for client-side playback.
    MistServer live stream URLs are also passed through directly.
    """
    # Handle direct HLS manifests
    if stream == "hls" and _looks_like_hls_manifest(youtube_url):
        info = _direct_hls_info(youtube_url)
        # Return original URL directly - no proxy needed for browser access
        # The proxy was causing routing through distant Cloudflare edges
        return youtube_url, None, youtube_url, info
    
    # Handle MistServer live streams (webrtc, cmaf, etc.)
    if _looks_like_mist_stream(youtube_url):
        info = _direct_mist_info(youtube_url)
        logger.info(f"[MistServer] Passing through live stream: {youtube_url}")
        return youtube_url, None, youtube_url, info

    def ydl_progress_hook(d):
        if d['status'] == 'downloading':
            logger.info(f"[yt-dlp] Downloading: {d.get('_percent_str', '?')} of {d.get('_total_bytes_str', '?')} at {d.get('_speed_str', '?')}")
        elif d['status'] == 'finished':
            logger.info(f"[yt-dlp] Download finished: {d.get('filename', '?')}")

    ydl_opts = {
        # Prefer H.264 (avc1) MP4 up to 1080p for widest client compatibility; fall back as needed.
        "format": "bestvideo[vcodec^=avc1][ext=mp4][height<=1080]+bestaudio[ext=m4a]/best[ext=mp4][height<=1080]/best[height<=1080]",
        # Store in the same library tree as local files so the client can list/replay it.
        "outtmpl": f"{YOUTUBE_DIR}/%(id)s.%(ext)s",
        "quiet": False,
        "verbose": True,
        "no_warnings": False,
        "noplaylist": True,
        "merge_output_format": "mp4",
        "progress_hooks": [ydl_progress_hook],
    }

    with yt_dlp.YoutubeDL(ydl_opts) as ydl:
        info = ydl.extract_info(youtube_url, download=True)
        video_id = info.get("id") or str(uuid.uuid4())
        prepared_filename = ydl.prepare_filename(info)
        local_path = _resolve_downloaded_path(video_id, prepared_filename)
        file_url, local_relpath = _file_url_for_local_path(local_path)

        # Register with Cache Manager (LRU)
        cache_manager.register_access(video_id)

        # Persist minimal metadata for library listing UI
        info["source"] = "youtube"
        if not info.get("duration"):
            dur = _probe_duration_seconds(local_path)
            if dur:
                info["duration"] = dur
        if local_relpath is not None:
            info["path"] = local_relpath
        _write_sidecar_info(local_path, info)

        hls_url = None
        video_url = file_url
        if stream == "hls":
            hls_url = _ensure_hls(video_id, local_path)
            # Re-register access after HLS gen if we consider it "active" work? 
            # Not strictly needed if ID is same.
            video_url = hls_url

        return video_url, file_url, hls_url, info


def enqueue_video(state: GlobalState, youtube_url: str, stream: str = "file"):
    video_url, file_url, hls_url, info = download_video(youtube_url, stream=stream)
    entry = build_entry(video_url, info, file_url=file_url, hls_url=hls_url, stream=stream)
    state.queue.append(entry)
    return entry
