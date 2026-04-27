import asyncio
import hashlib
import logging
import time
import urllib.parse
import uuid
from typing import Any, Optional

import httpx

BROWSER_CAPTURE_TIMEOUT_MS = 30000
PLAYLIST_TTL_SECONDS = 6 * 60 * 60
REQUEST_TIMEOUT = 12
STREAM_INFO_URL = "https://api.tving.com/v2/media/stream/info"
logger = logging.getLogger(__name__)

DEFAULT_PARAMS = {
    "screenCode": "CSSD0100",
    "networkCode": "CSND0900",
    "osCode": "CSOD0900",
    "teleCode": "CSCD0900",
    "apiKey": "1e7952d0917d6aab1f0293a063697610",
    "mediaCode": "C51850",
    "info": "Y",
    "callingFrom": "HTML5",
    "adReq": "adproxy",
    "uuid": "2410204104-300a362f",
    "deviceInfo": "PC",
    "noCache": "1777106279",
}

BASE_HEADERS = {
    "accept": "application/json, text/plain, */*",
    "accept-language": "ko,en;q=0.9",
    "origin": "https://www.tving.com",
    "referer": "https://www.tving.com/",
    "user-agent": (
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/147.0.0.0 Safari/537.36"
    ),
}

_tving_playlists: dict[str, dict[str, Any]] = {}


class TvingPlaylistError(Exception):
    def __init__(self, message: str, status: int = 400, details: Any = None):
        super().__init__(message)
        self.status = status
        self.details = details


def parse_cookie(cookie: str) -> dict[str, str]:
    values = {}
    for part in cookie.split(";"):
        if "=" not in part:
            continue
        key, value = part.strip().split("=", 1)
        values[key] = urllib.parse.unquote(value)
    return values


def extract_auth(input_value: str) -> dict[str, str]:
    value = input_value.strip()
    if not value:
        raise TvingPlaylistError("Enter a token or cookie.")

    cookie_values = parse_cookie(value)
    if "_tving_token" in cookie_values:
        return {
            "token": cookie_values.get("_tving_token", ""),
            "cookie": value,
            "authToken": cookie_values.get("authToken", ""),
            "accessToken": cookie_values.get("accessToken", ""),
        }

    if value.lower().startswith("bearer "):
        value = value[7:].strip()

    return {
        "token": urllib.parse.unquote(value),
        "cookie": "",
        "authToken": "",
        "accessToken": "",
    }


def build_headers(auth: dict[str, str]) -> dict[str, str]:
    headers = dict(BASE_HEADERS)
    if auth["token"]:
        headers["authorization"] = f"Bearer {auth['token']}"
    if auth["cookie"]:
        headers["cookie"] = auth["cookie"]
    if auth["authToken"]:
        headers["Auth-Token"] = auth["authToken"]
    if auth["accessToken"]:
        headers["Access-Token"] = auth["accessToken"]
    return headers


def cookie_text_to_playwright_cookies(cookie: str) -> list[dict[str, Any]]:
    cookies = []
    for part in cookie.split(";"):
        if "=" not in part:
            continue

        name, value = part.strip().split("=", 1)
        decoded_value = urllib.parse.unquote(value)
        for domain in (".tving.com", "www.tving.com", "api.tving.com", "gw.tving.com"):
            cookies.append(
                {
                    "name": name,
                    "value": decoded_value,
                    "domain": domain,
                    "path": "/",
                    "httpOnly": False,
                    "secure": True,
                    "sameSite": "Lax",
                }
            )
    return cookies


def clean_cookie_text(cookie: str) -> str:
    text = (cookie or "").strip()
    if text.startswith("{") or text.startswith("["):
        raise TvingPlaylistError("Netscape/header-style cookie text is required, not JSON export.")

    lines = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith("#") and not line.startswith("#HttpOnly_"):
            continue
        if line.lower().startswith("cookie:"):
            line = line.split(":", 1)[1].strip()
        if "\t" in line:
            parts = line.split("\t")
            if len(parts) >= 7:
                lines.append(f"{parts[-2]}={parts[-1]}")
            continue
        lines.append(line)
    return "; ".join(lines)


