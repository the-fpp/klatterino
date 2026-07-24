"""Credential-free Streamlink resolver for canonical public Rumble URLs.

When present, the first ``videostream`` card's ``duration`` attribute is an
authoritative offline marker. Current channel pages can omit that legacy
marker, so their bounded embed metadata supplies the live/HLS decision.
"""

from __future__ import annotations

import ipaddress
import json
import os
import re
import sys
import time
from html.parser import HTMLParser
from typing import Any, Iterable
from urllib.parse import urlencode, urlparse

import requests
from streamlink.plugin import Plugin, pluginmatcher
from streamlink.stream.hls import HLSStream


_MAX_PAGE_BYTES = 4 * 1_048_576
_MAX_EMBED_BYTES = 524_288
_TIMEOUT = (5.0, 10.0)
_USER_AGENT = "chatterino-rumble/1"
_EMBED_ID_RE = re.compile(r"^v[a-z0-9]{1,127}$", re.IGNORECASE)
_EMBED_URL_RE = re.compile(
    r"^https://(?:www\.)?rumble\.com/embed/(?P<id>v[a-z0-9]{1,127})/?$",
    re.IGNORECASE,
)
_INTERSTITIAL_TITLES = (
    "just a moment",
    "access denied",
    "verify you are human",
    "attention required",
)
_JSON_EMBED_KEYS = frozenset({"embedUrl", "embed_url", "videoUrl"})
_RESULT_MARKER = "chatterino-rumble-result:"


class _ContractError(ValueError):
    pass


class _PageParser(HTMLParser):
    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.first_duration: str | None = None
        self.embed_ids: set[str] = set()
        self.titles: list[str] = []
        self.challenge = False
        self._depth = 0
        self._in_title = False
        self._title_parts: list[str] = []
        self._body_started = False
        self._script_type: str | None = None
        self._script_parts: list[str] = []
        self._inert_stack: list[str] = []
        self.malformed = False

    @staticmethod
    def _attributes(attrs: list[tuple[str, str | None]]) -> tuple[dict[str, str], set[str]]:
        result: dict[str, str] = {}
        duplicates: set[str] = set()
        for raw_name, raw_value in attrs:
            name = raw_name.lower()
            if name in result:
                duplicates.add(name)
            result[name] = raw_value or ""
        return result, duplicates

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        self._depth += 1
        tag = tag.lower()
        inert_tags = {"template", "noscript", "noembed", "noframes", "textarea", "xmp", "plaintext"}
        if self._inert_stack:
            if tag in inert_tags:
                self._inert_stack.append(tag)
            return
        if tag in inert_tags:
            self._inert_stack.append(tag)
            return
        values, duplicates = self._attributes(attrs)

        if tag == "body":
            self._body_started = True
        elif tag == "title" and not self._body_started:
            self._in_title = True
            self._title_parts = []
        elif tag == "script":
            if "type" in duplicates:
                self.malformed = True
                return
            self._script_type = values.get("type", "").strip().lower()
            self._script_parts = []
        elif tag == "form":
            if duplicates & {"id", "action"}:
                self.malformed = True
            markers = f"{values.get('id', '')} {values.get('action', '')}".lower()
            self.challenge = self.challenge or any(
                marker in markers for marker in ("challenge", "captcha", "cdn-cgi")
            )
        elif tag == "div" and self.first_duration is None:
            classes = values.get("class", "").lower().split()
            if "videostream" in classes:
                if duplicates & {"class", "duration"}:
                    self.malformed = True
                    return
                duration = values.get("duration")
                if duration is None or not re.fullmatch(r"0|[1-9][0-9]*", duration):
                    self.malformed = True
                else:
                    self.first_duration = duration
        elif tag == "iframe":
            if "src" in duplicates:
                self.malformed = True
                return
            match = _EMBED_URL_RE.fullmatch(values.get("src", ""))
            if match:
                self.embed_ids.add(match.group("id").lower())

    def handle_startendtag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        self.handle_starttag(tag, attrs)
        self.handle_endtag(tag)

    def handle_endtag(self, tag: str) -> None:
        tag = tag.lower()
        if self._inert_stack:
            if tag == self._inert_stack[-1]:
                self._inert_stack.pop()
            self._depth = max(0, self._depth - 1)
            return
        if tag == "title" and self._in_title:
            title = "".join(self._title_parts).strip()
            if title:
                self.titles.append(title)
            self._in_title = False
            self._title_parts = []
        elif tag == "script" and self._script_type is not None:
            if self._script_type in {"application/json", "application/ld+json"}:
                try:
                    data = json.loads(
                        "".join(self._script_parts),
                        object_pairs_hook=_unique_json_object,
                    )
                    _collect_json_embed_ids(data, self.embed_ids)
                except (RecursionError, UnicodeError, ValueError):
                    pass
            self._script_type = None
            self._script_parts = []
        self._depth = max(0, self._depth - 1)

    def handle_data(self, data: str) -> None:
        if self._in_title:
            self._title_parts.append(data)
        elif self._script_type is not None:
            self._script_parts.append(data)

    @property
    def complete(self) -> bool:
        return (
            not self._in_title
            and self._script_type is None
            and not self._inert_stack
        )


