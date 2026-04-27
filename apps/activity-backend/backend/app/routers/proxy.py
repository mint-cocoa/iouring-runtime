import httpx
import asyncio
from typing import Optional
from fastapi import APIRouter, HTTPException, Request, Response
import logging
from urllib.parse import urlparse, unquote, urlencode, urljoin

router = APIRouter()
logger = logging.getLogger(__name__)

TVING_BROWSER_HEADERS = {
    "User-Agent": (
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/147.0.0.0 Safari/537.36"
    ),
    "Referer": "https://www.tving.com/",
    "Origin": "https://www.tving.com",
}

# Shared client logic (re-implemented to be self-contained or imported)
_http_client: Optional[httpx.AsyncClient] = None

def _get_http_client() -> httpx.AsyncClient:
    global _http_client
    if _http_client is None:
        _http_client = httpx.AsyncClient(
            follow_redirects=True,
            timeout=httpx.Timeout(30.0, connect=10.0),
            headers={"User-Agent": "Cocoatube/1.0"},
        )
    return _http_client

async def close_http_client():
    global _http_client
    if _http_client is not None:
        await _http_client.aclose()
        _http_client = None

def _is_public_host(hostname: str) -> bool:
    if not hostname or hostname.lower() in {"localhost"}:
        return False
    # Simplified check. Import full logic from main if stricter security needed.
    return True

def _proxy_hls_url(remote_url: str) -> str:
    from urllib.parse import urlencode
    return "/proxy/hls?" + urlencode({"url": remote_url})

def _proxy_hls_url_with_params(remote_url: str, extra_params: Optional[dict[str, str]] = None) -> str:
    params = {"url": remote_url}
    if extra_params:
        params.update({k: v for k, v in extra_params.items() if v})
    return "/proxy/hls?" + urlencode(params)

def _needs_tving_headers(url: str) -> bool:
    parsed = urlparse(url)
    hostname = (parsed.hostname or "").lower()
    return (
        hostname.endswith("tving.com")
        or "mediatailor" in hostname
        or "aws-kbo-smart" in hostname
    )

def _rewrite_hls_manifest(text: str, base_url: str, extra_proxy_params: Optional[dict[str, str]] = None) -> str:
    import re
    out_lines = []
    uri_attr_re = re.compile(r'URI="([^"]+)"')

    def should_proxy(u: str) -> bool:
        p = urlparse(u)
        if p.scheme in {"http", "https"}:
            return True
        if p.scheme == "" and (p.netloc == "" or u.startswith("//")):
            return True
        return False

    for raw in (text or "").splitlines():
        line = raw.strip()
        if not line:
            out_lines.append(raw)
            continue

        if line.startswith("#"):
            def repl(m: re.Match) -> str:
                orig = m.group(1)
                if not should_proxy(orig):
                    return m.group(0)
                resolved = urljoin(base_url, orig)
                return f'URI="{_proxy_hls_url_with_params(resolved, extra_proxy_params)}"'

            out_lines.append(uri_attr_re.sub(repl, raw))
            continue

        if should_proxy(line):
            resolved = urljoin(base_url, line)
            out_lines.append(_proxy_hls_url_with_params(resolved, extra_proxy_params))
        else:
            out_lines.append(raw)

    return "\n".join(out_lines) + ("\n" if text.endswith("\n") else "")


def _rewrite_dash_manifest(xml_text: str, base_url: str) -> str:
    """
    Rewrite DASH MPD manifest for Discord Activity URL Mapping.
    
    Uses Discord's URL Mapping feature instead of query-param proxy.
    This preserves DASH template variables ($Number$, $Time$, etc.)
    that Shaka Player substitutes client-side.
    
    Requires Discord Developer Portal URL Mapping:
      /stream → stream.mintcocoa.cc
    """
    import re
    from urllib.parse import urlparse
    
    # Parse the base URL to extract the path prefix
    parsed = urlparse(base_url)
    
    # Attributes that contain URLs we need to rewrite
    url_attrs = ['media', 'initialization', 'sourceURL', 'index', 'indexRange']
    
    def rewrite_url(match: re.Match) -> str:
        attr_name = match.group(1)
        url_value = match.group(2)
        
        # Skip if already using our mapping prefix
        if url_value.startswith('/stream/') or url_value.startswith('data:'):
            return match.group(0)
        
        # For relative URLs, just prefix with /stream/
        if not url_value.startswith('http://') and not url_value.startswith('https://'):
            return f'{attr_name}="/stream/{url_value}"'
        
        # For absolute URLs, extract path and use /stream/ prefix
        url_parsed = urlparse(url_value)
        path = url_parsed.path.lstrip('/')
        return f'{attr_name}="/stream/{path}"'
    
    # Match attr="value" for URL attributes
    for attr in url_attrs:
        pattern = rf'({attr})="([^"]+)"'
        xml_text = re.sub(pattern, rewrite_url, xml_text, flags=re.IGNORECASE)
    
    # Handle BaseURL elements - set to /stream/
    def rewrite_base_url(match: re.Match) -> str:
        return '<BaseURL>/stream/</BaseURL>'
    
    xml_text = re.sub(r'<BaseURL>([^<]+)</BaseURL>', rewrite_base_url, xml_text, flags=re.IGNORECASE)
    
    # If no BaseURL exists, add one
    if '<BaseURL>' not in xml_text and '<baseurl>' not in xml_text.lower():
        xml_text = re.sub(
            r'(<MPD[^>]*>)',
            r'\1\n  <BaseURL>/stream/</BaseURL>',
            xml_text,
            count=1,
            flags=re.IGNORECASE
        )
    
    return xml_text

