import time
import logging
from typing import Dict, List, Optional
from uuid import uuid4
from fastapi import WebSocket

logger = logging.getLogger(__name__)

class ConnectionManager:
    def __init__(self):
        # instance_id -> list of websockets
        self.active_connections: Dict[str, List[WebSocket]] = {}
        # key: id(websocket) -> session info
        self.sessions: Dict[int, dict] = {}

    async def connect(self, websocket: WebSocket):
        await websocket.accept()
        # Initial connect puts it in no-instance limbo
        self.sessions[id(websocket)] = {
            "session_id": str(uuid4()),
            "client_id": None,
            "joined_at": time.time(),
            "instance_id": None
        }

    def register_instance(self, websocket: WebSocket, instance_id: str, client_id: str, payload: Optional[dict] = None):
        """Binds a connected websocket to an instance."""
        sid = id(websocket)
        if sid in self.sessions:
            self.sessions[sid]["instance_id"] = instance_id
            self.sessions[sid]["client_id"] = client_id
            # Add to active connections for this instance
            if instance_id not in self.active_connections:
                self.active_connections[instance_id] = []
            if websocket not in self.active_connections[instance_id]:
                self.active_connections[instance_id].append(websocket)
        return self.sessions.get(sid)

    def disconnect(self, websocket: WebSocket) -> Optional[dict]:
        sid = id(websocket)
        session = self.sessions.pop(sid, None)
        if session:
            instance_id = session.get("instance_id")
            if instance_id and instance_id in self.active_connections:
                if websocket in self.active_connections[instance_id]:
                    self.active_connections[instance_id].remove(websocket)
        return session

    def register(self, websocket: WebSocket, client_id: str, meta: Optional[dict] = None) -> dict:
        now_ts = time.time()
        info = self.sessions.get(id(websocket))
        if info is None:
            info = {
                "session_id": str(uuid4()),
                "client_id": client_id,
                "joined_at": now_ts,
                "app": None,
            }
        else:
            info["client_id"] = client_id

        if isinstance(meta, dict):
            info["app"] = meta.get("app") or info.get("app")
        self.sessions[id(websocket)] = info
        return info

    def list_clients(self) -> list[dict]:
        clients = list(self.sessions.values())
        clients.sort(key=lambda x: x.get("joined_at", 0))
        return [
            {
                "session_id": c.get("session_id"),
                "client_id": c.get("client_id"),
                "joined_at": c.get("joined_at"),
                "app": c.get("app"),
            }
            for c in clients
        ]

    async def broadcast(self, message: dict, instance_id: Optional[str] = None):
        payload = message.get("payload")
        if message.get("type") != "PRESENCE_UPDATE" and isinstance(payload, dict) and "server_now" not in payload:
            payload["server_now"] = time.time()

        if instance_id:
            # Broadcast to specific instance
            if instance_id in self.active_connections:
                for connection in self.active_connections[instance_id]:
                    try:
                        await connection.send_json(message)
                    except Exception as exc:
                        logger.error(f"Error broadcasting to {instance_id}: {exc}")
        else:
            # Broadcast to all instances
            for iid, connections in self.active_connections.items():
                for connection in connections:
                    try:
                        await connection.send_json(message)
                    except Exception as exc:
                        logger.error(f"Error broadcasting to all: {exc}")

manager = ConnectionManager()

async def broadcast_presence(instance_id: str, event: Optional[dict] = None):
    await manager.broadcast({
        "type": "PRESENCE_UPDATE",
        "payload": {
            "event": event,
            "clients": manager.list_clients(),
            "client_count": len(manager.sessions),
        }
    }, instance_id=instance_id)

async def _next_state_seq() -> int:
    return int(time.time() * 1000) # Simple seq based on time or random
    # In main.py it was a global counter.
    # We can use a simple approximate or move the global counter here.
    
_state_seq = 0
def get_next_state_seq() -> int:
    global _state_seq
    _state_seq += 1
    return _state_seq