def _parse_page(body: bytes, *, require_status: bool) -> str:
    if b"\0" in body:
        raise _ContractError("invalid page")
    try:
        text = body.decode("utf-8", errors="strict")
    except UnicodeDecodeError as exc:
        raise _ContractError("invalid page") from exc

    parser = _PageParser()
    try:
        parser.feed(text)
        parser.close()
    except (AssertionError, ValueError) as exc:
        raise _ContractError("invalid page") from exc

    if not parser.complete or len(parser.titles) != 1:
        raise _ContractError("invalid page")
    title = parser.titles[0].lower()
    if parser.malformed or parser.challenge or any(marker in title for marker in _INTERSTITIAL_TITLES):
        raise _ContractError("invalid page")
    if require_status:
        if parser.first_duration is not None and parser.first_duration != "0":
            raise _ContractError("offline")
    if len(parser.embed_ids) != 1:
        raise _ContractError("missing embed")
    return next(iter(parser.embed_ids))


def _safe_media_url(value: Any) -> str | None:
    if not isinstance(value, str) or len(value) > 16_384 or any(ord(char) < 32 for char in value):
        return None
    parsed = urlparse(value)
    if parsed.scheme != "https" or not parsed.hostname or parsed.username or parsed.password:
        return None
    if parsed.port not in (None, 443):
        return None
    host = parsed.hostname.rstrip(".").lower()
    if "." not in host or host == "localhost" or host.endswith((".localhost", ".local", ".internal")):
        return None
    try:
        ipaddress.ip_address(host)
        return None
    except ValueError:
        pass
    labels = host.split(".")
    if any(not re.fullmatch(r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?", label) for label in labels):
        return None
    return value


def _hls_variants(data: Any) -> Iterable[tuple[str, str]]:
    if not isinstance(data, dict) or data.get("live") != 2:
        return []
    ua = data.get("ua")
    hls = ua.get("hls") if isinstance(ua, dict) else None
    if isinstance(hls, dict):
        candidates = list(hls.items())
    elif isinstance(hls, list):
        candidates = [(str(index), item) for index, item in enumerate(hls)]
    else:
        return []

    variants: list[tuple[str, str]] = []
    seen_names: set[str] = set()
    seen_urls: set[str] = set()
    for key, item in candidates:
        if not isinstance(item, dict):
            continue
        url = _safe_media_url(item.get("url"))
        if not url or url in seen_urls:
            continue
        meta = item.get("meta") if isinstance(item.get("meta"), dict) else {}
        height = meta.get("h", key)
        name = f"{height}p" if str(height).isdigit() and int(height) > 0 else "source"
        if name in seen_names:
            continue
        seen_names.add(name)
        seen_urls.add(url)
        variants.append((name, url))
    return variants


def _expand_hls_variants(
    session: Any,
    variants: Iterable[tuple[str, str]],
) -> dict[str, HLSStream]:
    """Turn provider HLS entries into playable Streamlink media streams.

    Rumble commonly exposes a multivariant/master playlist in a metadata entry
    whose own label looks like a concrete quality. Constructing ``HLSStream``
    directly from that URL resolves successfully, but opening it immediately
    reaches EOF because Streamlink refuses to play a variant playlist as a
    media playlist. Expand masters first and retain the provider label only
    when the URL is already a concrete media playlist.
    """

    streams: dict[str, HLSStream] = {}
    parse_failed = False
    for fallback_name, url in variants:
        try:
            expanded = HLSStream.parse_variant_playlist(
                session,
                url,
                check_streams=True,
            )
        except (OSError, RecursionError, UnicodeError, ValueError):
            parse_failed = True
            continue

        if expanded:
            for name, stream in expanded.items():
                streams.setdefault(name, stream)
        else:
            streams.setdefault(fallback_name, HLSStream(session, url))

    if not streams and parse_failed:
        raise _ContractError("HLS playlist rejected")
    return streams


def _unique_json_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise _ContractError("duplicate JSON key")
        result[key] = value
    return result


def _collect_json_embed_ids(
    value: Any,
    output: set[str],
    *,
    depth: int = 0,
    budget: list[int] | None = None,
) -> None:
    if budget is None:
        budget = [4096]
    if depth > 8 or budget[0] <= 0:
        return
    budget[0] -= 1
    if isinstance(value, dict):
        for key, child in value.items():
            if key in _JSON_EMBED_KEYS and isinstance(child, str):
                match = _EMBED_URL_RE.fullmatch(child)
                if match:
                    output.add(match.group("id").lower())
            _collect_json_embed_ids(
                child,
                output,
                depth=depth + 1,
                budget=budget,
            )
    elif isinstance(value, list):
        for child in value:
            _collect_json_embed_ids(
                child,
                output,
                depth=depth + 1,
                budget=budget,
            )


@pluginmatcher(
    re.compile(
        r"https://(?:www\.)?rumble\.com/(?:"
        r"c/(?P<channel>[A-Za-z0-9][A-Za-z0-9_-]{0,79})/?|"
        r"embed/(?P<embed>v[a-z0-9]{1,127})/?|"
        r"(?P<video>v[a-z0-9]{1,127}(?:-[^/?#]*)?\.html)"
        r")$",
        re.IGNORECASE,
    ),
)
class Rumble(Plugin):
    def _result(self, code: str, streams: dict[str, HLSStream] | None = None):
        self.chatterino_result = code
        if code != "success" and os.environ.get("CHATTERINO_STREAMLINK_DIAGNOSTICS") == "1":
            print(f"{_RESULT_MARKER}{code}", file=sys.stderr, flush=True)
        return streams

    def _fetch(self, url: str, *, accept: str, limit: int) -> bytes:
        session = requests.Session()
        session.trust_env = False
        session.auth = None
        session.cookies.clear()
        session.headers.clear()
        try:
            response = session.get(
                url,
                headers={"Accept": accept, "User-Agent": _USER_AGENT},
                allow_redirects=False,
                stream=True,
                timeout=_TIMEOUT,
            )
            try:
                if response.status_code != 200 or response.url != url:
                    raise _ContractError("request rejected")
                media_type = response.headers.get("Content-Type", "").split(";", 1)[0].strip().lower()
                expected = "text/html" if accept == "text/html" else "application/json"
                if media_type != expected:
                    raise _ContractError("media type rejected")
                body = bytearray()
                deadline = time.monotonic() + 20.0
                for chunk in response.iter_content(chunk_size=65_536):
                    if time.monotonic() > deadline:
                        raise _ContractError("request timed out")
                    body.extend(chunk)
                    if len(body) > limit:
                        raise _ContractError("body too large")
                return bytes(body)
            finally:
                response.close()
        finally:
            session.close()

    def _embed_id(self) -> str:
        direct = self.match.group("embed")
        if direct:
            return direct.lower()
        page_url = self.url
        if self.match.group("channel"):
            page_url = self.url.rstrip("/") + "/live/"
        body = self._fetch(page_url, accept="text/html", limit=_MAX_PAGE_BYTES)
        return _parse_page(body, require_status=bool(self.match.group("channel")))

    def _get_streams(self):
        try:
            if (
                re.search(r"[\\\x00-\x20\x7f]", self.url)
                or re.search(r"%(?![0-9A-Fa-f]{2})", self.url)
                or re.search(r"%(?:2[fF]|5[cC]|00)", self.url)
            ):
                return None
            embed_id = self._embed_id()
            if not _EMBED_ID_RE.fullmatch(embed_id):
                return None
            query = urlencode({"request": "video", "ver": "2", "v": embed_id})
            body = self._fetch(
                f"https://rumble.com/embedJS/u3/?{query}",
                accept="application/json",
                limit=_MAX_EMBED_BYTES,
            )
            data = json.loads(body, object_pairs_hook=_unique_json_object)
            if not isinstance(data, dict) or data.get("live") != 2:
                return self._result("no_live_stream")
            variants = list(_hls_variants(data))
            if not variants:
                return self._result("resolver_failed")
            streams = _expand_hls_variants(self.session, variants)
            if not streams:
                return self._result("resolver_failed")
            return self._result("success", streams)
        except _ContractError as exc:
            if str(exc) == "offline":
                return self._result("no_live_stream")
            return self._result("resolver_failed")
        except (
            OSError,
            requests.RequestException,
            RecursionError,
            UnicodeError,
            ValueError,
            json.JSONDecodeError,
        ):
            return self._result("resolver_failed")


__plugin__ = Rumble