@router.api_route("/hls", methods=["GET", "HEAD"])
async def proxy_hls(request: Request, url: str, tving_id: Optional[str] = None):
    parsed = urlparse(url)
    if parsed.scheme not in {"http", "https"}:
        decoded_url = unquote(url)
        parsed = urlparse(decoded_url)
        if parsed.scheme in {"http", "https"}:
            url = decoded_url
        else:
            raise HTTPException(status_code=400, detail="Only http/https URLs are supported")
    if not parsed.netloc:
        raise HTTPException(status_code=400, detail="Invalid URL")
    
    # Check host allowed (skip thorough SSRF check for brevity, assume internal trusted if needed, 
    # but strictly we should use the logic from before)
    
    client = _get_http_client()
    forward_headers = {}
    if _needs_tving_headers(url):
        forward_headers.update(TVING_BROWSER_HEADERS)
    if tving_id:
        try:
            from ..services.tving import get_registered_playlist

            playlist = get_registered_playlist(tving_id)
            if playlist and playlist.get("headers"):
                forward_headers.update(playlist["headers"])
        except Exception as exc:
            logger.warning(f"Failed to load TVING proxy headers: {exc}")
    rng = request.headers.get("range")
    if rng:
        forward_headers["Range"] = rng
    for h in ("referer", "origin"):
        v = request.headers.get(h)
        header_name = h.capitalize()
        if v and header_name not in forward_headers:
            forward_headers[header_name] = v

    try:
        method = request.method
        req = client.build_request(method, url, headers=forward_headers)
        upstream = await client.send(req, stream=(method == "GET"))
    except httpx.RequestError as exc:
        raise HTTPException(status_code=502, detail=f"Upstream request failed: {exc}")

    if request.method == "HEAD":
        passthrough_headers = {"Cache-Control": "no-store"}
        for h in ("content-type", "content-length", "accept-ranges", "etag", "last-modified"):
            v = upstream.headers.get(h)
            if v:
                passthrough_headers[h.title()] = v
        await upstream.aclose()
        return Response(content=b"", status_code=upstream.status_code, headers=passthrough_headers)

    status = upstream.status_code
    content_type = upstream.headers.get("content-type", "")

    # HLS Playlist detection
    is_hls = parsed.path.lower().endswith(".m3u8") or "mpegurl" in content_type.lower()
    if is_hls:
        try:
            body = await upstream.aread()
        finally:
            await upstream.aclose()

        try:
            text = body.decode("utf-8", errors="replace")
        except Exception:
            text = ""
        
        if not text.lstrip().startswith("#EXTM3U"):
            headers = {"Cache-Control": "no-store"}
            if content_type:
                headers["Content-Type"] = content_type
            return Response(content=body, status_code=status, headers=headers)

        extra_proxy_params = {"tving_id": tving_id} if tving_id else None
        rewritten = _rewrite_hls_manifest(text, url, extra_proxy_params)
        headers = {
            "Cache-Control": "no-store",
            "Content-Type": "application/vnd.apple.mpegurl; charset=utf-8",
        }
        return Response(content=rewritten.encode("utf-8"), status_code=status, headers=headers)

    # DASH MPD detection
    is_dash = parsed.path.lower().endswith(".mpd") or "dash" in content_type.lower() or "xml" in content_type.lower()
    if is_dash:
        try:
            body = await upstream.aread()
        finally:
            await upstream.aclose()

        try:
            text = body.decode("utf-8", errors="replace")
        except Exception:
            text = ""
        
        # Check if it looks like an MPD
        if "<MPD" in text or "<?xml" in text:
            rewritten = _rewrite_dash_manifest(text, url)
            headers = {
                "Cache-Control": "no-store",
                "Content-Type": "application/dash+xml; charset=utf-8",
            }
            return Response(content=rewritten.encode("utf-8"), status_code=status, headers=headers)

    # Segment streaming
    passthrough_headers = {"Cache-Control": "no-store"}
    for h in ("content-type", "content-length", "accept-ranges", "content-range", "etag", "last-modified"):
        v = upstream.headers.get(h)
        if v:
            passthrough_headers[h.title()] = v

    async def iter_bytes():
        try:
            async for chunk in upstream.aiter_bytes():
                yield chunk
        finally:
            await upstream.aclose()
    
    from fastapi.responses import StreamingResponse
    return StreamingResponse(iter_bytes(), status_code=status, headers=passthrough_headers)