def _proxy_hls_url(remote_url: str, playlist_id: Optional[str] = None) -> str:
    params = {"url": remote_url}
    if playlist_id:
        params["tving_id"] = playlist_id
    return "/proxy/hls?" + urllib.parse.urlencode(params)


def is_http_url(value: Any) -> bool:
    parsed = urllib.parse.urlparse(str(value))
    return parsed.scheme in {"http", "https"} and bool(parsed.netloc)


def is_m3u8_url(value: Any) -> bool:
    if not is_http_url(value):
        return False
    parsed = urllib.parse.urlparse(str(value))
    return parsed.path.endswith(".m3u8")


def clean_candidate_url(value: Any) -> str:
    text = str(value).strip().strip('"').strip("'")
    text = text.replace("\\/", "/")
    return urllib.parse.unquote(text)


def walk_strings(value: Any):
    if isinstance(value, str):
        yield value
    elif isinstance(value, dict):
        for child in value.values():
            yield from walk_strings(child)
    elif isinstance(value, list):
        for child in value:
            yield from walk_strings(child)


def walk_items(value: Any, path: str = ""):
    if isinstance(value, dict):
        for key, child in value.items():
            child_path = f"{path}.{key}" if path else str(key)
            yield from walk_items(child, child_path)
    elif isinstance(value, list):
        for index, child in enumerate(value):
            yield from walk_items(child, f"{path}[{index}]")
    else:
        yield path, value


def encrypted_stream_values(data: dict[str, Any]) -> dict[str, Any]:
    stream = data.get("body", {}).get("stream", {})
    broadcast = stream.get("broadcast", {}) if isinstance(stream, dict) else {}
    values = {}

    if not isinstance(broadcast, dict):
        return values

    for key in ("broad_url", "multi_broad_url"):
        value = broadcast.get(key)
        if isinstance(value, str) and "|" in value:
            prefix, encrypted = value.split("|", 1)
            values[key] = {
                "prefix": prefix,
                "length": len(encrypted),
                "preview": encrypted[:48],
            }

    return values


def find_playlist_candidates(data: dict[str, Any]) -> list[dict[str, str]]:
    preferred_paths = (
        "body.content.info.schedule.broadcast_url[0].broad_url1",
        "body.content.info.schedule.broadcast_url[0].broad_url2",
        "body.content.info.schedule.broadcast_url[0].broad_url4",
        "body.content.info.schedule.broadcast_url[0].broad_url5",
        "body.content.info.schedule.broadcast_url[0].ndvr_host",
        "body.content.info.schedule.broadcast_url[0].audio_url",
    )

    seen = set()
    candidates = []

    all_values = list(walk_items(data))
    by_path = {path: value for path, value in all_values}

    for path in preferred_paths:
        value = by_path.get(path)
        url = clean_candidate_url(value) if value else ""
        if is_m3u8_url(url) and url not in seen:
            seen.add(url)
            candidates.append({"path": path, "url": url})

    for path, value in all_values:
        if not isinstance(value, str):
            continue

        url = clean_candidate_url(value)
        if url in seen:
            continue

        if is_m3u8_url(url):
            seen.add(url)
            candidates.append({"path": path, "url": url})

    return candidates


def find_master_url(data: dict[str, Any]) -> tuple[str, list[dict[str, str]]]:
    candidates = find_playlist_candidates(data)
    if candidates:
        return candidates[0]["url"], candidates

    playback = data.get("data", {}).get("stream", {}).get("playback", {})
    invalid_values = []
    for value in walk_strings(playback):
        text = clean_candidate_url(value)
        if text and not is_http_url(text):
            invalid_values.append(text[:96])

    raise TvingPlaylistError(
        "Could not find a valid http(s) .m3u8 URL in the TVING response.",
        details={
            "candidateCount": 0,
            "encryptedStreamValues": encrypted_stream_values(data),
            "legacyPlaybackKeys": list(playback.keys()) if isinstance(playback, dict) else [],
            "legacyInvalidPlaybackValues": invalid_values[:5],
        },
    )


def is_playlist_uri(line: str) -> bool:
    if not line or line.startswith("#"):
        return False

    parsed = urllib.parse.urlparse(line)
    path = parsed.path if parsed.scheme else line.split("?", 1)[0]
    return path.endswith(".m3u8")


