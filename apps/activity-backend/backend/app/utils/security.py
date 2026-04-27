import os
import logging
from fastapi import Request, HTTPException
from fastapi.responses import JSONResponse

logger = logging.getLogger(__name__)

def _env_truthy(name: str) -> bool:
    return (os.getenv(name) or "").strip().lower() in {"1", "true", "yes", "on"}

async def access_guard_middleware(request: Request, call_next):
    if _env_truthy("COCOATUBE_ALLOW_EXTERNAL_BROWSER"):
        return await call_next(request)

    # 1. Discord Proxy Request? (Check Signature header)
    if "x-signature-ed25519" in request.headers:
         return await call_next(request)
    
    # 2. Discord Activity Proxy (discordsays.com)
    # Check Referer or Origin for Discord's proxy domain
    referer = request.headers.get("referer", "")
    origin = request.headers.get("origin", "")
    if "discordsays.com" in referer or "discordsays.com" in origin:
        return await call_next(request)
    
    # 3. Discord proxy ticket present (embedded app SDK uses this)
    # The URL might contain discord_proxy_ticket
    if "discord_proxy_ticket" in str(request.url):
        return await call_next(request)
    
    # 4. Localhost Request?
    client_host = request.client.host
    if client_host in ("127.0.0.1", "::1", "localhost"):
        return await call_next(request)
    
    # 5. Docker internal network (nginx -> backend)
    # Docker internal IPs typically start with 172.
    if client_host and client_host.startswith("172."):
        return await call_next(request)
        
    # Block external browser access
    return JSONResponse(status_code=403, content={"detail": "Access denied. Only accessible via Discord Activity or Localhost."})


async def validate_activity_instance(instance_id: str) -> bool:
    # Allow "default" for localhost testing
    if instance_id == "default":
        return True
        
    bot_token = os.getenv("DISCORD_BOT_TOKEN")
    app_id = os.getenv("DISCORD_CLIENT_ID")
    if not bot_token or not app_id:
        # Fail safe: if no tokens are configured, we can't validate.
        # For dev environment without tokens, this warns.
        logger.warning("Missing DISCORD_BOT_TOKEN or DISCORD_CLIENT_ID. Skipping instance validation (INSECURE).")
        return True 
    
    # TODO: Implement actual validation via Discord API using bot_token
    # For now, trust the instances if tokens are present
    return True

async def ensure_authorized(request: Request, instance_id: str | None = None):
    if _env_truthy("COCOATUBE_ALLOW_EXTERNAL_BROWSER"):
        return

    # 1. Check Localhost
    host = request.client.host
    if host in ["127.0.0.1", "localhost", "::1"]:
        return # Authorized
        
    # 2. Check Instance ID
    if not instance_id:
         raise HTTPException(status_code=403, detail="Access denied: No instance_id provided")
         
    is_valid = await validate_activity_instance(instance_id)
    if not is_valid:
        raise HTTPException(status_code=403, detail="Access denied: Invalid instance_id")