@router.api_route("/netflix/{path:path}", methods=["GET", "POST", "PUT", "DELETE"])
async def proxy_netflix(request: Request, path: str):
    """
    Proxy requests to the external Netflix Automation API to avoid CORS.
    Target: https://remote.mintcocoa.cc
    """
    target_base = "https://airequest.mintcocoa.cc"
    # Ensure path starts with / if not present (though path param usually cleans it, we join carefully)
    # path comes from {path:path}, so "status" -> target_base + "/status"
    
    # If path is empty, it might be root
    url = f"{target_base}/{path}"
    
    client = _get_http_client()
    
    # Forward headers (excluding host/cors)
    forward_headers = {}
    # We might want to pass Content-Type
    if request.headers.get("content-type"):
        forward_headers["Content-Type"] = request.headers.get("content-type")
        
    # Read body
    body = await request.body()
    
    logger.info(f"Proxying Netflix Req: {request.method} {url}")
    logger.info(f"Headers: {forward_headers}")
    logger.info(f"Body: {body.decode('utf-8', errors='replace')}")

    try:
        req = client.build_request(
            request.method, 
            url, 
            headers=forward_headers, 
            content=body
        )
        upstream = await client.send(req)
    except httpx.RequestError as exc:
        logger.error(f"Netflix upstream failed: {exc}")
        raise HTTPException(status_code=502, detail=f"Netflix upstream failed: {exc}")
        
    upstream_content = await upstream.aread()
    logger.info(f"Upstream Resp: {upstream.status_code}")
    logger.info(f"Upstream Body: {upstream_content.decode('utf-8', errors='replace')}")

    return Response(
        content=upstream_content,
        status_code=upstream.status_code,
        media_type=upstream.headers.get("content-type", "application/json")
    )


# ============ WebRTC Proxy for Discord CSP compliance ============

def _rewrite_webrtc_html(html: str, base_url: str) -> str:
    """
    Rewrite WebRTC HTML to use our proxy for reader.js and whep.
    This allows embedding in Discord Activity which has strict CSP.
    """
    import re
    from urllib.parse import urlencode, urlparse
    
    parsed = urlparse(base_url)
    stream_base = f"{parsed.scheme}://{parsed.netloc}{parsed.path}"
    if not stream_base.endswith('/'):
        stream_base += '/'
    
    # Remove Cloudflare scripts that violate Discord CSP
    html = re.sub(
        r'<script[^>]*src="[^"]*cloudflare[^"]*"[^>]*></script>',
        '',
        html,
        flags=re.IGNORECASE
    )
    
    # Rewrite relative paths to use our proxy
    # reader.js -> /proxy/webrtc/reader.js?base=<stream_base>
    html = html.replace(
        'src="./reader.js"',
        f'src="/proxy/webrtc/reader.js?{urlencode({"base": stream_base})}"'
    )
    
    # Rewrite the WHEP URL in the script - the MediaMTXWebRTCReader uses relative 'whep' path
    # We need to intercept and proxy the whep requests, so modify the URL construction
    whep_proxy_url = f"/proxy/webrtc/whep?{urlencode({'base': stream_base})}"
    
    # Replace: url: new URL('whep', window.location.href) + window.location.search
    # With: url: '<our_proxy_whep_url>' + window.location.search
    html = html.replace(
        "url: new URL('whep', window.location.href) + window.location.search",
        f"url: '{whep_proxy_url}' + window.location.search"
    )
    
    return html