def parse_master_playlist(master_url: str, master_text: str) -> list[dict[str, Any]]:
    base_url = master_url.rsplit("/", 1)[0] + "/"
    playlists = []
    seen = set()

    for raw_line in master_text.splitlines():
        line = raw_line.strip()
        uri = None
        source = "media"

        if line.startswith("#EXT-X-MEDIA:TYPE=AUDIO") and 'URI="' in line:
            uri = line.split('URI="', 1)[1].split('"', 1)[0]
            source = "audio"
        elif is_playlist_uri(line):
            uri = line

        if not uri:
            continue

        absolute_url = urllib.parse.urljoin(base_url, uri)
        if absolute_url in seen:
            continue

        seen.add(absolute_url)
        playlists.append(classify_playlist(absolute_url, source))

    return playlists


def compact_unique_urls(items: list[dict[str, Any]]) -> list[dict[str, Any]]:
    seen = set()
    output = []
    for item in items:
        url = item["url"]
        if url in seen:
            continue
        seen.add(url)
        output.append(item)
    return output


def is_master_playlist_url(url: str) -> bool:
    parsed = urllib.parse.urlparse(url)
    return "sogne-live.tving.com" in parsed.netloc and parsed.path.endswith("/playlist.m3u8")


def is_media_playlist_url(url: str) -> bool:
    parsed = urllib.parse.urlparse(url)
    return "mediatailor" in parsed.netloc and parsed.path.endswith(".m3u8")


def is_segment_url(url: str) -> bool:
    parsed = urllib.parse.urlparse(url)
    path = parsed.path.lower()
    return (
        "segment_" in path
        or path.endswith(".mp4")
        or path.endswith(".m4s")
        or path.endswith(".ts")
    )


def classify_playlist(url: str, source: str) -> dict[str, Any]:
    path = urllib.parse.urlparse(url).path
    filename = path.rsplit("/", 1)[-1]

    if source == "audio" or filename == "11.m3u8":
        kind = "audio"
    elif filename == "0.m3u8":
        kind = "video"
    elif "segment_" in filename:
        kind = "segment"
    elif "mediatailor" in urllib.parse.urlparse(url).netloc:
        kind = "media"
    else:
        kind = "media"

    return {
        "url": url,
        "kind": kind,
        "preferred": filename in {"0.m3u8", "11.m3u8"},
    }


def choose_media_pair(playlists: list[dict[str, Any]]) -> tuple[Optional[str], Optional[str]]:
    video_url = next(
        (
            item["url"]
            for item in playlists
            if item["kind"] not in {"audio", "segment"}
            and urllib.parse.urlparse(item["url"]).path.rsplit("/", 1)[-1] == "1.m3u8"
        ),
        None,
    )
    if not video_url:
        video_url = next(
        (item["url"] for item in playlists if item["kind"] == "video" and item["preferred"]),
        None,
        )
    if not video_url:
        video_url = next(
            (item["url"] for item in playlists if item["kind"] not in {"audio", "segment"}),
            None,
        )

    audio_url = next(
        (item["url"] for item in playlists if item["kind"] == "audio" and item["preferred"]),
        None,
    )
    if not audio_url:
        audio_url = next((item["url"] for item in playlists if item["kind"] == "audio"), None)

    return video_url, audio_url


def build_combined_master_playlist(video_url: str, audio_url: Optional[str], playlist_id: Optional[str] = None) -> str:
    lines = [
        "#EXTM3U",
        "#EXT-X-VERSION:7",
        "#EXT-X-INDEPENDENT-SEGMENTS",
    ]

    audio_group = ""
    codecs = 'avc1.640028'
    if audio_url:
        audio_group = 'tving-audio'
        codecs = 'avc1.640028,mp4a.40.2'
        lines.append(
            '#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="tving-audio",NAME="TVING",'
            f'DEFAULT=YES,AUTOSELECT=YES,URI="{_proxy_hls_url(audio_url, playlist_id)}"'
        )

    stream_attrs = [
        "BANDWIDTH=6000000",
        "AVERAGE-BANDWIDTH=4500000",
        f'CODECS="{codecs}"',
    ]
    if audio_group:
        stream_attrs.append(f'AUDIO="{audio_group}"')

    lines.append("#EXT-X-STREAM-INF:" + ",".join(stream_attrs))
    lines.append(_proxy_hls_url(video_url, playlist_id))
    return "\n".join(lines) + "\n"


