import os
import shutil
import logging
import threading
import time
from typing import List, Optional
from pathlib import Path

from ..config import CACHE_MAX_ITEMS, YOUTUBE_DIR, HLS_DIR
from ..dependencies import _cleanup_youtube_assets

logger = logging.getLogger(__name__)

class CacheManager:
    def __init__(self, max_items: int = CACHE_MAX_ITEMS):
        self.max_items = max_items
        # Tracking list of video_ids in order of usage (most recent at end)
        self._lru_list: List[str] = []
        self._lock = threading.Lock()
        
    def register_access(self, video_id: str):
        """Mark a video as recently used/downloaded."""
        with self._lock:
            if video_id in self._lru_list:
                self._lru_list.remove(video_id)
            self._lru_list.append(video_id)
            self._evict_if_needed()
            
    def _evict_if_needed(self):
        while len(self._lru_list) > self.max_items:
            # Evict oldest (first in list)
            oldest_id = self._lru_list.pop(0)
            logger.info(f"[Cache] Evicting old video: {oldest_id}")
            # Use shared cleanup util
            _cleanup_youtube_assets({"id": oldest_id})
            
    def clear_all(self):
        """Wipe entire cache (useful on startup if we wanted to enforce start-clean)."""
        with self._lock:
            self._lru_list.clear()
            # Delete all files in YOUTUBE_DIR and HLS_DIR?
            # For now, rely on container restart to clean files, 
            # or manual logic here if needed.
            pass

cache_manager = CacheManager()
