import os

# Control policy (who can send INTENT_* that affects global playback)
# - open: any client can control (current default behavior)
# - first: first client to send an INTENT_* becomes controller; others are denied until controller disconnects or TTL expires
CONTROL_MODE = os.getenv("COCOATUBE_CONTROL_MODE", "open").strip().lower()
try:
    CONTROL_TTL_SECONDS = float(os.getenv("COCOATUBE_CONTROL_TTL_SECONDS", "0") or "0")
except Exception:
    CONTROL_TTL_SECONDS = 0.0

# HLS segment configuration
try:
    HLS_SEGMENT_SECONDS = float(os.getenv("COCOATUBE_HLS_SEGMENT_SECONDS", "6") or "6")
except Exception:
    HLS_SEGMENT_SECONDS = 6.0

# Live HLS sync: target program-time latency (seconds) behind wall-clock.
try:
    HLS_TARGET_LATENCY_SECONDS = float(os.getenv("COCOATUBE_HLS_TARGET_LATENCY_SECONDS", "6") or "6")
except Exception:
    HLS_TARGET_LATENCY_SECONDS = 6.0
HLS_TARGET_LATENCY_SECONDS = max(0.0, HLS_TARGET_LATENCY_SECONDS)

# Directories
DOWNLOAD_DIR = "downloads"
os.makedirs(DOWNLOAD_DIR, exist_ok=True)
HLS_DIR = os.path.join(DOWNLOAD_DIR, "hls")
os.makedirs(HLS_DIR, exist_ok=True)
LOCAL_DIR = os.path.join(DOWNLOAD_DIR, "local")
os.makedirs(LOCAL_DIR, exist_ok=True)
YOUTUBE_DIR = os.path.join(LOCAL_DIR, "youtube")
os.makedirs(YOUTUBE_DIR, exist_ok=True)
UPLOAD_DIR = os.path.join(LOCAL_DIR, "uploads")
os.makedirs(UPLOAD_DIR, exist_ok=True)

try:
    MAX_UPLOAD_MB = float(os.getenv("COCOATUBE_MAX_UPLOAD_MB", "2048") or "2048")
except Exception:
    MAX_UPLOAD_MB = 2048.0
MAX_UPLOAD_BYTES = int(MAX_UPLOAD_MB * 1024 * 1024) if MAX_UPLOAD_MB and MAX_UPLOAD_MB > 0 else 0

try:
    CACHE_MAX_ITEMS = int(os.getenv("COCOATUBE_CACHE_MAX_ITEMS", "5") or "5")
except:
    CACHE_MAX_ITEMS = 5

DISCORD_API_BASE = "https://discord.com/api/v10"