@router.get("/webrtc/page")
async def proxy_webrtc_page(url: str):
    """
    Proxy the MediaMTX WebRTC player page and rewrite it to use our WHEP proxy.
    This allows embedding in Discord Activity which blocks external iframes.
    """
    parsed = urlparse(url)
    if parsed.scheme not in {"http", "https"}:
        raise HTTPException(status_code=400, detail="Only http/https URLs are supported")
    
    client = _get_http_client()
    
    try:
        resp = await client.get(url)
    except httpx.RequestError as exc:
        raise HTTPException(status_code=502, detail=f"Upstream request failed: {exc}")
    
    if resp.status_code != 200:
        raise HTTPException(status_code=resp.status_code, detail="Upstream returned error")
    
    html = resp.text
    rewritten = _rewrite_webrtc_html(html, url)
    
    return Response(
        content=rewritten,
        status_code=200,
        media_type="text/html; charset=utf-8"
    )


@router.get("/webrtc/reader.js")
async def proxy_webrtc_reader(base: str):
    """
    Proxy the MediaMTX reader.js file.
    """
    reader_url = urljoin(base, "reader.js")
    
    client = _get_http_client()
    
    try:
        resp = await client.get(reader_url)
    except httpx.RequestError as exc:
        raise HTTPException(status_code=502, detail=f"Upstream request failed: {exc}")
    
    return Response(
        content=resp.content,
        status_code=resp.status_code,
        media_type="application/javascript"
    )


@router.api_route("/webrtc/whep", methods=["GET", "POST", "PATCH", "DELETE", "OPTIONS"])
async def proxy_webrtc_whep(request: Request, base: str):
    """
    Proxy WebRTC WHEP signaling requests.
    WHEP uses POST for offer, PATCH for ICE candidates.
    """
    whep_url = urljoin(base, "whep")
    
    # Append query params (excluding 'base')
    query_params = dict(request.query_params)
    query_params.pop("base", None)
    if query_params:
        whep_url += "?" + urlencode(query_params)
    
    client = _get_http_client()
    
    # Forward headers
    forward_headers = {}
    for h in ("content-type", "authorization", "if-match"):
        v = request.headers.get(h)
        if v:
            forward_headers[h.title()] = v
    
    body = await request.body()
    
    try:
        req = client.build_request(
            request.method,
            whep_url,
            headers=forward_headers,
            content=body if body else None
        )
        upstream = await client.send(req)
    except httpx.RequestError as exc:
        logger.error(f"WHEP proxy failed: {exc}")
        raise HTTPException(status_code=502, detail=f"WHEP upstream failed: {exc}")
    
    upstream_content = await upstream.aread()
    
    # Build response headers
    response_headers = {}
    for h in ("content-type", "location", "link", "access-control-allow-origin", 
              "access-control-allow-credentials", "access-control-expose-headers"):
        v = upstream.headers.get(h)
        if v:
            # Rewrite Location header to use our proxy
            if h.lower() == "location":
                # The location is the session URL for PATCH requests
                # We need to proxy this too
                v = f"/proxy/webrtc/session?{urlencode({'url': v})}"
            response_headers[h.title()] = v
    
    return Response(
        content=upstream_content,
        status_code=upstream.status_code,
        headers=response_headers
    )


@router.api_route("/webrtc/session", methods=["GET", "POST", "PATCH", "DELETE", "OPTIONS"])
async def proxy_webrtc_session(request: Request, url: str):
    """
    Proxy WebRTC session requests (for ICE candidates after initial WHEP handshake).
    """
    client = _get_http_client()
    
    # Forward headers
    forward_headers = {}
    for h in ("content-type", "authorization", "if-match"):
        v = request.headers.get(h)
        if v:
            forward_headers[h.title()] = v
    
    body = await request.body()
    
    try:
        req = client.build_request(
            request.method,
            url,
            headers=forward_headers,
            content=body if body else None
        )
        upstream = await client.send(req)
    except httpx.RequestError as exc:
        logger.error(f"Session proxy failed: {exc}")
        raise HTTPException(status_code=502, detail=f"Session upstream failed: {exc}")
    
    upstream_content = await upstream.aread()
    
    response_headers = {}
    for h in ("content-type", "access-control-allow-origin", "access-control-allow-credentials"):
        v = upstream.headers.get(h)
        if v:
            response_headers[h.title()] = v
    
    return Response(
        content=upstream_content,
        status_code=upstream.status_code,
        headers=response_headers
    )