async def capture_browser_flow(cookie: str, media_code: str) -> dict[str, Any]:
    try:
        from playwright.async_api import async_playwright
    except ImportError as error:
        raise TvingPlaylistError(
            "Playwright is not installed. Install backend requirements and Chromium.",
            status=500,
        ) from error

    cookie_values = parse_cookie(cookie)
    missing = [key for key in ("_tving_token", "authToken", "accessToken") if not cookie_values.get(key)]
    if missing:
        raise TvingPlaylistError(
            "Full browser cookie is required for TVING browser capture.",
            details={"missingCookieKeys": missing},
        )

    stream_info_url = None
    master_urls: list[dict[str, str]] = []
    media_urls: list[dict[str, str]] = []
    segment_urls: list[dict[str, str]] = []
    relevant_requests: list[str] = []
    relevant_responses: list[dict[str, Any]] = []
    failed_requests: list[dict[str, str]] = []
    page_title = ""
    page_url = ""
    body_preview = ""

    def record_url(url: str):
        nonlocal stream_info_url
        lower_url = url.lower()
        if any(token in lower_url for token in ("tving", "m3u8", "mediatailor", "segment_", ".mp4", ".m4s")):
            relevant_requests.append(url[:300])
        if "stream/sports/v3/stream/info" in lower_url or "media/stream/info" in lower_url:
            stream_info_url = url
        elif is_master_playlist_url(url):
            master_urls.append({"url": url, "source": "sogne-live"})
        elif is_media_playlist_url(url):
            media_urls.append({"url": url, "source": "mediatailor"})
        elif is_segment_url(url):
            segment_urls.append({"url": url, "source": "segment"})

    async with async_playwright() as p:
        browser = await p.chromium.launch(
            headless=True,
            args=["--no-sandbox", "--disable-dev-shm-usage"],
        )
        try:
            context = await browser.new_context(
                user_agent=BASE_HEADERS["user-agent"],
                viewport={"width": 1365, "height": 900},
                locale="ko-KR",
                timezone_id="Asia/Seoul",
            )
            await context.add_cookies(cookie_text_to_playwright_cookies(cookie))
            page = await context.new_page()

            page.on("request", lambda request: record_url(request.url))
            page.on(
                "response",
                lambda response: relevant_responses.append(
                    {"status": response.status, "url": response.url[:300]}
                )
                if any(
                    token in response.url.lower()
                    for token in ("tving", "m3u8", "mediatailor", "segment_", ".mp4", ".m4s")
                )
                else None,
            )
            page.on(
                "requestfailed",
                lambda request: failed_requests.append(
                    {
                        "url": request.url[:300],
                        "failure": (request.failure or "unknown")[:200],
                    }
                )
                if any(
                    token in request.url.lower()
                    for token in ("tving", "m3u8", "mediatailor", "segment_", ".mp4", ".m4s")
                )
                else None,
            )

            await page.goto(
                f"https://www.tving.com/player/{media_code}",
                wait_until="domcontentloaded",
                timeout=BROWSER_CAPTURE_TIMEOUT_MS,
            )
            try:
                await page.wait_for_load_state("networkidle", timeout=5000)
            except Exception:
                pass
            await page.wait_for_timeout(3000)

            for selector in (
                'button[aria-label*="재생"]',
                '[role=button][aria-label*="재생"]',
                'button:has-text("재생")',
                'button:has-text("Play")',
                'text="중계"',
                'a:has-text("중계")',
                '[role=tab]:has-text("중계")',
                "video",
                "[role=button]",
                "button",
                "svg",
            ):
                try:
                    await page.locator(selector).first.click(timeout=1500)
                    await page.wait_for_timeout(1000)
                    if media_urls or segment_urls:
                        break
                except Exception:
                    continue

            try:
                await page.keyboard.press("Space")
            except Exception:
                pass

            deadline = asyncio.get_running_loop().time() + (BROWSER_CAPTURE_TIMEOUT_MS / 1000)
            while asyncio.get_running_loop().time() < deadline:
                if media_urls or (master_urls and segment_urls):
                    break
                await page.wait_for_timeout(500)

            page_title = await page.title()
            page_url = page.url
            try:
                body_preview = (await page.locator("body").inner_text(timeout=1500))[:1200]
            except Exception:
                body_preview = ""
        finally:
            await browser.close()

    master_urls = compact_unique_urls(master_urls)
    media_urls = compact_unique_urls(media_urls)
    segment_urls = compact_unique_urls(segment_urls)

    playlists = [classify_playlist(item["url"], "mediatailor") for item in media_urls]
    if not playlists:
        playlists = [classify_playlist(item["url"], "media") for item in master_urls]

    video_url, audio_url = choose_media_pair(playlists)
    if not video_url:
        details = {
            "mediaCode": media_code,
            "pageTitle": page_title,
            "pageUrl": page_url,
            "bodyPreview": body_preview,
            "streamInfoSeen": bool(stream_info_url),
            "masterCount": len(master_urls),
            "mediaCount": len(media_urls),
            "segmentCount": len(segment_urls),
            "relevantRequestCount": len(relevant_requests),
            "relevantRequests": relevant_requests[:30],
            "relevantResponses": relevant_responses[:30],
            "failedRequests": failed_requests[:10],
            "cookieKeys": sorted(cookie_values.keys()),
        }
        logger.warning("TVING browser capture failed: %s", details)
        raise TvingPlaylistError(
            "Browser capture did not observe a signed playlist request.",
            details=details,
            status=502,
        )

    return {
        "streamInfoUrl": stream_info_url,
        "masterUrl": master_urls[0]["url"] if master_urls else "",
        "videoUrl": video_url,
        "audioUrl": audio_url,
        "playlists": playlists,
        "count": len(playlists),
        "masterPlaylists": master_urls,
        "segments": segment_urls[:12],
        "segmentCount": len(segment_urls),
        "captureMode": "browser",
    }


