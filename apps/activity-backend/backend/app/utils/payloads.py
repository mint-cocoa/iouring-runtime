import time
from typing import Optional

from ..state import GlobalState
from ..services.socket import manager
from ..dependencies import get_effective_time, _is_hls_now, _hls_target_pdt_ms
from ..config import CONTROL_MODE, CONTROL_TTL_SECONDS, HLS_TARGET_LATENCY_SECONDS

def _control_info(state: GlobalState) -> dict:
    return {
        "mode": CONTROL_MODE,
        "ttl_seconds": CONTROL_TTL_SECONDS,
        "controller_client_id": state.controller_client_id,
        "controller_acquired_at": state.controller_acquired_at,
        "controller_last_intent_at": state.controller_last_intent_at,
    }

def build_state_update_payload(state: GlobalState) -> dict:
    server_now = time.time()
    effective_time = get_effective_time(state)
    paused_time = state.current_time if not state.is_playing else None

    playback = {
        "is_playing": state.is_playing,
        "start_at": state.start_at,
        "paused_time": paused_time,
        "time": effective_time,
    }
    if _is_hls_now(state):
        playback["hls"] = {
            "mode": "wall_clock_latency",
            "target_latency_seconds": HLS_TARGET_LATENCY_SECONDS,
            "pdt_ms": _hls_target_pdt_ms(state, server_now),
        }

    return {
        "server_now": server_now,
        "media": {
            "url": state.current_video_url,
            **(state.metadata or {}),
        },
        "playback": playback,
        "control": _control_info(state),
        "clients": manager.list_clients(),
        "client_count": len(manager.sessions),
        "queue": state.queue,
    }
