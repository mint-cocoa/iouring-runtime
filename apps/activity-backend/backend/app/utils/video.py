import os
import json
import time
import logging
import subprocess
import glob
import hashlib
from pathlib import Path
from typing import Optional

from ..config import HLS_DIR, HLS_SEGMENT_SECONDS, LOCAL_DIR, DOWNLOAD_DIR
from ..state import _instances, _instances_lock

logger = logging.getLogger(__name__)

# HLS generation locks (avoid concurrent ffmpeg on same asset)
import threading
_hls_locks: dict[str, threading.Lock] = {}
_hls_locks_guard = threading.Lock()

def _get_hls_lock(video_id: str) -> threading.Lock:
    with _hls_locks_guard:
        lock = _hls_locks.get(video_id)
        if lock is None:
            lock = threading.Lock()
            _hls_locks[video_id] = lock
        return lock


def _ensure_hls(video_id: str, input_path: str) -> str:
    out_dir = os.path.join(HLS_DIR, video_id)
    playlist_path = os.path.join(out_dir, "index.m3u8")
    os.makedirs(out_dir, exist_ok=True)

    lock = _get_hls_lock(video_id)
    with lock:
        meta_path = os.path.join(out_dir, "meta.json")

        def run_ffmpeg(args: list[str]) -> subprocess.CompletedProcess[str]:
            """Run ffmpeg with progress logging."""
            logger.info(f"[ffmpeg] Starting HLS transcoding for {video_id}")
            start_time = time.time()
            
            # Run with progress output
            proc = subprocess.Popen(
                args,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            
            # Read stderr for progress (ffmpeg outputs to stderr)
            stderr_output = []
            for line in iter(proc.stderr.readline, ''):
                stderr_output.append(line)
                # Log progress lines (frame=, time=, speed=)
                if 'frame=' in line or 'time=' in line:
                    # Parse and log progress
                    stripped = line.strip()
                    if stripped:
                        logger.info(f"[ffmpeg] {stripped[:100]}")
                elif line.strip():
                    # Log other meaningful lines
                    logger.debug(f"[ffmpeg] {line.strip()}")
            
            proc.wait()
            elapsed = time.time() - start_time
            logger.info(f"[ffmpeg] HLS transcoding completed in {elapsed:.1f}s (exit code: {proc.returncode})")
            
            return subprocess.CompletedProcess(
                args=args,
                returncode=proc.returncode,
                stdout='',
                stderr=''.join(stderr_output)
            )

        def probe_codecs(path: str) -> dict:
            proc = subprocess.run(
                ["ffprobe", "-v", "error", "-show_streams", "-of", "json", path],
                capture_output=True,
                text=True,
            )
            if proc.returncode != 0:
                raise RuntimeError(proc.stderr.strip() or "ffprobe failed")
            payload = json.loads(proc.stdout)
            streams = payload.get("streams") or []
            v = next((s for s in streams if s.get("codec_type") == "video"), None) or {}
            a = next((s for s in streams if s.get("codec_type") == "audio"), None) or {}
            return {
                "video_codec": v.get("codec_name"),
                "video_pix_fmt": v.get("pix_fmt"),
                "audio_codec": a.get("codec_name"),
            }

        def choose_mode(codecs: dict) -> str:
            # Try copy mode first for H.264 videos (10x faster)
            # Falls back to transcode if copy fails or for non-H.264 sources
            video_codec = codecs.get("video_codec", "").lower()
            if video_codec in ("h264", "avc", "avc1"):
                logger.info(f"[HLS] Trying copy mode for H.264 video")
                return "copy"
            else:
                logger.info(f"[HLS] Using transcode mode for non-H.264 video ({video_codec})")
                return "transcode"

        def rm_files(dir_path: str):
            for p in glob.glob(os.path.join(dir_path, "*")):
                try:
                    if os.path.isdir(p):
                        continue
                    os.remove(p)
                except Exception:
                    pass

        codecs = probe_codecs(input_path)
        desired_mode = choose_mode(codecs)
        input_stat = os.stat(input_path)

        if os.path.exists(playlist_path) and os.path.exists(meta_path):
            try:
                meta = json.loads(Path(meta_path).read_text(encoding="utf-8"))
            except Exception:
                meta = {}
            if (
                meta.get("mode") == desired_mode
                and meta.get("input_mtime") == input_stat.st_mtime
                and meta.get("input_size") == input_stat.st_size
                and meta.get("hls_segment_seconds") == HLS_SEGMENT_SECONDS
            ):
                return f"/hls/{video_id}/index.m3u8"

        if os.path.exists(playlist_path):
            rm_files(out_dir)

        # Use fMP4 (CMAF) segments instead of MPEG2-TS for Shaka Player compatibility
        segment_pattern = os.path.join(out_dir, "seg_%05d.m4s")
        init_segment = os.path.join(out_dir, "init.mp4")

        common_prefix = [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "info",  # Changed from error to info for progress output
            "-progress", "pipe:2",  # Output progress to stderr
            "-y",
            "-i",
            input_path,
            "-map",
            "0:v:0",
            "-map",
            "0:a:0?",
        ]
        hls_flags = "independent_segments"
        # Include EXT-X-PROGRAM-DATE-TIME tags so clients can align playback via PDT.
        hls_flags = "program_date_time+" + hls_flags
        common_suffix = [
            "-hls_time",
            str(HLS_SEGMENT_SECONDS),
            "-hls_list_size",
            "0",
            "-hls_flags",
            hls_flags,
            # Use fMP4 (CMAF) instead of MPEG2-TS - avoids Shaka transmuxing issues
            "-hls_segment_type",
            "fmp4",
            "-hls_fmp4_init_filename",
            "init.mp4",
            "-hls_segment_filename",
            segment_pattern,
            "-f",
            "hls",
            playlist_path,
        ]

        if desired_mode == "copy":
            # Copy mode: Copy video stream (fast) but transcode audio to apply volume reduction.
            # volume=0.6 reduces audio to 60% of original.
            # For fMP4, we don't need h264_mp4toannexb bitstream filter
            proc = run_ffmpeg(
                common_prefix 
                + [
                    "-c:v", "copy",
                    "-c:a", "aac", 
                    "-b:a", "128k", 
                    "-af", "volume=0.6"
                ] 
                + common_suffix
            )
            if proc.returncode != 0:
                logger.warning(f"[HLS] Copy mode failed (ffmpeg error), falling back to transcode")
                rm_files(out_dir)
                desired_mode = "transcode"
            else:
                logger.info(f"[HLS] Copy mode succeeded")

        if desired_mode == "transcode":
            # GOP 설정: 30fps 기준 6초 세그먼트에 맞춰 180프레임
            gop_frames = str(int(30 * HLS_SEGMENT_SECONDS))
            proc2 = run_ffmpeg(
                common_prefix
                + [
                    "-c:v",
                    "libx264",
                    "-preset",
                    "ultrafast",  # 최대 속도
                    "-crf",
                    "23",
                    # Use main profile for fMP4 compatibility
                    "-profile:v",
                    "main",
                    "-level:v",
                    "4.0",
                    "-pix_fmt",
                    "yuv420p",
                    "-g",
                    gop_frames,
                    "-keyint_min",
                    gop_frames,
                    "-sc_threshold",
                    "0",
                    "-force_key_frames",
                    f"expr:gte(t,n_forced*{HLS_SEGMENT_SECONDS})",
                    # fMP4 specific: use movflags for proper fragmentation
                    "-movflags",
                    "+frag_keyframe+empty_moov+default_base_moof",
                    "-c:a",
                    "aac",
                    "-b:a",
                    "128k",
                    "-af",
                    "volume=0.6",
                    "-ac",
                    "2",
                    "-ar",
                    "48000",
                ]
                + common_suffix
            )
            if proc2.returncode != 0:
                raise RuntimeError(f"ffmpeg HLS conversion failed: {proc2.stderr}")

        Path(meta_path).write_text(
            json.dumps(
                {
                    "mode": desired_mode,
                    "codecs": codecs,
                    "input_mtime": input_stat.st_mtime,
                    "input_size": input_stat.st_size,
                    "hls_segment_seconds": HLS_SEGMENT_SECONDS,
                },
                ensure_ascii=False,
            ),
            encoding="utf-8",
        )

    return f"/hls/{video_id}/index.m3u8"

def _probe_duration_seconds(path: str) -> float:
    try:
        proc = subprocess.run(
            ["ffprobe", "-v", "error", "-show_format", "-of", "json", path],
            capture_output=True,
            text=True,
        )
        if proc.returncode != 0:
            return 0.0
        payload = json.loads(proc.stdout or "{}")
        fmt = payload.get("format") or {}
        dur = fmt.get("duration")
        if dur is None:
            return 0.0
        dur_f = float(dur)
        return dur_f if dur_f > 0 else 0.0
    except Exception:
        return 0.0