async def fetch_text(url: str, headers: dict[str, str], error_label: str) -> str:
    try:
        async with httpx.AsyncClient(timeout=REQUEST_TIMEOUT, follow_redirects=True) as client:
            response = await client.get(url, headers=headers)
    except httpx.RequestError as error:
        raise TvingPlaylistError(f"{error_label} request failed: {error}", status=502) from error

    if response.status_code >= 400:
        raise TvingPlaylistError(
            f"{error_label} request error: HTTP {response.status_code}",
            status=response.status_code,
            details=response.text[:1200],
        )

    return response.text


async def fetch_stream_info_flow(cookie: str, media_code: str) -> dict[str, Any]:
    auth = extract_auth(cookie)
    params = dict(DEFAULT_PARAMS)
    params["mediaCode"] = media_code
    params["noCache"] = str(int(time.time() * 1000))

    try:
        async with httpx.AsyncClient(timeout=REQUEST_TIMEOUT, follow_redirects=True) as client:
            response = await client.get(
                STREAM_INFO_URL,
                headers=build_headers(auth),
                params=params,
            )
    except httpx.RequestError as error:
        raise TvingPlaylistError(f"TVING info API request failed: {error}", status=502) from error

    if response.status_code >= 400:
        raise TvingPlaylistError(
            f"TVING info API error: HTTP {response.status_code}",
            status=response.status_code,
            details=response.text[:1200],
        )

    try:
        data = response.json()
    except ValueError as error:
        raise TvingPlaylistError("TVING info API response was not JSON.", status=502) from error

    header_status = data.get("header", {}).get("status")
    if header_status and int(header_status) >= 400:
        raise TvingPlaylistError(
            f"TVING info API returned status {header_status}.",
            status=401 if int(header_status) == 401 else 400,
            details=data.get("header"),
        )

    master_url, candidates = find_master_url(data)
    master_fetch_error = None

    try:
        master_text = await fetch_text(master_url, headers=build_headers(auth), error_label="Master playlist")
        playlists = parse_master_playlist(master_url, master_text)
        if not playlists and "#EXTM3U" in master_text:
            playlists = [classify_playlist(master_url, "media")]
    except TvingPlaylistError as error:
        master_fetch_error = str(error)
        playlists = [classify_playlist(master_url, "media")]

    candidate_urls = {item["url"] for item in playlists}
    for candidate in candidates:
        if candidate["url"] in candidate_urls:
            continue
        candidate_urls.add(candidate["url"])
        playlists.append(classify_playlist(candidate["url"], "media"))

    video_url, audio_url = choose_media_pair(playlists)
    if not video_url:
        raise TvingPlaylistError(
            "TVING info API returned playlists, but no video m3u8 could be selected.",
            status=502,
            details={"playlistCount": len(playlists), "playlists": playlists[:10]},
        )

    return {
        "streamInfoUrl": str(response.url),
        "masterUrl": master_url,
        "videoUrl": video_url,
        "audioUrl": audio_url,
        "playlists": playlists,
        "count": len(playlists),
        "candidates": candidates,
        "masterFetchError": master_fetch_error,
        "encryptedStreamValues": encrypted_stream_values(data),
        "captureMode": "stream_info_api",
    }


