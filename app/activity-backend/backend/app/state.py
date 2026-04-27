from typing import Optional, List, Dict
import threading
import time

# Default stream URL when queue is empty (DASH MPD via Discord URL Mapping)
# Requires Discord Developer Portal URL Mapping: /stream → stream.mintcocoa.cc
DEFAULT_STREAM_URL = "/stream/cmaf/obs-whip/index.mpd"

class GlobalState:
    def __init__(self):
        # Initialize with no VOD playing (live stream is handled separately by frontend)
        self.current_video_url: Optional[str] = None
        self.metadata: dict = {}
        self.is_playing: bool = False
        self.current_time: float = 0.0
        self.last_updated: float = 0.0  # time.time()
        
        # Queue: list of dicts (the 'entry' dicts)
        self.queue: List[dict] = []

        # Start timestamp for current session (to align with clients).
        # We broadcast this + current_video_url + is_playing.
        self.start_at: Optional[float] = None
        
        # To handle pause/resume accurately, distinct from 'last_updated'
        self.pause_timestamp: Optional[float] = None

        # Controller / Intent control (debouncing / locking)
        self.controller_client_id: Optional[str] = None
        self.controller_acquired_at: Optional[float] = None
        self.controller_last_intent_at: Optional[float] = None

        # Live HLS sync pause anchor (program-date-time in epoch ms)
        self.hls_pause_pdt_ms: Optional[int] = None

        # AI Chat history: [{id, type, text, user: {id, avatar}, timestamp}]
        self.ai_chat_history: List[dict] = []

# Instance Management
_instances: Dict[str, GlobalState] = {}
_instances_lock = threading.Lock()

def get_state(instance_id: str) -> GlobalState:
    with _instances_lock:
        if instance_id not in _instances:
            _instances[instance_id] = GlobalState()
        return _instances[instance_id]
