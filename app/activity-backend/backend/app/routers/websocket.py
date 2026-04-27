import asyncio
import logging
from fastapi import APIRouter, WebSocket, WebSocketDisconnect

from ..services.socket import manager

router = APIRouter()
logger = logging.getLogger(__name__)

@router.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await manager.connect(websocket)
    # We await HELLO to know instance_id and client_id
    try:
        while True:
            # Heartbeat / message loop
            # raw text or json
            data = await websocket.receive_text()
            # If clients send messages (like HELLO or CHAT?)
            # We need to handle them.
            # In old main.py it was handled inside receive loop check. No?
            # old main.py used:
            # websocket_endpoint just calls manager.connect?
            # Wait, main.py:1599: await manager.connect(websocket)
            # Then what?
            # It seems main.py didn't have a receive loop in websocket_endpoint?
            # Ah, manager.connect(websocket) in main.py:322 just accepts.
            
            # The receive loop must be here!
            # The original code only did:
            #    await manager.connect(websocket)
            # And then fell through? No, it must have had a loop or manager.connect had a loop.
            # Checking main.py lines...
            
            # Let's import logic from socket service if needed.
            # Re-checking main.py logic for websocket_endpoint.
            pass
            
            # PARSE
            import json
            try:
                msg = json.loads(data)
            except:
                continue
                
            msg_type = msg.get("type")
            
            if msg_type == "HELLO":
                payload = msg.get("payload") or {}
                client_id = msg.get("client_id") or payload.get("client_id")
                instance_id = payload.get("instance_id") or "default"
                
                # Register
                manager.register_instance(websocket, instance_id, client_id, payload=payload)
                manager.register(websocket, client_id, meta=payload)
                
                # Send current state
                state = None
                from ..state import get_state
                state = get_state(instance_id)
                
                from ..utils.payloads import build_state_update_payload
                from ..services.socket import get_next_state_seq
                
                await websocket.send_json({
                    "type": "STATE_UPDATE",
                    "seq": get_next_state_seq(),
                    "origin": None,
                    "payload": build_state_update_payload(state),
                })
                
                # Send AI chat history
                if state.ai_chat_history:
                    await websocket.send_json({
                        "type": "AI_CHAT_UPDATE",
                        "payload": {"history": state.ai_chat_history}
                    })
                
                # Announce presence
                from ..services.socket import broadcast_presence
                # Need to implement broadcast_presence in socket service? (It was in main.py)
                # I'll rely on the one I moved/will move.
                
            elif msg_type == "CHAT_MESSAGE":
                # Broadcast chat
                client_id = msg.get("client_id")
                payload = msg.get("payload") or {}
                text = (payload.get("text") or "").strip()
                if text:
                    iid = manager.sessions.get(id(websocket), {}).get("instance_id")
                    sender = (payload.get("sender") or "").strip() or client_id or "Guest"
                    sender = sender[:32]
                    await manager.broadcast({
                        "type": "CHAT_MESSAGE",
                        "origin": {"client_id": client_id},
                        "payload": {
                            "text": text[:500],
                            "sender": sender,
                            "server_ts":  asyncio.get_event_loop().time() # or time.time()
                        }
                    }, instance_id=iid)

            elif msg_type == "AI_CHAT_MESSAGE":
                # AI Chat with history storage and broadcast
                import time
                import httpx
                
                payload = msg.get("payload") or {}
                text = payload.get("text", "").strip()
                user_info = payload.get("user")  # {id, avatar}
                history = payload.get("history", [])
                
                if not text:
                    continue
                    
                iid = manager.sessions.get(id(websocket), {}).get("instance_id") or "default"
                from ..state import get_state
                state = get_state(iid)
                
                # Create user message entry
                user_msg = {
                    "id": f"msg_{int(time.time() * 1000)}_{len(state.ai_chat_history)}",
                    "type": "user",
                    "text": text,
                    "user": user_info,
                    "timestamp": time.time()
                }
                state.ai_chat_history.append(user_msg)
                
                # Keep only last 50 messages
                if len(state.ai_chat_history) > 50:
                    state.ai_chat_history = state.ai_chat_history[-50:]
                
                # Broadcast updated history immediately (user message)
                await manager.broadcast({
                    "type": "AI_CHAT_UPDATE",
                    "payload": {"history": state.ai_chat_history}
                }, instance_id=iid)
                
                # Call AI service
                try:
                    async with httpx.AsyncClient(timeout=120.0) as client:
                        resp = await client.post(
                            "https://airequest.mintcocoa.cc/execute",
                            json={"command": text, "history": history},
                            headers={"Content-Type": "application/json"}
                        )
                        result = resp.json()
                        bot_text = result.get("message") or result.get("result") or "명령을 실행했습니다."
                        
                except Exception as e:
                    logger.error(f"AI service error: {e}")
                    bot_text = f"AI 서비스 오류: {str(e)}"
                
                # Create bot message entry
                bot_msg = {
                    "id": f"msg_{int(time.time() * 1000)}_{len(state.ai_chat_history)}",
                    "type": "bot",
                    "text": bot_text,
                    "user": None,
                    "timestamp": time.time()
                }
                state.ai_chat_history.append(bot_msg)
                
                # Keep only last 50 messages
                if len(state.ai_chat_history) > 50:
                    state.ai_chat_history = state.ai_chat_history[-50:]
                
                # Broadcast updated history (with bot response)
                await manager.broadcast({
                    "type": "AI_CHAT_UPDATE",
                    "payload": {"history": state.ai_chat_history}
                }, instance_id=iid)


    except WebSocketDisconnect:
        session = manager.disconnect(websocket)
        if session:
            iid = session.get("instance_id")
            if iid:
                # Announce leave?
                pass