def register_combined_playlist(media_code: str, capture: dict[str, Any], cookie: str) -> dict[str, Any]:
    cleanup_expired_playlists()
    video_url = capture["videoUrl"]
    audio_url = capture.get("audioUrl")
    playlist_id = uuid.uuid4().hex
    manifest = build_combined_master_playlist(video_url, audio_url, playlist_id)
    url_hash = hashlib.sha1(f"{video_url}|{audio_url or ''}".encode("utf-8")).hexdigest()

    payload = {
        "id": playlist_id,
        "media_code": media_code,
        "manifest": manifest,
        "video_url": video_url,
        "audio_url": audio_url,
        "url_hash": url_hash,
        "created_at": time.time(),
        "capture": capture,
        "headers": build_headers(extract_auth(cookie)),
    }
    _tving_playlists[playlist_id] = payload
    return payload


def get_registered_playlist(playlist_id: str) -> Optional[dict[str, Any]]:
    playlist = _tving_playlists.get(playlist_id)
    if not playlist:
        return None
    if time.time() - float(playlist.get("created_at") or 0) > PLAYLIST_TTL_SECONDS:
        _tving_playlists.pop(playlist_id, None)
        return None
    return playlist


def cleanup_expired_playlists() -> None:
    cutoff = time.time() - PLAYLIST_TTL_SECONDS
    expired_ids = [
        playlist_id
        for playlist_id, playlist in _tving_playlists.items()
        if float(playlist.get("created_at") or 0) < cutoff
    ]
    for playlist_id in expired_ids:
        _tving_playlists.pop(playlist_id, None)


async def build_tving_entry_from_cookie(cookie_text: str, media_code: str) -> tuple[dict[str, Any], dict[str, Any]]:
    cookie = clean_cookie_text(cookie_text)
    media_code = (media_code or DEFAULT_PARAMS["mediaCode"]).strip() or DEFAULT_PARAMS["mediaCode"]
    try:
        capture = await capture_browser_flow(cookie, media_code)
    except TvingPlaylistError as browser_error:
        diagnostic: dict[str, Any] = {"browser": browser_error.details}
        try:
            fallback_capture = await fetch_stream_info_flow(cookie, media_code)
            diagnostic["streamInfoFallback"] = {
                "captureMode": fallback_capture.get("captureMode"),
                "masterUrl": fallback_capture.get("masterUrl"),
                "masterFetchError": fallback_capture.get("masterFetchError"),
                "count": fallback_capture.get("count"),
                "encryptedStreamValues": fallback_capture.get("encryptedStreamValues"),
            }
        except TvingPlaylistError as fallback_error:
            diagnostic["streamInfoFallbackError"] = {
                "message": str(fallback_error),
                "details": fallback_error.details,
            }

        raise TvingPlaylistError(
            str(browser_error),
            status=browser_error.status,
            details=diagnostic,
        ) from browser_error

    registered = register_combined_playlist(media_code, capture, cookie)

    entry = {
        "id": f"tving_{registered['id']}",
        "url": f"/api/tving/playlist/{registered['id']}.m3u8",
        "file_url": None,
        "hls_url": f"/api/tving/playlist/{registered['id']}.m3u8",
        "stream": "hls",
        "path": None,
        "source": "tving",
        "title": f"TVING {media_code}",
        "duration": 0,
        "thumbnail": "",
        "ext": "m3u8",
        "stream_type": "hls",
        "media_code": media_code,
        "audio_url": capture.get("audioUrl"),
        "video_url": capture.get("videoUrl"),
    }
    return entry, registered
