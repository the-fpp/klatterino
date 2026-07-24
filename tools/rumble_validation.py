#!/usr/bin/env python3
"""Bounded, redacted validation for the observed Rumble web-chat contract.

The live commands intentionally report only values from closed vocabularies. Remote
data and user-supplied locators are parsed in memory but can never reach a report.
"""

from __future__ import annotations

import argparse
import base64
import contextlib
import dataclasses
import datetime as dt
import enum
import getpass
import html.parser
import json
import math
import os
import re
import secrets
import signal
import socket
import ssl
import sys
import threading
import time
import unicodedata
import urllib.error
import urllib.parse
import urllib.request
import warnings
from typing import Any, Iterable, Mapping, Optional


VERSION = "1.11.0"
SCHEMA = 1
USER_AGENT = f"chatterino-rumble-validation/{VERSION}"

# Match the native decoded-prefix allowance. Current pages can continue with a
# large unrelated inline application after the title/live/embed contract, so
# page reads stop at the same decisive prefix used by the native resolver.
MAX_PAGE_BYTES = 4 * 1024 * 1024
PAGE_READ_CHUNK_BYTES = 64 * 1024
MAX_EMBED_BYTES = 256 * 1024
MAX_JSON_BYTES = 1024 * 1024
MAX_SSE_BYTES = 2 * 1024 * 1024
MAX_SSE_LINE_BYTES = 256 * 1024
MAX_SESSION_BYTES = 4096
MAX_HEADERS = 96
MAX_HEADER_BYTES = 64 * 1024
MAX_HTML_TAG_CHARS = 4096
MAX_PAGE_JSON_DEPTH = 16
MAX_PAGE_JSON_KEYS = 4096
MAX_EMBED_JSON_DEPTH = 1024

# Python's JSON decoder and the post-parse collapse both recurse. The input is
# pre-scanned to the production 1024-container ceiling before decoding.
if sys.getrecursionlimit() < 4096:
    sys.setrecursionlimit(4096)


class Overall(enum.Enum):
    PASS = "PASS"
    INCONCLUSIVE_ENVIRONMENT = "INCONCLUSIVE_ENVIRONMENT"
    INCONCLUSIVE_INPUT_OR_REMOTE = "INCONCLUSIVE_INPUT_OR_REMOTE"
    CONTRACT_MISMATCH = "CONTRACT_MISMATCH"
    INTERNAL_ERROR = "INTERNAL_ERROR"


class Scenario(enum.Enum):
    LIVE = "live"
    OFFLINE = "offline"
    TEST = "test"


class Stage(enum.Enum):
    PAGE = "channel page"
    EMBED = "embed metadata"
    SSE = "anonymous SSE"
    RECONNECT = "controlled reconnect"
    SESSION = "session probe"
    SEND = "one-shot send"


class Outcome(enum.Enum):
    PASS = "pass"
    OFFLINE = "offline-confirmed"
    ENVIRONMENT = "environment-failure"
    INPUT_REMOTE = "input-or-remote-failure"
    MISMATCH = "contract-mismatch"
    NOT_RUN = "not-run"


class Media(enum.Enum):
    HTML = "HTML"
    JSON = "JSON"
    EVENT_STREAM = "event-stream"
    EMPTY = "empty"
    OTHER = "other"
    NONE = "none"


class Timing(enum.Enum):
    UNDER_1 = "under 1 s"
    ONE_TO_5 = "1–5 s"
    FIVE_TO_20 = "5–20 s"
    OVER_20 = "over 20 s"
    TIMEOUT = "timeout"
    NONE = "none"


class Observation(enum.Enum):
    EMBED_PRESENT = "embed reference present"
    NO_EMBED = "no current embed reference"
    STREAM_PRESENT = "positive stream ID present"
    FIRST_VIDEO_LIVE = "first video duration marks live"
    FIRST_VIDEO_OFFLINE = "first video duration marks offline"
    NO_STREAM = "no positive stream ID"
    INIT_VALID_EMPTY = "valid init; required categories present; initial window empty"
    INIT_VALID_NONEMPTY = "valid init; required categories present; initial window nonempty"
    OFFLINE_204 = "confirmed offline at anonymous SSE stage"
    RECONNECT_OVERLAP = "reconnect init valid; initial-window overlap present"
    RECONNECT_NO_OVERLAP = "reconnect init valid; no initial-window overlap observed"
    RECONNECT_NO_IDS = "reconnect init valid; comparable message IDs not observed"
    NETWORK_UNAVAILABLE = "network environment unavailable"
    REQUEST_TIMEOUT = "bounded request timed out"
    REMOTE_STATUS = "remote status did not establish the contract"
    RETRY_AFTER = "remote rate limit included Retry-After"
    SHAPE_MISMATCH = "reachable response did not match the expected shape"
    REDIRECT_REJECTED = "redirect violated the fixed endpoint policy"
    RESPONSE_TOO_LARGE = "response exceeded the fixed safety bound"
    ACCESS_INTERSTITIAL = "access interstitial detected"
    SESSION_ACCEPTED = "session accepted; authenticated user shape present"
    SESSION_REJECTED = "session was not accepted"
    SEND_ACCEPTED = "one send accepted; stable message ID shape present"
    SEND_REJECTED = "one send rejected; not retried"
    SEND_AMBIGUOUS = "delivery outcome ambiguous after one attempt; not retried"
    SEND_ACK_MISMATCH = "acknowledgement shape mismatched; delivery ambiguous; not retried"
    SEND_DISABLED = "mutation not requested"
    INTERNAL = "internal validation invariant failed"


class FailureKind(enum.Enum):
    NETWORK = "network"
    TIMEOUT = "timeout"
    REDIRECT = "redirect"
    TOO_LARGE = "too-large"
    HEADER_LIMIT = "header-limit"
    SHAPE = "shape"


class PageState(enum.Enum):
    LIVE = "live"
    OFFLINE = "offline"
    INTERSTITIAL = "interstitial"
    MALFORMED = "malformed"


class PageReason(enum.Enum):
    LIVE = "page_live"
    OFFLINE = "page_offline"
    INTERSTITIAL = "page_interstitial"
    TITLE_MISSING = "page_title_missing"
    TITLE_AMBIGUOUS = "page_title_ambiguous"
    EMBED_AMBIGUOUS = "page_embed_ambiguous"
    SCHEMA = "page_schema"


class EmbedState(enum.Enum):
    LIVE = "live"
    OFFLINE = "offline"
    MALFORMED = "malformed"


class EmbedReason(enum.Enum):
    LIVE = "embed_live"
    OFFLINE = "embed_offline"
    JSON_SCHEMA = "embed_json_schema"
    DUPLICATE_KEY = "embed_duplicate_key"
    VID_INVALID = "embed_vid_invalid"
    TITLE_MISSING = "embed_title_missing"
    CHANNEL_ID_INVALID = "embed_channel_id_invalid"
    CHANNEL_TITLE_INVALID = "embed_channel_title_invalid"


class DiagnosticStage(enum.Enum):
    PAGE_REQUEST = "page_request"
    PAGE_CONTRACT = "page_contract"
    EMBED_REQUEST = "embed_request"
    EMBED_CONTRACT = "embed_contract"
    COMPLETE = "complete"


class DiagnosticReason(enum.Enum):
    LIVE_ACCEPTED = "live_accepted"
    OFFLINE_ACCEPTED = "offline_accepted"
    NETWORK_UNAVAILABLE = "network_unavailable"
    REQUEST_TIMEOUT = "request_timeout"
    REDIRECT_REJECTED = "redirect_rejected"
    RESPONSE_TOO_LARGE = "response_too_large"
    HEADER_LIMIT = "header_limit"
    REMOTE_STATUS = "remote_status"
    UNEXPECTED_STATUS = "unexpected_status"
    MEDIA_TYPE = "media_type"
    PAGE_INTERSTITIAL = "page_interstitial"
    PAGE_TITLE_MISSING = "page_title_missing"
    PAGE_TITLE_AMBIGUOUS = "page_title_ambiguous"
    PAGE_EMBED_AMBIGUOUS = "page_embed_ambiguous"
    PAGE_SCHEMA = "page_schema"
    EMBED_JSON_SCHEMA = "embed_json_schema"
    EMBED_DUPLICATE_KEY = "embed_duplicate_key"
    EMBED_VID_INVALID = "embed_vid_invalid"
    EMBED_TITLE_MISSING = "embed_title_missing"
    EMBED_CHANNEL_ID_INVALID = "embed_channel_id_invalid"
    EMBED_CHANNEL_TITLE_INVALID = "embed_channel_title_invalid"
    INTERNAL = "internal"


@dataclasses.dataclass(frozen=True)
class StageResult:
    scenario: Scenario
    stage: Stage
    outcome: Outcome
    status: Optional[int]
    media: Media
    timing: Timing
    observation: Observation


@dataclasses.dataclass(frozen=True)
class Report:
    kind: str
    overall: Overall
    rows: tuple[StageResult, ...]
    send_attempts: int = 0


@dataclasses.dataclass(frozen=True)
class Endpoints:
    page_base: str = "https://rumble.com"
    embed_base: str = "https://rumble.com"
    chat_base: str = "https://web7.rumble.com"
    service_base: str = "https://rumble.com"
    allow_http_for_tests: bool = False

    def hosts_for(self, *bases: str) -> frozenset[str]:
        hosts = []
        for base in bases:
            host = urllib.parse.urlsplit(base).hostname
            if not host:
                raise ValueError("endpoint host missing")
            hosts.append(host.lower())
        return frozenset(hosts)


@dataclasses.dataclass
class OpenedResponse:
    response: Any
    status: int
    media: Media
    timing: Timing
    retry_after_present: bool
    deadline: float
    consumed_body_bytes: int = 0
    body_limit: int = MAX_JSON_BYTES

    def close(self) -> None:
        self.response.close()


@dataclasses.dataclass(frozen=True)
class ResolvedStream:
    stream_id: Optional[int]
    offline_confirmed: bool
    rows: tuple[StageResult, ...]
    overall: Overall


@dataclasses.dataclass(frozen=True)
class InitShape:
    message_ids: frozenset[str]
    has_messages: bool


@dataclasses.dataclass(frozen=True)
class PageContract:
    state: PageState
    reason: PageReason
    embed_id: Optional[str] = None
    first_video_live: Optional[bool] = None


@dataclasses.dataclass(frozen=True)
class EmbedContract:
    state: EmbedState
    reason: EmbedReason
    stream_id: Optional[int] = None


@dataclasses.dataclass(frozen=True)
class DiagnosticReport:
    overall: Overall
    stage: DiagnosticStage
    reason: DiagnosticReason


class ValidationException(Exception):
    def __init__(self, kind: FailureKind, *, ambiguous_post: bool = False):
        super().__init__(kind.value)
        self.kind = kind
        self.ambiguous_post = ambiguous_post


class UsageException(Exception):
    pass


class _WallTimeout(Exception):
    pass


@contextlib.contextmanager
def _hard_wall_deadline(deadline: float) -> Iterable[None]:
    """Interrupt a slow-drip socket operation at an absolute Unix deadline."""
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise _WallTimeout()

    can_interrupt = (
        hasattr(signal, "setitimer")
        and threading.current_thread() is threading.main_thread()
    )
    if not can_interrupt:
        yield
        if time.monotonic() >= deadline:
            raise _WallTimeout()
        return

    previous_handler = signal.getsignal(signal.SIGALRM)
    previous_delay, previous_interval = signal.getitimer(signal.ITIMER_REAL)
    started = time.monotonic()

    def expire(signum: int, frame: Any) -> None:
        raise _WallTimeout()

    signal.signal(signal.SIGALRM, expire)
    alarm_delay = min(remaining, previous_delay) if previous_delay > 0 else remaining
    signal.setitimer(signal.ITIMER_REAL, max(alarm_delay, 0.001))
    try:
        yield
        if time.monotonic() >= deadline:
            raise _WallTimeout()
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, previous_handler)
        if previous_delay > 0:
            elapsed = time.monotonic() - started
            signal.setitimer(
                signal.ITIMER_REAL,
                max(previous_delay - elapsed, 0.001),
                previous_interval,
            )


class NoRedirectHandler(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req: Any, fp: Any, code: int, msg: str,
                         headers: Mapping[str, str], newurl: str) -> None:
        return None


def timing_bucket(seconds: float) -> Timing:
    if seconds < 1:
        return Timing.UNDER_1
    if seconds <= 5:
        return Timing.ONE_TO_5
    if seconds <= 20:
        return Timing.FIVE_TO_20
    return Timing.OVER_20


def media_shape(headers: Mapping[str, str], status: int) -> Media:
    if status == 204:
        return Media.EMPTY
    get_all = getattr(headers, "get_all", None)
    values = get_all("Content-Type") if callable(get_all) else None
    if values is not None:
        if len(values) != 1:
            return Media.OTHER
        raw_content_type = values[0].strip().lower()
    else:
        raw_content_type = (headers.get("Content-Type") or "").strip().lower()
    content_type = raw_content_type.split(";", 1)[0].strip()
    if content_type == "text/html":
        return Media.HTML
    if content_type == "application/json" or (
        content_type.startswith("application/") and content_type.endswith("+json")
    ):
        return Media.JSON
    event_stream_parts = [
        part.strip() for part in raw_content_type.split(";")
    ]
    if (
        event_stream_parts[0] == "text/event-stream"
        and len(event_stream_parts) <= 3
        and all(
            parameter == "charset=utf-8"
            for parameter in event_stream_parts[1:]
        )
    ):
        return Media.EVENT_STREAM
    return Media.OTHER


def _validated_url(url: str, allowed_hosts: frozenset[str],
                   allow_http_for_tests: bool) -> urllib.parse.SplitResult:
    try:
        parsed = urllib.parse.urlsplit(url)
        port = parsed.port
    except ValueError:
        raise ValidationException(FailureKind.REDIRECT) from None
    allowed_schemes = {"https"}
    if allow_http_for_tests:
        allowed_schemes.add("http")
    if parsed.scheme.lower() not in allowed_schemes:
        raise ValidationException(FailureKind.REDIRECT)
    if parsed.username is not None or parsed.password is not None:
        raise ValidationException(FailureKind.REDIRECT)
    host = (parsed.hostname or "").lower()
    if host not in allowed_hosts:
        raise ValidationException(FailureKind.REDIRECT)
    if not allow_http_for_tests:
        # Production accepts only canonical HTTPS authorities: even an
        # explicit default or empty port is rejected before QUrl can erase it.
        if port is not None or parsed.netloc.endswith(":") or parsed.fragment:
            raise ValidationException(FailureKind.REDIRECT)
    return parsed


def _same_resource_redirect(current: str, candidate: str) -> bool:
    """Match the production same-origin, identical-selector redirect rule."""
    before = urllib.parse.urlsplit(current)
    after = urllib.parse.urlsplit(candidate)
    before_query_marker = "?" in current.split("#", 1)[0]
    after_query_marker = "?" in candidate.split("#", 1)[0]
    return (
        before.scheme.lower() == after.scheme.lower()
        and (before.hostname or "").lower() ==
            (after.hostname or "").lower()
        and before.port == after.port
        and before.path == after.path
        and before.query == after.query
        and before_query_marker == after_query_marker
        and before.fragment == after.fragment
        and ("#" in current) == ("#" in candidate)
    )


def _headers_within_limits(
    header_items: Iterable[tuple[Any, Any]],
) -> bool:
    items = tuple(header_items)
    if len(items) > MAX_HEADERS:
        return False
    header_bytes = 0
    for name, value in items:
        encoded_name = str(name).encode("latin-1", "replace")
        encoded_value = str(value).encode("latin-1", "replace")
        if (
            not encoded_name
            or any(
                byte in encoded_name + encoded_value
                for byte in (0, 10, 13)
            )
        ):
            return False
        header_bytes += len(encoded_name) + len(encoded_value) + 4
        if header_bytes > MAX_HEADER_BYTES:
            return False
    return True


class NetworkTransport:
    """Cookie-free HTTP client with manual, host-constrained redirects."""

    def __init__(self, endpoints: Endpoints, timeout: float):
        self.endpoints = endpoints
        self.timeout = timeout
        self.opener = urllib.request.build_opener(NoRedirectHandler())

    def open(
        self,
        method: str,
        url: str,
        *,
        allowed_hosts: frozenset[str],
        headers: Optional[Mapping[str, str]] = None,
        body: Optional[bytes] = None,
        max_redirects: int = 3,
        max_body_bytes: int = MAX_JSON_BYTES,
    ) -> OpenedResponse:
        current = url
        redirects = 0
        consumed_body_bytes = 0
        started = time.monotonic()
        deadline = started + self.timeout
        while True:
            _validated_url(
                current,
                allowed_hosts,
                self.endpoints.allow_http_for_tests,
            )
            request_headers = {"User-Agent": USER_AGENT}
            if headers:
                request_headers.update(headers)
            request = urllib.request.Request(
                current,
                data=body,
                headers=request_headers,
                method=method,
            )
            try:
                remaining = deadline - time.monotonic()
                with _hard_wall_deadline(deadline):
                    response = self.opener.open(
                        request,
                        timeout=max(remaining, 0.001),
                    )
            except urllib.error.HTTPError as exc:
                response = exc
            except _WallTimeout:
                raise ValidationException(
                    FailureKind.TIMEOUT,
                    ambiguous_post=method == "POST",
                ) from None
            except (socket.timeout, TimeoutError):
                raise ValidationException(
                    FailureKind.TIMEOUT,
                    ambiguous_post=method == "POST",
                ) from None
            except (ssl.SSLError, urllib.error.URLError, ConnectionError, OSError):
                raise ValidationException(
                    FailureKind.NETWORK,
                    ambiguous_post=method == "POST",
                ) from None

            status = int(response.getcode() or 0)
            response_headers = response.headers
            raw_items = getattr(response_headers, "raw_items", None)
            header_items = (
                list(raw_items())
                if callable(raw_items)
                else list(response_headers.items())
            )
            if not _headers_within_limits(header_items):
                response.close()
                raise ValidationException(
                    FailureKind.HEADER_LIMIT,
                    ambiguous_post=method == "POST",
                )
            if status in {301, 302, 303, 307, 308}:
                location = response_headers.get("Location")
                try:
                    remaining_limit = max_body_bytes - consumed_body_bytes
                    with _hard_wall_deadline(deadline):
                        discarded = response.read(max(remaining_limit, 0) + 1)
                except _WallTimeout:
                    response.close()
                    raise ValidationException(
                        FailureKind.TIMEOUT,
                        ambiguous_post=method == "POST",
                    ) from None
                except (socket.timeout, TimeoutError):
                    response.close()
                    raise ValidationException(
                        FailureKind.TIMEOUT,
                        ambiguous_post=method == "POST",
                    ) from None
                except (ConnectionError, OSError, urllib.error.URLError):
                    response.close()
                    raise ValidationException(
                        FailureKind.NETWORK,
                        ambiguous_post=method == "POST",
                    ) from None
                response.close()
                consumed_body_bytes += len(discarded)
                if consumed_body_bytes > max_body_bytes:
                    raise ValidationException(
                        FailureKind.TOO_LARGE,
                        ambiguous_post=method == "POST",
                    )
                if not location or redirects >= max_redirects:
                    raise ValidationException(
                        FailureKind.REDIRECT,
                        ambiguous_post=method == "POST",
                    )
                candidate = urllib.parse.urljoin(current, location)
                try:
                    _validated_url(
                        candidate,
                        allowed_hosts,
                        self.endpoints.allow_http_for_tests,
                    )
                except ValidationException:
                    raise ValidationException(
                        FailureKind.REDIRECT,
                        ambiguous_post=method == "POST",
                    ) from None
                if not _same_resource_redirect(current, candidate):
                    raise ValidationException(
                        FailureKind.REDIRECT,
                        ambiguous_post=method == "POST",
                    )
                current = candidate
                # Production applies the body cap independently to each reply
                # in an accepted redirect chain.
                consumed_body_bytes = 0
                redirects += 1
                continue

            return OpenedResponse(
                response=response,
                status=status,
                media=media_shape(response_headers, status),
                timing=timing_bucket(time.monotonic() - started),
                retry_after_present=response_headers.get("Retry-After") is not None,
                deadline=deadline,
                consumed_body_bytes=consumed_body_bytes,
                body_limit=max_body_bytes,
            )

    @staticmethod
    def read_bounded(opened: OpenedResponse, limit: int) -> bytes:
        remaining_limit = min(limit, opened.body_limit) - opened.consumed_body_bytes
        if remaining_limit < 0:
            raise ValidationException(FailureKind.TOO_LARGE)
        try:
            with _hard_wall_deadline(opened.deadline):
                body = opened.response.read(remaining_limit + 1)
        except _WallTimeout:
            raise ValidationException(FailureKind.TIMEOUT) from None
        except (socket.timeout, TimeoutError):
            raise ValidationException(FailureKind.TIMEOUT) from None
        except (ConnectionError, OSError, urllib.error.URLError):
            raise ValidationException(FailureKind.NETWORK) from None
        if len(body) > remaining_limit:
            raise ValidationException(FailureKind.TOO_LARGE)
        return body

    @staticmethod
    def read_channel_page_contract(
        opened: OpenedResponse, limit: int = MAX_PAGE_BYTES,
    ) -> PageContract:
        remaining_limit = min(limit, opened.body_limit) - opened.consumed_body_bytes
        if remaining_limit < 0:
            raise ValidationException(FailureKind.TOO_LARGE)
        body = bytearray()
        while True:
            request_bytes = min(
                PAGE_READ_CHUNK_BYTES,
                remaining_limit - len(body) + 1,
            )
            try:
                with _hard_wall_deadline(opened.deadline):
                    chunk = opened.response.read(max(request_bytes, 1))
            except _WallTimeout:
                raise ValidationException(FailureKind.TIMEOUT) from None
            except (socket.timeout, TimeoutError):
                raise ValidationException(FailureKind.TIMEOUT) from None
            except (ConnectionError, OSError, urllib.error.URLError):
                raise ValidationException(FailureKind.NETWORK) from None
            body.extend(chunk)
            if len(body) > remaining_limit:
                raise ValidationException(FailureKind.TOO_LARGE)

            decisive = parse_channel_page_prefix(bytes(body))
            if decisive is not None:
                return decisive
            if not chunk:
                return parse_page_contract(bytes(body))


def _utf16_size(value: str) -> int:
    """Return QString-compatible UTF-16 code-unit length."""
    return len(value.encode("utf-16-le", "surrogatepass")) // 2


def _is_qt_space(value: str) -> bool:
    return (
        value in "\t\n\v\f\r\u0085"
        or unicodedata.category(value) in {"Zs", "Zl", "Zp"}
    )


def _qt_trimmed(value: str) -> str:
    start = 0
    end = len(value)
    while start < end and _is_qt_space(value[start]):
        start += 1
    while end > start and _is_qt_space(value[end - 1]):
        end -= 1
    return value[start:end]


def _qt_simplified(value: str) -> str:
    result: list[str] = []
    pending_space = False
    for character in value:
        if _is_qt_space(character):
            pending_space = bool(result)
            continue
        if pending_space:
            result.append(" ")
            pending_space = False
        result.append(character)
    return "".join(result)


def _has_valid_percent_encoding(value: str) -> bool:
    return re.search(r"%(?![0-9A-Fa-f]{2})", value) is None


def parse_channel_locator(value: str) -> str:
    value = _qt_trimmed(value)
    if not value or _utf16_size(value) > 4096 or "\0" in value:
        raise UsageException()
    slug_pattern = re.compile(r"[A-Za-z0-9](?:[A-Za-z0-9_-]{0,79})\Z")
    if slug_pattern.fullmatch(value):
        return value

    if "://" not in value or not _has_valid_percent_encoding(value):
        raise UsageException()
    scheme_end = value.find("://")
    authority_start = scheme_end + 3
    authority_end = len(value)
    for delimiter in "/?#":
        position = value.find(delimiter, authority_start)
        if position >= 0:
            authority_end = min(authority_end, position)
    raw_authority = value[authority_start:authority_end]
    if raw_authority.lower() not in {"rumble.com", "www.rumble.com"}:
        raise UsageException()
    try:
        parsed = urllib.parse.urlsplit(value)
        port = parsed.port
    except ValueError:
        raise UsageException() from None
    if parsed.scheme.lower() != "https":
        raise UsageException()
    if (parsed.hostname or "").lower() not in {"rumble.com", "www.rumble.com"}:
        raise UsageException()
    if parsed.username is not None or parsed.password is not None:
        raise UsageException()
    if port is not None or ":" in parsed.netloc:
        raise UsageException()
    raw_segments = parsed.path.split("/")
    if raw_segments and raw_segments[0] == "":
        raw_segments = raw_segments[1:]
    if raw_segments and raw_segments[-1] == "":
        raw_segments = raw_segments[:-1]
    if any(not segment for segment in raw_segments) or len(raw_segments) not in {2, 3}:
        raise UsageException()
    try:
        segments = [
            urllib.parse.unquote(segment, encoding="utf-8", errors="strict")
            for segment in raw_segments
        ]
    except UnicodeDecodeError:
        raise UsageException() from None
    if segments[0].lower() not in {"c", "user"}:
        raise UsageException()
    if len(segments) == 3 and segments[2].lower() != "live":
        raise UsageException()
    slug = segments[1]
    if not slug_pattern.fullmatch(slug):
        raise UsageException()
    return slug


_EMBED_URL = re.compile(
    r"https://(?:www\.)?rumble\.com/embed/(v[a-z0-9]{1,127})/?\Z",
    re.IGNORECASE,
)
_DECIMAL_ID = re.compile(r"[1-9][0-9]{0,127}\Z")
_JSON_EMBED_KEYS = frozenset({"embedUrl", "embed_url", "videoUrl"})
_INTERSTITIAL_TITLES = (
    "just a moment",
    "access denied",
    "verify you are human",
    "attention required",
)


class _DuplicateJsonKey(ValueError):
    pass


class _JsonNumber(str):
    pass


class _JsonObjectPairs(list[tuple[str, Any]]):
    pass


def _reject_json_constant(value: str) -> Any:
    raise ValueError()


def _json_number(value: str) -> _JsonNumber:
    try:
        finite = math.isfinite(float(value))
    except (OverflowError, ValueError):
        finite = False
    if not finite:
        raise ValueError()
    return _JsonNumber(value)


def _validate_json_string(value: str) -> None:
    # JSON escapes can materialize lone surrogates in Python, while the
    # production Qt/RapidJSON parsers require valid Unicode scalar text.
    value.encode("utf-16-le", "strict")


def _collapse_unique_page_json(
    value: Any,
    *,
    depth: int = 0,
    key_count: Optional[list[int]] = None,
) -> Any:
    """Apply the production page-script depth, key, and uniqueness bounds."""
    if key_count is None:
        key_count = [0]
    if isinstance(value, _JsonObjectPairs):
        if depth >= MAX_PAGE_JSON_DEPTH:
            raise ValueError()
        result: dict[str, Any] = {}
        for key, child in value:
            _validate_json_string(key)
            key_count[0] += 1
            if key_count[0] > MAX_PAGE_JSON_KEYS:
                raise ValueError()
            if key in result:
                raise _DuplicateJsonKey()
            result[key] = _collapse_unique_page_json(
                child, depth=depth + 1, key_count=key_count,
            )
        return result
    if isinstance(value, list):
        if depth >= MAX_PAGE_JSON_DEPTH:
            raise ValueError()
        return [
            _collapse_unique_page_json(
                child, depth=depth + 1, key_count=key_count,
            )
            for child in value
        ]
    if isinstance(value, str) and not isinstance(value, _JsonNumber):
        _validate_json_string(value)
    return value


def _load_unique_json(body: bytes | str) -> Any:
    parsed = json.loads(
        body,
        object_pairs_hook=_JsonObjectPairs,
        parse_int=_json_number,
        parse_float=_json_number,
        parse_constant=_reject_json_constant,
    )
    return _collapse_unique_page_json(parsed)


def _collapse_json_pairs(value: Any) -> Any:
    if isinstance(value, _JsonObjectPairs):
        result: dict[str, Any] = {}
        for key, child in value:
            result[key] = _collapse_json_pairs(child)
        return result
    if isinstance(value, list):
        return [_collapse_json_pairs(child) for child in value]
    return value


def _normalize_qt_json_escapes(body: bytes) -> bytes:
    """Mirror QJson's permissive unknown string-escape handling."""
    normalized = bytearray()
    in_string = False
    cursor = 0
    standard = b'"\\/bfnrtu'
    while cursor < len(body):
        byte = body[cursor]
        if not in_string:
            normalized.append(byte)
            if byte == ord('"'):
                in_string = True
            cursor += 1
            continue
        if byte == ord('"'):
            normalized.append(byte)
            in_string = False
            cursor += 1
            continue
        if byte != ord('\\') or cursor + 1 >= len(body):
            normalized.append(byte)
            cursor += 1
            continue
        escaped = body[cursor + 1]
        if escaped in standard:
            normalized.extend((byte, escaped))
        else:
            # QJson consumes one raw byte and appends that byte's U+00XX code
            # point, rather than decoding a following UTF-8 sequence.
            normalized.extend(chr(escaped).encode("utf-8"))
        cursor += 2
    return bytes(normalized)


def _normalize_qt_json_numbers(body: bytes) -> bytes:
    """Canonicalize QJson's accepted abbreviated fraction spellings."""
    normalized = bytearray()
    in_string = False
    cursor = 0
    delimiters = b",]} \t\r\n"
    while cursor < len(body):
        byte = body[cursor]
        if in_string:
            normalized.append(byte)
            if byte == ord('\\') and cursor + 1 < len(body):
                normalized.append(body[cursor + 1])
                cursor += 2
                continue
            if byte == ord('"'):
                in_string = False
            cursor += 1
            continue
        if byte == ord('"'):
            normalized.append(byte)
            in_string = True
            cursor += 1
            continue
        if (
            byte == ord('-')
            and cursor + 2 < len(body)
            and body[cursor + 1] == ord('.')
            and ord('0') <= body[cursor + 2] <= ord('9')
        ):
            normalized.extend(b"-0.")
            cursor += 2
            continue
        if (
            byte == ord('.')
            and cursor + 1 < len(body)
            and ord('0') <= body[cursor + 1] <= ord('9')
        ):
            normalized.extend(b"0.")
            cursor += 1
            continue
        if ord('0') <= byte <= ord('9'):
            start = cursor
            while (
                cursor < len(body)
                and ord('0') <= body[cursor] <= ord('9')
            ):
                cursor += 1
            normalized.extend(body[start:cursor])
            if cursor < len(body) and body[cursor] == ord('.'):
                following = body[cursor + 1] if cursor + 1 < len(body) else None
                normalized.append(ord('.'))
                cursor += 1
                if following is None or following in delimiters or following in b"eE":
                    normalized.append(ord('0'))
            continue
        normalized.append(byte)
        cursor += 1
    return bytes(normalized)


def _validate_embed_json_depth(body: bytes) -> None:
    depth = 0
    in_string = False
    cursor = 0
    while cursor < len(body):
        byte = body[cursor]
        if in_string:
            if byte == ord('\\') and cursor + 1 < len(body):
                cursor += 2
                continue
            if byte == ord('"'):
                in_string = False
            cursor += 1
            continue
        if byte == ord('"'):
            in_string = True
        elif byte in (ord('{'), ord('[')):
            depth += 1
            if depth > MAX_EMBED_JSON_DEPTH:
                raise ValueError()
        elif byte in (ord('}'), ord(']')):
            depth -= 1
        cursor += 1


def _load_embed_json(body: bytes) -> Any:
    """Reject duplicate top-level keys; tolerate unrelated nested metadata."""
    _validate_embed_json_depth(body)
    normalized = _normalize_qt_json_escapes(body)
    parsed = json.loads(
        _normalize_qt_json_numbers(normalized),
        strict=False,
        object_pairs_hook=_JsonObjectPairs,
        parse_int=_json_number,
        parse_float=_json_number,
        parse_constant=_reject_json_constant,
    )
    if not isinstance(parsed, _JsonObjectPairs):
        return _collapse_json_pairs(parsed)
    seen: set[str] = set()
    for key, _value in parsed:
        if key in seen:
            raise _DuplicateJsonKey()
        seen.add(key)
    return _collapse_json_pairs(parsed)


def _embed_id_from_url(value: str) -> Optional[str]:
    match = _EMBED_URL.fullmatch(value)
    if not match:
        return None
    identifier = match.group(1)
    # The production URL regex is case-insensitive, but its captured ID is
    # subsequently validated by the lowercase-only ID grammar.
    return identifier if identifier == identifier.lower() else None


def _collect_json_embed_ids(value: Any, result: set[str], depth: int = 0) -> None:
    if depth > 8:
        return
    if isinstance(value, dict):
        for key, child in value.items():
            if key in _JSON_EMBED_KEYS and isinstance(child, str):
                identifier = _embed_id_from_url(child)
                if identifier:
                    result.add(identifier)
            _collect_json_embed_ids(child, result, depth + 1)
    elif isinstance(value, list):
        for child in value:
            _collect_json_embed_ids(child, result, depth + 1)


def _decode_title(raw: str) -> str:
    replacements = (
        ("&lt;", "<"),
        ("&gt;", ">"),
        ("&quot;", '"'),
        ("&apos;", "'"),
        ("&#39;", "'"),
        ("&amp;", "&"),
    )
    for encoded, decoded in replacements:
        raw = re.sub(re.escape(encoded), decoded, raw, flags=re.IGNORECASE)
    return _qt_simplified(raw)


def _is_html_space(value: str) -> bool:
    return value in " \t\r\n\f"


def _is_html_tag_name_char(value: str) -> bool:
    return value.isascii() and (value.isalnum() or value in "-:")


def _is_html_attribute_name_char(value: str) -> bool:
    return (
        not _is_html_space(value)
        and value not in '\0"\'<>/='
    )


def _parse_raw_start_tag(
    raw: str,
) -> Optional[tuple[str, dict[str, str], bool]]:
    """Parse the exact raw start tag with the production scanner's grammar."""
    if (
        _utf16_size(raw) > MAX_HTML_TAG_CHARS + 1
        or len(raw) < 3
        or raw[0] != "<"
        or raw[-1] != ">"
    ):
        return None
    end = len(raw) - 1
    cursor = 1
    name_start = cursor
    while cursor < end and _is_html_tag_name_char(raw[cursor]):
        cursor += 1
    if cursor == name_start:
        return None
    name = raw[name_start:cursor].lower()
    attributes: dict[str, str] = {}
    self_closing = False

    if cursor < end and not _is_html_space(raw[cursor]) and raw[cursor] != "/":
        return None

    while cursor < end:
        while cursor < end and _is_html_space(raw[cursor]):
            cursor += 1
        if cursor == end:
            break
        if raw[cursor] == "/":
            cursor += 1
            while cursor < end and _is_html_space(raw[cursor]):
                cursor += 1
            if cursor != end:
                return None
            self_closing = True
            break

        attribute_start = cursor
        while cursor < end and _is_html_attribute_name_char(raw[cursor]):
            cursor += 1
        if cursor == attribute_start:
            return None
        attribute = raw[attribute_start:cursor].lower()
        while cursor < end and _is_html_space(raw[cursor]):
            cursor += 1

        value = ""
        if cursor < end and raw[cursor] == "=":
            cursor += 1
            while cursor < end and _is_html_space(raw[cursor]):
                cursor += 1
            if cursor == end:
                return None
            if raw[cursor] in "\"'":
                quote = raw[cursor]
                cursor += 1
                value_start = cursor
                while cursor < end and raw[cursor] != quote:
                    cursor += 1
                if cursor == end:
                    return None
                value = raw[value_start:cursor]
                cursor += 1
            else:
                value_start = cursor
                while cursor < end and not _is_html_space(raw[cursor]):
                    if raw[cursor] in "\"'<`=":
                        return None
                    cursor += 1
                if cursor == value_start:
                    return None
                value = raw[value_start:cursor]
        if attribute in attributes:
            return None
        attributes[attribute] = value
    return name, attributes, self_closing


_PAGE_CONTRACT_TAGS = frozenset({
    "form", "iframe", "noembed", "noframes", "noscript", "plaintext",
    "script", "style", "template", "textarea", "title", "xmp",
})


def _raw_html_tag_name(
    raw: str, closing: bool = False, start: int = 0,
) -> Optional[str]:
    cursor = start + (2 if closing else 1)
    if len(raw) <= cursor or raw[start] != "<":
        return None
    if closing and raw[start + 1] != "/":
        return None
    name_start = cursor
    while cursor < len(raw) and _is_html_tag_name_char(raw[cursor]):
        cursor += 1
    return raw[name_start:cursor].lower() if cursor != name_start else None


def _find_unbounded_html_tag_end(
    raw: str, cursor: int,
) -> Optional[int]:
    quote = ""
    while cursor < len(raw):
        value = raw[cursor]
        if quote:
            if value == quote:
                quote = ""
        elif value in "\"'":
            quote = value
        elif value == ">":
            return cursor
        cursor += 1
    return None


class _PageHtmlParser(html.parser.HTMLParser):
    """Conservative subset of the production resolver's page scanner."""

    _IGNORED_RAW = frozenset({
        "style", "textarea", "noscript", "xmp", "noembed", "noframes",
    })
    _TEMPLATE_RAW = _IGNORED_RAW | frozenset({"script", "title", "iframe"})

    def __init__(self) -> None:
        super().__init__(convert_charrefs=False)
        self.malformed = False
        self.titles: list[str] = []
        self.embed_ids: set[str] = set()
        self.first_video_live: Optional[bool] = None
        self.first_video_malformed = False
        self.challenge_form = False
        self.template_depth = 0
        self.raw_name: Optional[str] = None
        self.raw_data: list[str] = []
        self.raw_type = ""
        self.raw_in_template = False
        self.raw_title_is_document = False
        self.body_started = False
        self.plaintext = False

    def handle_starttag(self, tag: str,
                        _attrs: list[tuple[str, Optional[str]]]) -> None:
        tag = tag.lower()
        if self.plaintext:
            return
        if self.raw_name is not None:
            if self.raw_name == "title" and self.raw_title_is_document:
                self.malformed = True
            return

        # The response body is already capped. Hydration and inline-SVG tags
        # cannot affect title, interstitial, raw/template, or embed extraction,
        # so their attribute count and length must not invalidate the page.
        raw = self.get_starttag_text() or ""
        raw_name = _raw_html_tag_name(raw)
        raw_name_end = 1 + len(raw_name or "")
        raw_name_has_boundary = (
            raw_name is not None
            and raw_name_end < len(raw)
            and (
                _is_html_space(raw[raw_name_end])
                or raw[raw_name_end] in "/>"
            )
        )
        if (
            not self.template_depth
            and raw_name == "body"
            and raw_name_has_boundary
        ):
            # Only the pre-body HTML document title identifies the page.
            # Body titles (most notably inline-SVG accessibility labels) are
            # foreign/inert metadata and must not make the page ambiguous.
            self.body_started = True
            return
        if (
            not self.template_depth
            and self.body_started
            and raw_name == "title"
            and raw_name_has_boundary
        ):
            self.raw_name = "title"
            self.raw_data = []
            self.raw_type = ""
            self.raw_in_template = False
            self.raw_title_is_document = False
            return
        if (
            not self.template_depth
            and raw_name == "div"
            and raw_name_has_boundary
            and self.first_video_live is None
            and not self.first_video_malformed
        ):
            if "videostream" not in raw.lower():
                return
            parsed = _parse_raw_start_tag(raw)
            if parsed is None or parsed[0] != raw_name:
                self.first_video_malformed = True
                return
            classes = [
                value
                for value in re.split(
                    r"[ \t\r\n\f]+", parsed[1].get("class", ""),
                )
                if value
            ]
            if "videostream" not in classes:
                return
            if "duration" not in parsed[1]:
                self.first_video_malformed = True
                return
            duration = parsed[1]["duration"]
            if duration == "0":
                self.first_video_live = True
            elif re.fullmatch(r"[1-9][0-9]*", duration):
                self.first_video_live = False
            else:
                self.first_video_malformed = True
            return
        if not self.template_depth and raw_name not in _PAGE_CONTRACT_TAGS:
            return

        parsed = _parse_raw_start_tag(raw)
        if parsed is None or parsed[0] != raw_name:
            self.malformed = True
            return
        tag = parsed[0]
        attributes = parsed[1]
        self_closing = parsed[2]
        if self.template_depth:
            # Template contents are inert but still tokenized by production;
            # every candidate tag must satisfy the same raw grammar.
            if tag == "template":
                self.template_depth += 1
            elif tag == "plaintext":
                self.malformed = True
            elif tag in self._TEMPLATE_RAW:
                # Production skips through the matching raw-element close so
                # fake template closes inside inert fallback text stay inert.
                self.raw_name = tag
                self.raw_data = []
                self.raw_type = ""
                self.raw_in_template = True
                self.raw_title_is_document = False
            return

        if tag == "plaintext":
            self.plaintext = True
            return
        if tag == "template":
            self.template_depth = 1
            return
        if tag == "title":
            if self_closing:
                self.malformed = True
                return
            self.raw_name = tag
            self.raw_data = []
            self.raw_in_template = False
            self.raw_title_is_document = True
            return
        if tag == "script":
            if self_closing:
                self.malformed = True
                return
            self.raw_name = tag
            self.raw_data = []
            self.raw_type = _qt_trimmed(attributes.get("type", "")).lower()
            self.raw_in_template = False
            return
        if tag in self._IGNORED_RAW:
            self.raw_name = tag
            self.raw_data = []
            self.raw_in_template = False
            return
        if tag == "form":
            for name in ("id", "action"):
                value = attributes.get(name, "").lower()
                if any(token in value for token in ("challenge", "captcha", "cdn-cgi")):
                    self.challenge_form = True
        if tag == "iframe":
            source = attributes.get("src")
            if source is not None:
                identifier = _embed_id_from_url(source)
                if identifier:
                    self.embed_ids.add(identifier)
            self.raw_name = tag
            self.raw_data = []
            self.raw_in_template = False

    def handle_startendtag(self, tag: str,
                           attrs: list[tuple[str, Optional[str]]]) -> None:
        # HTML ignores the slash for the non-void elements relevant here.
        self.handle_starttag(tag, attrs)

    def parse_endtag(self, index: int) -> int:
        name = _raw_html_tag_name(self.rawdata, closing=True, start=index)
        if name is None:
            self.malformed = True
            end = _find_unbounded_html_tag_end(
                self.rawdata, index + 2,
            )
            return -1 if end is None else end + 1
        if not self.template_depth and name not in _PAGE_CONTRACT_TAGS:
            end = _find_unbounded_html_tag_end(
                self.rawdata, index + 2,
            )
            return -1 if end is None else end + 1

        end = self.rawdata.find(">", index + 2)
        if end >= 0:
            raw = self.rawdata[index:end + 1]
            if (
                _utf16_size(raw) > MAX_HTML_TAG_CHARS + 1
                or re.fullmatch(
                    r"</[A-Za-z0-9:-]+[ \t\r\n\f]*>", raw,
                ) is None
            ):
                self.malformed = True
        return super().parse_endtag(index)

    def handle_endtag(self, tag: str) -> None:
        tag = tag.lower()
        if self.plaintext:
            return
        if self.raw_name is not None:
            if tag != self.raw_name:
                if self.raw_name == "title" and self.raw_title_is_document:
                    self.malformed = True
                return
            raw_name = self.raw_name
            raw = "".join(self.raw_data)
            raw_in_template = self.raw_in_template
            raw_title_is_document = self.raw_title_is_document
            self.raw_name = None
            self.raw_data = []
            self.raw_in_template = False
            self.raw_title_is_document = False
            raw_type = self.raw_type
            self.raw_type = ""
            if raw_in_template:
                return
            if raw_name == "title":
                if not raw_title_is_document:
                    return
                if _utf16_size(raw) > 4096 or "<" in raw:
                    self.malformed = True
                else:
                    title = _decode_title(raw)
                    if title:
                        self.titles.append(title)
            elif raw_name == "script" and raw_type in {
                "application/json", "application/ld+json",
            }:
                try:
                    value = _load_unique_json(raw)
                except (
                    UnicodeDecodeError,
                    json.JSONDecodeError,
                    _DuplicateJsonKey,
                    RecursionError,
                    ValueError,
                ):
                    return
                _collect_json_embed_ids(value, self.embed_ids)
            return
        if self.template_depth and tag == "template":
            self.template_depth -= 1

    def handle_data(self, data: str) -> None:
        if self.raw_name == "script" or (
            self.raw_name == "title" and self.raw_title_is_document
        ):
            self.raw_data.append(data)

    def handle_entityref(self, name: str) -> None:
        if self.raw_name == "title" and self.raw_title_is_document:
            self.raw_data.append(f"&{name};")

    def handle_charref(self, name: str) -> None:
        if self.raw_name == "title" and self.raw_title_is_document:
            self.raw_data.append(f"&#{name};")

    def handle_comment(self, data: str) -> None:
        if self.raw_name == "title" and self.raw_title_is_document:
            self.malformed = True


def parse_channel_page_prefix(body: bytes) -> Optional[PageContract]:
    """Return only an authoritative channel result from a partial document."""
    if b"\0" in body:
        return None
    try:
        text = body.decode("utf-8", "strict")
    except UnicodeDecodeError:
        return None

    parser = _PageHtmlParser()
    try:
        # Do not close the incremental parser: the received chunk may end in
        # unrelated markup after the decisive contract tokens.
        parser.feed(text)
    except (AssertionError, ValueError):
        return None
    if (
        parser.malformed
        or not parser.body_started
        or len(parser.titles) != 1
        or _utf16_size(parser.titles[0]) > 4096
    ):
        return None

    lowered_title = parser.titles[0].lower()
    if parser.challenge_form or any(
        token in lowered_title for token in _INTERSTITIAL_TITLES
    ):
        return PageContract(PageState.INTERSTITIAL, PageReason.INTERSTITIAL)
    if parser.first_video_live is False:
        return PageContract(
            PageState.OFFLINE,
            PageReason.OFFLINE,
            first_video_live=False,
        )
    if parser.first_video_live is True and len(parser.embed_ids) == 1:
        return PageContract(
            PageState.LIVE,
            PageReason.LIVE,
            next(iter(parser.embed_ids)),
            first_video_live=True,
        )
    return None


def parse_page_contract(body: bytes) -> PageContract:
    if b"\0" in body:
        return PageContract(PageState.MALFORMED, PageReason.SCHEMA)
    try:
        text = body.decode("utf-8", "strict")
    except UnicodeDecodeError:
        return PageContract(PageState.MALFORMED, PageReason.SCHEMA)

    parser = _PageHtmlParser()
    try:
        parser.feed(text)
        # HTMLParser otherwise discards an unterminated comment/declaration on
        # close(), while the production scanner rejects it.
        if parser.rawdata:
            parser.malformed = True
        parser.close()
    except (AssertionError, ValueError):
        return PageContract(PageState.MALFORMED, PageReason.SCHEMA)
    if parser.malformed or parser.raw_name is not None or parser.template_depth:
        return PageContract(PageState.MALFORMED, PageReason.SCHEMA)
    if not parser.titles:
        return PageContract(PageState.MALFORMED, PageReason.TITLE_MISSING)
    if len(parser.titles) != 1:
        return PageContract(PageState.MALFORMED, PageReason.TITLE_AMBIGUOUS)

    lowered_title = parser.titles[0].lower()
    if parser.challenge_form or any(
        token in lowered_title for token in _INTERSTITIAL_TITLES
    ):
        return PageContract(PageState.INTERSTITIAL, PageReason.INTERSTITIAL)
    if parser.first_video_malformed:
        return PageContract(PageState.MALFORMED, PageReason.SCHEMA)
    if parser.first_video_live is False:
        return PageContract(
            PageState.OFFLINE,
            PageReason.OFFLINE,
            first_video_live=False,
        )
    if not parser.embed_ids:
        if parser.first_video_live is True:
            return PageContract(PageState.MALFORMED, PageReason.SCHEMA)
        return PageContract(PageState.OFFLINE, PageReason.OFFLINE)
    if len(parser.embed_ids) != 1:
        return PageContract(PageState.MALFORMED, PageReason.EMBED_AMBIGUOUS)
    return PageContract(
        PageState.LIVE,
        PageReason.LIVE,
        next(iter(parser.embed_ids)),
        parser.first_video_live,
    )


def extract_embed_id(body: bytes) -> Optional[str]:
    parsed = parse_page_contract(body)
    return parsed.embed_id if parsed.state == PageState.LIVE else None


def parse_embed_contract(body: bytes) -> EmbedContract:
    try:
        document = _load_embed_json(body)
    except _DuplicateJsonKey:
        return EmbedContract(EmbedState.MALFORMED, EmbedReason.DUPLICATE_KEY)
    except (
        UnicodeDecodeError,
        json.JSONDecodeError,
        RecursionError,
        ValueError,
    ):
        return EmbedContract(EmbedState.MALFORMED, EmbedReason.JSON_SCHEMA)
    if not isinstance(document, dict):
        return EmbedContract(EmbedState.MALFORMED, EmbedReason.JSON_SCHEMA)
    if "vid" not in document or document["vid"] is None:
        return EmbedContract(EmbedState.OFFLINE, EmbedReason.OFFLINE)

    value = document["vid"]
    if isinstance(value, bool) or not isinstance(value, (str, _JsonNumber)):
        return EmbedContract(EmbedState.MALFORMED, EmbedReason.VID_INVALID)
    if not _DECIMAL_ID.fullmatch(value):
        return EmbedContract(EmbedState.MALFORMED, EmbedReason.VID_INVALID)

    title = document.get("title")
    if (
        not isinstance(title, str)
        or not _qt_trimmed(title)
        or _utf16_size(title) > 4096
    ):
        return EmbedContract(EmbedState.MALFORMED, EmbedReason.TITLE_MISSING)

    channel_id = document.get("channel_id")
    if channel_id is not None:
        if (
            isinstance(channel_id, bool)
            or not isinstance(channel_id, (str, _JsonNumber))
            or not _DECIMAL_ID.fullmatch(channel_id)
        ):
            return EmbedContract(
                EmbedState.MALFORMED,
                EmbedReason.CHANNEL_ID_INVALID,
            )

    if "channel_title" in document:
        channel_title = document["channel_title"]
        if (
            not isinstance(channel_title, str)
            or not _qt_trimmed(channel_title)
            or _utf16_size(channel_title) > 4096
        ):
            return EmbedContract(
                EmbedState.MALFORMED,
                EmbedReason.CHANNEL_TITLE_INVALID,
            )
    return EmbedContract(EmbedState.LIVE, EmbedReason.LIVE, int(value))


def parse_embed_stream_id(body: bytes) -> Optional[int]:
    parsed = parse_embed_contract(body)
    if parsed.state == EmbedState.MALFORMED:
        raise ValidationException(FailureKind.SHAPE)
    return parsed.stream_id


def _failure_result(scenario: Scenario, stage: Stage,
                    exc: ValidationException) -> tuple[StageResult, Overall]:
    if exc.kind == FailureKind.TIMEOUT:
        return (
            StageResult(
                scenario, stage, Outcome.ENVIRONMENT, None, Media.NONE,
                Timing.TIMEOUT, Observation.REQUEST_TIMEOUT,
            ),
            Overall.INCONCLUSIVE_ENVIRONMENT,
        )
    if exc.kind == FailureKind.NETWORK:
        return (
            StageResult(
                scenario, stage, Outcome.ENVIRONMENT, None, Media.NONE,
                Timing.NONE, Observation.NETWORK_UNAVAILABLE,
            ),
            Overall.INCONCLUSIVE_ENVIRONMENT,
        )
    observation = {
        FailureKind.REDIRECT: Observation.REDIRECT_REJECTED,
        FailureKind.TOO_LARGE: Observation.RESPONSE_TOO_LARGE,
        FailureKind.HEADER_LIMIT: Observation.RESPONSE_TOO_LARGE,
        FailureKind.SHAPE: Observation.SHAPE_MISMATCH,
    }[exc.kind]
    return (
        StageResult(
            scenario, stage, Outcome.MISMATCH, None, Media.NONE,
            Timing.NONE, observation,
        ),
        Overall.CONTRACT_MISMATCH,
    )


def _remote_failure(scenario: Scenario, stage: Stage,
                    opened: OpenedResponse) -> StageResult:
    observation = (
        Observation.RETRY_AFTER
        if opened.status == 429 and opened.retry_after_present
        else Observation.REMOTE_STATUS
    )
    return StageResult(
        scenario, stage, Outcome.INPUT_REMOTE, opened.status, opened.media,
        opened.timing, observation,
    )


def resolve_stream(
    transport: NetworkTransport,
    slug: str,
    scenario: Scenario,
    *,
    expect_live: bool,
) -> ResolvedStream:
    endpoints = transport.endpoints
    page_hosts = endpoints.hosts_for(endpoints.page_base)
    if not endpoints.allow_http_for_tests:
        page_hosts = page_hosts | frozenset({"rumble.com", "www.rumble.com"})
    page_result: Optional[StageResult] = None
    embed_id: Optional[str] = None
    saw_valid_page_without_embed = False

    for profile in ("c", "user"):
        quoted_slug = urllib.parse.quote(slug, safe="")
        url = f"{endpoints.page_base}/{profile}/{quoted_slug}/live/"
        try:
            opened = transport.open(
                "GET",
                url,
                allowed_hosts=page_hosts,
                headers={"Accept": "text/html"},
            )
        except ValidationException as exc:
            row, overall = _failure_result(scenario, Stage.PAGE, exc)
            return ResolvedStream(None, False, (row,), overall)
        try:
            if (
                200 <= opened.status < 300
                and opened.status != 204
                and opened.media != Media.HTML
            ):
                row = StageResult(
                    scenario, Stage.PAGE, Outcome.MISMATCH, opened.status,
                    opened.media, opened.timing, Observation.SHAPE_MISMATCH,
                )
                return ResolvedStream(
                    None, False, (row,), Overall.CONTRACT_MISMATCH,
                )
            try:
                page = (
                    transport.read_channel_page_contract(opened)
                    if opened.status == 200
                    else None
                )
                if page is None:
                    transport.read_bounded(opened, MAX_PAGE_BYTES)
            except ValidationException as exc:
                row, overall = _failure_result(scenario, Stage.PAGE, exc)
                return ResolvedStream(None, False, (row,), overall)
            if opened.status == 404:
                if profile == "c":
                    continue
                if saw_valid_page_without_embed:
                    break
                return ResolvedStream(
                    None,
                    False,
                    (_remote_failure(scenario, Stage.PAGE, opened),),
                    Overall.INCONCLUSIVE_INPUT_OR_REMOTE,
                )
            if opened.status != 200:
                if 200 <= opened.status < 300:
                    row = StageResult(
                        scenario, Stage.PAGE, Outcome.MISMATCH, opened.status,
                        opened.media, opened.timing,
                        Observation.SHAPE_MISMATCH,
                    )
                    return ResolvedStream(
                        None, False, (row,), Overall.CONTRACT_MISMATCH,
                    )
                return ResolvedStream(
                    None,
                    False,
                    (_remote_failure(scenario, Stage.PAGE, opened),),
                    Overall.INCONCLUSIVE_INPUT_OR_REMOTE,
                )
            if page is None:
                row = StageResult(
                    scenario, Stage.PAGE, Outcome.MISMATCH, opened.status,
                    opened.media, opened.timing, Observation.SHAPE_MISMATCH,
                )
                return ResolvedStream(
                    None, False, (row,), Overall.CONTRACT_MISMATCH,
                )
            if page.state == PageState.INTERSTITIAL:
                row = StageResult(
                    scenario, Stage.PAGE, Outcome.INPUT_REMOTE, opened.status,
                    opened.media, opened.timing, Observation.ACCESS_INTERSTITIAL,
                )
                return ResolvedStream(
                    None, False, (row,), Overall.INCONCLUSIVE_INPUT_OR_REMOTE,
                )
            if page.state == PageState.MALFORMED:
                row = StageResult(
                    scenario, Stage.PAGE, Outcome.MISMATCH, opened.status,
                    opened.media, opened.timing, Observation.SHAPE_MISMATCH,
                )
                return ResolvedStream(
                    None, False, (row,), Overall.CONTRACT_MISMATCH,
                )
            embed_id = page.embed_id
            if page.state == PageState.LIVE and embed_id:
                page_result = StageResult(
                    scenario, Stage.PAGE, Outcome.PASS, opened.status,
                    opened.media, opened.timing,
                    Observation.FIRST_VIDEO_LIVE
                    if page.first_video_live is True
                    else Observation.EMBED_PRESENT,
                )
                break
            saw_valid_page_without_embed = True
            page_result = StageResult(
                scenario,
                Stage.PAGE,
                Outcome.INPUT_REMOTE if expect_live else Outcome.OFFLINE,
                opened.status,
                opened.media,
                opened.timing,
                Observation.FIRST_VIDEO_OFFLINE
                if page.first_video_live is False
                else Observation.NO_EMBED,
            )
            # The legacy /user/ profile is attempted only when the primary
            # /c/ endpoint is 404. A valid offline page is authoritative.
            break
        finally:
            opened.close()

    if not embed_id:
        if page_result is None:
            page_result = StageResult(
                scenario, Stage.PAGE, Outcome.INPUT_REMOTE, 404, Media.OTHER,
                Timing.NONE, Observation.REMOTE_STATUS,
            )
        return ResolvedStream(
            None,
            not expect_live and saw_valid_page_without_embed,
            (page_result,),
            Overall.INCONCLUSIVE_INPUT_OR_REMOTE if expect_live else Overall.PASS,
        )

    query = urllib.parse.urlencode({"request": "video", "ver": "2", "v": embed_id})
    embed_url = f"{endpoints.embed_base}/embedJS/u3/?{query}"
    try:
        opened = transport.open(
            "GET",
            embed_url,
            allowed_hosts=(
                endpoints.hosts_for(endpoints.embed_base)
                if endpoints.allow_http_for_tests
                else endpoints.hosts_for(endpoints.embed_base)
                | frozenset({"rumble.com", "www.rumble.com"})
            ),
            headers={"Accept": "application/json"},
            max_body_bytes=MAX_EMBED_BYTES,
        )
    except ValidationException as exc:
        row, overall = _failure_result(scenario, Stage.EMBED, exc)
        return ResolvedStream(None, False, (page_result, row), overall)
    try:
        if (
            200 <= opened.status < 300
            and opened.status != 204
            and opened.media != Media.JSON
        ):
            row = StageResult(
                scenario, Stage.EMBED, Outcome.MISMATCH, opened.status,
                opened.media, opened.timing, Observation.SHAPE_MISMATCH,
            )
            return ResolvedStream(
                None, False, (page_result, row), Overall.CONTRACT_MISMATCH,
            )
        try:
            body = transport.read_bounded(opened, MAX_EMBED_BYTES)
        except ValidationException as exc:
            row, overall = _failure_result(scenario, Stage.EMBED, exc)
            return ResolvedStream(None, False, (page_result, row), overall)
        if opened.status != 200:
            if 200 <= opened.status < 300:
                row = StageResult(
                    scenario, Stage.EMBED, Outcome.MISMATCH, opened.status,
                    opened.media, opened.timing, Observation.SHAPE_MISMATCH,
                )
                return ResolvedStream(
                    None, False, (page_result, row),
                    Overall.CONTRACT_MISMATCH,
                )
            row = _remote_failure(scenario, Stage.EMBED, opened)
            return ResolvedStream(
                None, False, (page_result, row), Overall.INCONCLUSIVE_INPUT_OR_REMOTE,
            )
        try:
            embed = parse_embed_contract(body)
            if embed.state == EmbedState.MALFORMED:
                raise ValidationException(FailureKind.SHAPE)
            stream_id = embed.stream_id
        except ValidationException as exc:
            row, overall = _failure_result(scenario, Stage.EMBED, exc)
            return ResolvedStream(None, False, (page_result, row), overall)
        if stream_id is None:
            row = StageResult(
                scenario,
                Stage.EMBED,
                Outcome.INPUT_REMOTE if expect_live else Outcome.OFFLINE,
                opened.status,
                opened.media,
                opened.timing,
                Observation.NO_STREAM,
            )
            return ResolvedStream(
                None,
                not expect_live,
                (page_result, row),
                Overall.INCONCLUSIVE_INPUT_OR_REMOTE if expect_live else Overall.PASS,
            )
        row = StageResult(
            scenario, Stage.EMBED, Outcome.PASS, opened.status, opened.media,
            opened.timing, Observation.STREAM_PRESENT,
        )
        return ResolvedStream(stream_id, False, (page_result, row), Overall.PASS)
    finally:
        opened.close()


def _diagnostic_failure(
    stage: DiagnosticStage,
    exc: ValidationException,
) -> DiagnosticReport:
    reason = {
        FailureKind.NETWORK: DiagnosticReason.NETWORK_UNAVAILABLE,
        FailureKind.TIMEOUT: DiagnosticReason.REQUEST_TIMEOUT,
        FailureKind.REDIRECT: DiagnosticReason.REDIRECT_REJECTED,
        FailureKind.TOO_LARGE: DiagnosticReason.RESPONSE_TOO_LARGE,
        FailureKind.HEADER_LIMIT: DiagnosticReason.HEADER_LIMIT,
        FailureKind.SHAPE: DiagnosticReason.PAGE_SCHEMA,
    }[exc.kind]
    overall = (
        Overall.INCONCLUSIVE_ENVIRONMENT
        if exc.kind in {FailureKind.NETWORK, FailureKind.TIMEOUT}
        else Overall.CONTRACT_MISMATCH
    )
    return DiagnosticReport(overall, stage, reason)


def diagnose_channel(
    slug: str,
    *,
    endpoints: Endpoints = Endpoints(),
    timeout: float = 20,
) -> DiagnosticReport:
    """Resolve one channel through page/embed without exposing remote values."""
    transport = NetworkTransport(endpoints, timeout)
    page_hosts = endpoints.hosts_for(endpoints.page_base)
    if not endpoints.allow_http_for_tests:
        page_hosts |= frozenset({"rumble.com", "www.rumble.com"})
    page: Optional[PageContract] = None

    for profile in ("c", "user"):
        url = (
            f"{endpoints.page_base}/{profile}/"
            f"{urllib.parse.quote(slug, safe='')}/live/"
        )
        try:
            opened = transport.open(
                "GET",
                url,
                allowed_hosts=page_hosts,
                headers={"Accept": "text/html"},
            )
        except ValidationException as exc:
            return _diagnostic_failure(DiagnosticStage.PAGE_REQUEST, exc)
        try:
            if (
                200 <= opened.status < 300
                and opened.status != 204
                and opened.media != Media.HTML
            ):
                return DiagnosticReport(
                    Overall.CONTRACT_MISMATCH,
                    DiagnosticStage.PAGE_CONTRACT,
                    DiagnosticReason.MEDIA_TYPE,
                )
            try:
                parsed_page = (
                    transport.read_channel_page_contract(opened)
                    if opened.status == 200
                    else None
                )
                if parsed_page is None:
                    transport.read_bounded(opened, MAX_PAGE_BYTES)
            except ValidationException as exc:
                return _diagnostic_failure(DiagnosticStage.PAGE_REQUEST, exc)
            if opened.status == 404 and profile == "c":
                continue
            if opened.status != 200:
                if 200 <= opened.status < 300:
                    return DiagnosticReport(
                        Overall.CONTRACT_MISMATCH,
                        DiagnosticStage.PAGE_CONTRACT,
                        DiagnosticReason.UNEXPECTED_STATUS,
                    )
                return DiagnosticReport(
                    Overall.INCONCLUSIVE_INPUT_OR_REMOTE,
                    DiagnosticStage.PAGE_REQUEST,
                    DiagnosticReason.REMOTE_STATUS,
                )
            page = parsed_page
        finally:
            opened.close()
        break

    if page is None:
        return DiagnosticReport(
            Overall.INCONCLUSIVE_INPUT_OR_REMOTE,
            DiagnosticStage.PAGE_REQUEST,
            DiagnosticReason.REMOTE_STATUS,
        )
    if page.state == PageState.INTERSTITIAL:
        return DiagnosticReport(
            Overall.INCONCLUSIVE_INPUT_OR_REMOTE,
            DiagnosticStage.PAGE_CONTRACT,
            DiagnosticReason.PAGE_INTERSTITIAL,
        )
    if page.state == PageState.MALFORMED:
        reason = {
            PageReason.TITLE_MISSING: DiagnosticReason.PAGE_TITLE_MISSING,
            PageReason.TITLE_AMBIGUOUS:
                DiagnosticReason.PAGE_TITLE_AMBIGUOUS,
            PageReason.EMBED_AMBIGUOUS:
                DiagnosticReason.PAGE_EMBED_AMBIGUOUS,
            PageReason.SCHEMA: DiagnosticReason.PAGE_SCHEMA,
        }.get(page.reason, DiagnosticReason.PAGE_SCHEMA)
        return DiagnosticReport(
            Overall.CONTRACT_MISMATCH,
            DiagnosticStage.PAGE_CONTRACT,
            reason,
        )
    if page.state == PageState.OFFLINE:
        return DiagnosticReport(
            Overall.PASS,
            DiagnosticStage.COMPLETE,
            DiagnosticReason.OFFLINE_ACCEPTED,
        )
    if page.embed_id is None:
        return DiagnosticReport(
            Overall.INTERNAL_ERROR,
            DiagnosticStage.PAGE_CONTRACT,
            DiagnosticReason.INTERNAL,
        )

    query = urllib.parse.urlencode({
        "request": "video", "ver": "2", "v": page.embed_id,
    })
    url = f"{endpoints.embed_base}/embedJS/u3/?{query}"
    embed_hosts = endpoints.hosts_for(endpoints.embed_base)
    if not endpoints.allow_http_for_tests:
        embed_hosts |= frozenset({"rumble.com", "www.rumble.com"})
    try:
        opened = transport.open(
            "GET",
            url,
            allowed_hosts=embed_hosts,
            headers={"Accept": "application/json"},
            max_body_bytes=MAX_EMBED_BYTES,
        )
    except ValidationException as exc:
        return _diagnostic_failure(DiagnosticStage.EMBED_REQUEST, exc)
    try:
        if (
            200 <= opened.status < 300
            and opened.status != 204
            and opened.media != Media.JSON
        ):
            return DiagnosticReport(
                Overall.CONTRACT_MISMATCH,
                DiagnosticStage.EMBED_CONTRACT,
                DiagnosticReason.MEDIA_TYPE,
            )
        try:
            body = transport.read_bounded(opened, MAX_EMBED_BYTES)
        except ValidationException as exc:
            return _diagnostic_failure(DiagnosticStage.EMBED_REQUEST, exc)
        if opened.status != 200:
            if 200 <= opened.status < 300:
                return DiagnosticReport(
                    Overall.CONTRACT_MISMATCH,
                    DiagnosticStage.EMBED_CONTRACT,
                    DiagnosticReason.UNEXPECTED_STATUS,
                )
            return DiagnosticReport(
                Overall.INCONCLUSIVE_INPUT_OR_REMOTE,
                DiagnosticStage.EMBED_REQUEST,
                DiagnosticReason.REMOTE_STATUS,
            )
        embed = parse_embed_contract(body)
    finally:
        opened.close()

    if embed.state == EmbedState.OFFLINE:
        return DiagnosticReport(
            Overall.PASS,
            DiagnosticStage.COMPLETE,
            DiagnosticReason.OFFLINE_ACCEPTED,
        )
    if embed.state == EmbedState.MALFORMED:
        reason = {
            EmbedReason.JSON_SCHEMA: DiagnosticReason.EMBED_JSON_SCHEMA,
            EmbedReason.DUPLICATE_KEY: DiagnosticReason.EMBED_DUPLICATE_KEY,
            EmbedReason.VID_INVALID: DiagnosticReason.EMBED_VID_INVALID,
            EmbedReason.TITLE_MISSING: DiagnosticReason.EMBED_TITLE_MISSING,
            EmbedReason.CHANNEL_ID_INVALID:
                DiagnosticReason.EMBED_CHANNEL_ID_INVALID,
            EmbedReason.CHANNEL_TITLE_INVALID:
                DiagnosticReason.EMBED_CHANNEL_TITLE_INVALID,
        }.get(embed.reason, DiagnosticReason.EMBED_JSON_SCHEMA)
        return DiagnosticReport(
            Overall.CONTRACT_MISMATCH,
            DiagnosticStage.EMBED_CONTRACT,
            reason,
        )
    return DiagnosticReport(
        Overall.PASS,
        DiagnosticStage.COMPLETE,
        DiagnosticReason.LIVE_ACCEPTED,
    )


def parse_sse_init(opened: OpenedResponse) -> InitShape:
    total = 0
    data_lines: list[bytes] = []

    def consume_event() -> Optional[InitShape]:
        nonlocal data_lines
        if not data_lines:
            return None
        raw = b"\n".join(data_lines)
        data_lines = []
        try:
            event = json.loads(raw)
        except (UnicodeDecodeError, json.JSONDecodeError):
            raise ValidationException(FailureKind.SHAPE) from None
        if not isinstance(event, dict):
            raise ValidationException(FailureKind.SHAPE)
        if event.get("type") != "init":
            return None
        data = event.get("data")
        if not isinstance(data, dict):
            raise ValidationException(FailureKind.SHAPE)
        if not all(key in data for key in ("users", "channels", "config", "messages")):
            raise ValidationException(FailureKind.SHAPE)
        users = data.get("users")
        channels = data.get("channels")
        config = data.get("config")
        messages = data.get("messages")
        # The current inline RumbleChat client iterates both catalogs with
        # Array.forEach(). Older synthetic fixtures incorrectly modeled them
        # as objects keyed by ID.
        if not isinstance(users, list) or not isinstance(channels, list):
            raise ValidationException(FailureKind.SHAPE)
        if not all(isinstance(user, dict) for user in users):
            raise ValidationException(FailureKind.SHAPE)
        if not all(isinstance(channel, dict) for channel in channels):
            raise ValidationException(FailureKind.SHAPE)
        if (
            not isinstance(config, dict)
            or not isinstance(config.get("badges"), dict)
        ):
            raise ValidationException(FailureKind.SHAPE)
        if not isinstance(messages, list):
            raise ValidationException(FailureKind.SHAPE)
        if not all(isinstance(message, dict) for message in messages):
            raise ValidationException(FailureKind.SHAPE)
        message_ids: set[str] = set()
        for message in messages:
            if not isinstance(message, dict):
                continue
            value = message.get("id")
            if isinstance(value, (str, int)) and not isinstance(value, bool):
                message_ids.add(str(value))
        return InitShape(frozenset(message_ids), bool(messages))

    while total <= MAX_SSE_BYTES:
        try:
            with _hard_wall_deadline(opened.deadline):
                line = opened.response.readline(MAX_SSE_LINE_BYTES + 1)
        except _WallTimeout:
            raise ValidationException(FailureKind.TIMEOUT) from None
        except (socket.timeout, TimeoutError):
            raise ValidationException(FailureKind.TIMEOUT) from None
        except (ConnectionError, OSError, urllib.error.URLError):
            raise ValidationException(FailureKind.NETWORK) from None
        if len(line) > MAX_SSE_LINE_BYTES:
            raise ValidationException(FailureKind.TOO_LARGE)
        if not line:
            result = consume_event()
            if result:
                return result
            break
        total += len(line)
        if total > MAX_SSE_BYTES:
            raise ValidationException(FailureKind.TOO_LARGE)
        stripped = line.rstrip(b"\r\n")
        if not stripped:
            result = consume_event()
            if result:
                return result
            continue
        if stripped.startswith(b":"):
            continue
        if stripped.startswith(b"data:"):
            value = stripped[5:]
            if value.startswith(b" "):
                value = value[1:]
            data_lines.append(value)
    raise ValidationException(FailureKind.SHAPE)


def _validate_sse_once(
    transport: NetworkTransport,
    stream_id: int,
    scenario: Scenario,
    stage: Stage,
) -> tuple[Optional[InitShape], StageResult, Overall]:
    url = f"{transport.endpoints.chat_base}/chat/api/chat/{stream_id}/stream"
    try:
        opened = transport.open(
            "GET",
            url,
            allowed_hosts=transport.endpoints.hosts_for(transport.endpoints.chat_base),
            headers={
                "Accept": "text/event-stream",
                "Cache-Control": "no-cache",
                "Origin": "https://rumble.com",
                "Referer": "https://rumble.com/",
            },
            max_body_bytes=MAX_SSE_BYTES,
        )
    except ValidationException as exc:
        row, overall = _failure_result(scenario, stage, exc)
        return None, row, overall
    try:
        if opened.status == 204:
            row = StageResult(
                scenario,
                stage,
                Outcome.OFFLINE if scenario == Scenario.OFFLINE else Outcome.INPUT_REMOTE,
                opened.status,
                opened.media,
                opened.timing,
                Observation.OFFLINE_204,
            )
            overall = (
                Overall.PASS
                if scenario == Scenario.OFFLINE
                else Overall.INCONCLUSIVE_INPUT_OR_REMOTE
            )
            return None, row, overall
        if opened.status != 200:
            return (
                None,
                _remote_failure(scenario, stage, opened),
                Overall.INCONCLUSIVE_INPUT_OR_REMOTE,
            )
        if opened.media != Media.EVENT_STREAM:
            row = StageResult(
                scenario, stage, Outcome.MISMATCH, opened.status, opened.media,
                opened.timing, Observation.SHAPE_MISMATCH,
            )
            return None, row, Overall.CONTRACT_MISMATCH
        try:
            init = parse_sse_init(opened)
        except ValidationException as exc:
            # Once a reachable endpoint advertises SSE, failure to produce a valid
            # init is a contract mismatch rather than proof of network unavailability.
            observation = (
                Observation.REQUEST_TIMEOUT
                if exc.kind == FailureKind.TIMEOUT
                else Observation.RESPONSE_TOO_LARGE
                if exc.kind == FailureKind.TOO_LARGE
                else Observation.SHAPE_MISMATCH
            )
            row = StageResult(
                scenario,
                stage,
                Outcome.MISMATCH,
                opened.status,
                opened.media,
                Timing.TIMEOUT if exc.kind == FailureKind.TIMEOUT else opened.timing,
                observation,
            )
            return None, row, Overall.CONTRACT_MISMATCH
        observation = (
            Observation.INIT_VALID_NONEMPTY
            if init.has_messages
            else Observation.INIT_VALID_EMPTY
        )
        row = StageResult(
            scenario, stage, Outcome.PASS, opened.status, opened.media,
            opened.timing, observation,
        )
        return init, row, Overall.PASS
    finally:
        opened.close()


def _combine_overall(values: Iterable[Overall]) -> Overall:
    values = tuple(values)
    for candidate in (
        Overall.INTERNAL_ERROR,
        Overall.CONTRACT_MISMATCH,
        Overall.INCONCLUSIVE_ENVIRONMENT,
        Overall.INCONCLUSIVE_INPUT_OR_REMOTE,
    ):
        if candidate in values:
            return candidate
    return Overall.PASS


def validate_public(
    live_slug: str,
    offline_slug: str,
    *,
    endpoints: Endpoints = Endpoints(),
    timeout: float = 20,
) -> Report:
    transport = NetworkTransport(endpoints, timeout)
    rows: list[StageResult] = []
    outcomes: list[Overall] = []

    live = resolve_stream(
        transport, live_slug, Scenario.LIVE, expect_live=True,
    )
    rows.extend(live.rows)
    outcomes.append(live.overall)
    if live.stream_id is not None:
        first, row, overall = _validate_sse_once(
            transport, live.stream_id, Scenario.LIVE, Stage.SSE,
        )
        rows.append(row)
        outcomes.append(overall)
        if first is not None:
            second, reconnect_row, reconnect_overall = _validate_sse_once(
                transport, live.stream_id, Scenario.LIVE, Stage.RECONNECT,
            )
            if second is not None:
                if first.message_ids and second.message_ids:
                    observation = (
                        Observation.RECONNECT_OVERLAP
                        if first.message_ids & second.message_ids
                        else Observation.RECONNECT_NO_OVERLAP
                    )
                else:
                    observation = Observation.RECONNECT_NO_IDS
                reconnect_row = dataclasses.replace(
                    reconnect_row,
                    observation=observation,
                )
            rows.append(reconnect_row)
            outcomes.append(reconnect_overall)

    offline = resolve_stream(
        transport, offline_slug, Scenario.OFFLINE, expect_live=False,
    )
    rows.extend(offline.rows)
    outcomes.append(offline.overall)
    if offline.stream_id is not None:
        init, row, overall = _validate_sse_once(
            transport, offline.stream_id, Scenario.OFFLINE, Stage.SSE,
        )
        if init is not None:
            row = dataclasses.replace(
                row,
                outcome=Outcome.INPUT_REMOTE,
                observation=Observation.REMOTE_STATUS,
            )
            overall = Overall.INCONCLUSIVE_INPUT_OR_REMOTE
        rows.append(row)
        outcomes.append(overall)

    return Report("public", _combine_overall(outcomes), tuple(rows))


def _read_json_object(transport: NetworkTransport, opened: OpenedResponse) -> dict[str, Any]:
    body = transport.read_bounded(opened, MAX_JSON_BYTES)
    try:
        value = json.loads(body)
    except (UnicodeDecodeError, json.JSONDecodeError):
        raise ValidationException(FailureKind.SHAPE) from None
    if not isinstance(value, dict):
        raise ValidationException(FailureKind.SHAPE)
    return value


def _session_is_valid(document: Mapping[str, Any]) -> bool:
    user = document.get("user")
    if not isinstance(user, dict):
        return False
    identifier = user.get("id")
    return isinstance(identifier, (str, int)) and not isinstance(identifier, bool) and bool(identifier)


def _send_has_stable_id(document: Mapping[str, Any]) -> bool:
    data = document.get("data")
    if not isinstance(data, dict):
        return False
    identifier = data.get("id")
    return isinstance(identifier, (str, int)) and not isinstance(identifier, bool) and bool(identifier)


def _make_request_id() -> str:
    return base64.urlsafe_b64encode(os.urandom(32)).rstrip(b"=").decode("ascii")


def _make_message() -> str:
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%MZ")
    return f"Chatterino protocol validation {stamp} {secrets.token_hex(4)}"


def validate_authenticated(
    slug: str,
    session: str,
    *,
    send: bool,
    endpoints: Endpoints = Endpoints(),
    timeout: float = 20,
    request_id_factory: Any = _make_request_id,
    message_factory: Any = _make_message,
) -> Report:
    transport = NetworkTransport(endpoints, timeout)
    rows: list[StageResult] = []
    outcomes: list[Overall] = []

    resolved = resolve_stream(
        transport, slug, Scenario.TEST, expect_live=True,
    )
    rows.extend(resolved.rows)
    outcomes.append(resolved.overall)
    if resolved.stream_id is None:
        return Report("authenticated", _combine_overall(outcomes), tuple(rows), 0)

    service_query = urllib.parse.urlencode({"name": "user.has_unread_notifications"})
    service_url = f"{endpoints.service_base}/service.php?{service_query}"
    cookie_header = f"u_s={session}"
    try:
        opened = transport.open(
            "GET",
            service_url,
            allowed_hosts=endpoints.hosts_for(endpoints.service_base),
            headers={
                "Accept": "application/json, text/plain, */*",
                "Content-Type": "application/x-www-form-urlencoded",
                "Cookie": cookie_header,
            },
            max_redirects=0,
        )
    except ValidationException as exc:
        row, overall = _failure_result(Scenario.TEST, Stage.SESSION, exc)
        rows.append(row)
        outcomes.append(overall)
        return Report("authenticated", _combine_overall(outcomes), tuple(rows), 0)
    try:
        if opened.status != 200:
            observation = (
                Observation.SESSION_REJECTED
                if opened.status in {401, 403}
                else Observation.RETRY_AFTER
                if opened.status == 429 and opened.retry_after_present
                else Observation.REMOTE_STATUS
            )
            row = StageResult(
                Scenario.TEST, Stage.SESSION, Outcome.INPUT_REMOTE,
                opened.status, opened.media, opened.timing, observation,
            )
            rows.append(row)
            outcomes.append(Overall.INCONCLUSIVE_INPUT_OR_REMOTE)
            return Report("authenticated", _combine_overall(outcomes), tuple(rows), 0)
        if opened.media != Media.JSON:
            row = StageResult(
                Scenario.TEST, Stage.SESSION, Outcome.MISMATCH, opened.status,
                opened.media, opened.timing, Observation.SHAPE_MISMATCH,
            )
            rows.append(row)
            outcomes.append(Overall.CONTRACT_MISMATCH)
            return Report("authenticated", _combine_overall(outcomes), tuple(rows), 0)
        try:
            document = _read_json_object(transport, opened)
        except ValidationException as exc:
            row, overall = _failure_result(Scenario.TEST, Stage.SESSION, exc)
            rows.append(row)
            outcomes.append(overall)
            return Report("authenticated", _combine_overall(outcomes), tuple(rows), 0)
        if not _session_is_valid(document):
            row = StageResult(
                Scenario.TEST, Stage.SESSION, Outcome.INPUT_REMOTE,
                opened.status, opened.media, opened.timing,
                Observation.SESSION_REJECTED,
            )
            rows.append(row)
            outcomes.append(Overall.INCONCLUSIVE_INPUT_OR_REMOTE)
            return Report("authenticated", _combine_overall(outcomes), tuple(rows), 0)
        rows.append(StageResult(
            Scenario.TEST, Stage.SESSION, Outcome.PASS, opened.status,
            opened.media, opened.timing, Observation.SESSION_ACCEPTED,
        ))
        outcomes.append(Overall.PASS)
    finally:
        opened.close()

    if not send:
        rows.append(StageResult(
            Scenario.TEST, Stage.SEND, Outcome.NOT_RUN, None, Media.NONE,
            Timing.NONE, Observation.SEND_DISABLED,
        ))
        return Report("authenticated", _combine_overall(outcomes), tuple(rows), 0)

    request_id = request_id_factory()
    message = message_factory()
    body = json.dumps(
        {
            "data": {
                "request_id": request_id,
                "message": {"text": message},
                "rant": None,
                "channel_id": None,
            }
        },
        separators=(",", ":"),
    ).encode("utf-8")
    send_url = f"{endpoints.chat_base}/chat/api/chat/{resolved.stream_id}/message"
    try:
        opened = transport.open(
            "POST",
            send_url,
            allowed_hosts=endpoints.hosts_for(endpoints.chat_base),
            headers={
                "Accept": "application/json, text/plain, */*",
                "Content-Type": "application/json",
                "Origin": "https://rumble.com",
                "Cookie": cookie_header,
            },
            body=body,
            max_redirects=0,
        )
    except ValidationException as exc:
        if exc.ambiguous_post:
            row = StageResult(
                Scenario.TEST, Stage.SEND, Outcome.INPUT_REMOTE, None,
                Media.NONE,
                Timing.TIMEOUT if exc.kind == FailureKind.TIMEOUT else Timing.NONE,
                Observation.SEND_AMBIGUOUS,
            )
            overall = Overall.INCONCLUSIVE_INPUT_OR_REMOTE
        else:
            row, overall = _failure_result(Scenario.TEST, Stage.SEND, exc)
        rows.append(row)
        outcomes.append(overall)
        return Report("authenticated", _combine_overall(outcomes), tuple(rows), 1)
    try:
        if opened.status != 200:
            ambiguous_status = opened.status == 408 or opened.status >= 500
            observation = (
                Observation.SEND_AMBIGUOUS
                if ambiguous_status
                else Observation.RETRY_AFTER
                if opened.status == 429 and opened.retry_after_present
                else Observation.SEND_REJECTED
            )
            rows.append(StageResult(
                Scenario.TEST, Stage.SEND, Outcome.INPUT_REMOTE,
                opened.status, opened.media, opened.timing, observation,
            ))
            outcomes.append(Overall.INCONCLUSIVE_INPUT_OR_REMOTE)
            return Report("authenticated", _combine_overall(outcomes), tuple(rows), 1)
        if opened.media != Media.JSON:
            rows.append(StageResult(
                Scenario.TEST, Stage.SEND, Outcome.MISMATCH, opened.status,
                opened.media, opened.timing, Observation.SEND_ACK_MISMATCH,
            ))
            outcomes.append(Overall.CONTRACT_MISMATCH)
            return Report("authenticated", _combine_overall(outcomes), tuple(rows), 1)
        try:
            document = _read_json_object(transport, opened)
        except ValidationException as exc:
            row = StageResult(
                Scenario.TEST,
                Stage.SEND,
                Outcome.MISMATCH,
                opened.status,
                opened.media,
                Timing.TIMEOUT if exc.kind == FailureKind.TIMEOUT else opened.timing,
                Observation.SEND_ACK_MISMATCH,
            )
            overall = Overall.CONTRACT_MISMATCH
            rows.append(row)
            outcomes.append(overall)
            return Report("authenticated", _combine_overall(outcomes), tuple(rows), 1)
        if not _send_has_stable_id(document):
            rows.append(StageResult(
                Scenario.TEST, Stage.SEND, Outcome.MISMATCH, opened.status,
                opened.media, opened.timing, Observation.SEND_ACK_MISMATCH,
            ))
            outcomes.append(Overall.CONTRACT_MISMATCH)
            return Report("authenticated", _combine_overall(outcomes), tuple(rows), 1)
        rows.append(StageResult(
            Scenario.TEST, Stage.SEND, Outcome.PASS, opened.status,
            opened.media, opened.timing, Observation.SEND_ACCEPTED,
        ))
        outcomes.append(Overall.PASS)
        return Report("authenticated", _combine_overall(outcomes), tuple(rows), 1)
    finally:
        opened.close()


def render_report(report: Report, *, observed_date: Optional[dt.date] = None) -> str:
    date = observed_date or dt.datetime.now(dt.timezone.utc).date()
    if report.kind == "public":
        marker = "chatterino-rumble-public-validation"
        title = "Rumble anonymous protocol validation"
    else:
        marker = "chatterino-rumble-authenticated-validation"
        title = "Rumble authenticated protocol validation"

    lines = [
        f"<!-- {marker} schema={SCHEMA} -->",
        f"## {title}",
        "",
        f"- Validator version: `{VERSION}`",
        f"- Validator schema: `{SCHEMA}`",
        f"- Observation date (UTC): `{date.isoformat()}`",
        f"- Overall: **{report.overall.value}**",
        "- Sensitive values included: **no**",
    ]
    if report.kind == "public":
        lines.append("- Credentials/cookies sent: **no**")
    else:
        lines.extend([
            "- Session ingress tested: **interactive bearer-session import only**",
            f"- Mutation attempts: **{report.send_attempts}**",
        ])
    lines.extend([
        "",
        "| Scenario | Stage | Outcome | HTTP | Media shape | Timing | Observation |",
        "|---|---|---|---:|---|---|---|",
    ])
    for row in report.rows:
        status = str(row.status) if row.status is not None else "none"
        lines.append(
            f"| {row.scenario.value} | {row.stage.value} | {row.outcome.value} | "
            f"{status} | {row.media.value} | {row.timing.value} | "
            f"{row.observation.value} |"
        )
    token_kind = "PUBLIC" if report.kind == "public" else "AUTHENTICATED"
    lines.extend([
        "",
        f"Result token: `RUMBLE_{token_kind}_VALIDATION_V{SCHEMA}={report.overall.value}`",
    ])
    if report.kind == "authenticated":
        lines.extend([
            "",
            "### Not established by this run",
            "",
            "- Official OAuth or supported session acquisition",
            "- Session rotation, expiry duration, logout, or revocation",
            "- Whether Origin/CSRF fields are necessary rather than sufficient",
            "- Formal rate limits or deliberately induced production failures",
            "- Visible exactly-once delivery beyond the server acknowledgement",
        ])
    return "\n".join(lines) + "\n"


def render_diagnostic_report(
    report: DiagnosticReport,
    *,
    observed_date: Optional[dt.date] = None,
) -> str:
    """Render only local constants and closed-vocabulary diagnostic values."""
    date = observed_date or dt.datetime.now(dt.timezone.utc).date()
    return "\n".join([
        f"<!-- chatterino-rumble-diagnostic schema={SCHEMA} -->",
        "## Rumble resolver diagnostic",
        "",
        f"- Validator version: `{VERSION}`",
        f"- Validator schema: `{SCHEMA}`",
        f"- Observation date (UTC): `{date.isoformat()}`",
        f"- Overall: **{report.overall.value}**",
        f"- Stage: `{report.stage.value}`",
        f"- Reason: `{report.reason.value}`",
        "- Sensitive values included: **no**",
        "- Credentials/cookies sent: **no**",
        "",
        f"Result token: `RUMBLE_DIAGNOSTIC_V{SCHEMA}={report.overall.value}`",
        "",
    ])


def exit_code(overall: Overall) -> int:
    return {
        Overall.PASS: 0,
        Overall.INCONCLUSIVE_ENVIRONMENT: 10,
        Overall.INCONCLUSIVE_INPUT_OR_REMOTE: 11,
        Overall.CONTRACT_MISMATCH: 12,
        Overall.INTERNAL_ERROR: 70,
    }[overall]


class SafeArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        raise UsageException()


def _bounded_timeout(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError:
        raise argparse.ArgumentTypeError("invalid") from None
    if not 1 <= parsed <= 30:
        raise argparse.ArgumentTypeError("invalid")
    return parsed


def build_parser() -> SafeArgumentParser:
    parser = SafeArgumentParser(
        prog="rumble-validation",
        description="Emit a sanitized Rumble protocol-validation report.",
    )
    parser.add_argument("--version", action="version", version=VERSION)
    subparsers = parser.add_subparsers(dest="command", required=True)

    public = subparsers.add_parser(
        "public",
        help="validate public live/offline resolution and anonymous SSE",
    )
    public.add_argument("--live-channel", required=True, metavar="SLUG_OR_URL")
    public.add_argument("--offline-channel", required=True, metavar="SLUG_OR_URL")
    public.add_argument("--request-timeout", type=_bounded_timeout, default=20,
                        metavar="SECONDS")

    diagnose = subparsers.add_parser(
        "diagnose",
        help="diagnose one public channel through the page/embed contract",
    )
    diagnose.add_argument("--channel", required=True, metavar="SLUG_OR_URL")
    diagnose.add_argument("--request-timeout", type=_bounded_timeout, default=20,
                          metavar="SECONDS")

    authenticated = subparsers.add_parser(
        "authenticated",
        help="validate an interactive session and optional one-shot send",
    )
    authenticated.add_argument("--channel", required=True, metavar="SLUG_OR_URL")
    authenticated.add_argument(
        "--send",
        action="store_true",
        help="confirm and perform exactly one generated benign test send",
    )
    authenticated.add_argument("--request-timeout", type=_bounded_timeout, default=20,
                               metavar="SECONDS")
    return parser


def _read_session_from_tty(send: bool) -> str:
    if not sys.stdin.isatty():
        raise UsageException()
    if send:
        sys.stderr.write(
            "This will post one generated benign message to the approved test channel.\n"
            "Type SEND ONCE to continue: "
        )
        sys.stderr.flush()
        confirmation = sys.stdin.readline().rstrip("\r\n")
        if confirmation != "SEND ONCE":
            raise UsageException()
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("error", getpass.GetPassWarning)
            session = getpass.getpass(
                "Rumble u_s session (hidden; never stored or reported): "
            )
    except (EOFError, getpass.GetPassWarning):
        raise UsageException() from None
    encoded = session.encode("utf-8", "strict")
    if not encoded or len(encoded) > MAX_SESSION_BYTES:
        raise UsageException()
    if any(byte < 0x21 or byte > 0x7E or byte in {0x3B} for byte in encoded):
        raise UsageException()
    return session


def _internal_report(kind: str) -> Report:
    return Report(
        kind,
        Overall.INTERNAL_ERROR,
        (
            StageResult(
                Scenario.TEST,
                Stage.SESSION if kind == "authenticated" else Stage.PAGE,
                Outcome.MISMATCH,
                None,
                Media.NONE,
                Timing.NONE,
                Observation.INTERNAL,
            ),
        ),
    )


def _internal_diagnostic_report() -> DiagnosticReport:
    return DiagnosticReport(
        Overall.INTERNAL_ERROR,
        DiagnosticStage.COMPLETE,
        DiagnosticReason.INTERNAL,
    )


def main(argv: Optional[list[str]] = None) -> int:
    parser = build_parser()
    report_kind = "public"
    try:
        args = parser.parse_args(argv)
        if args.command == "public":
            live_slug = parse_channel_locator(args.live_channel)
            offline_slug = parse_channel_locator(args.offline_channel)
            report = validate_public(
                live_slug,
                offline_slug,
                timeout=args.request_timeout,
            )
        elif args.command == "diagnose":
            report_kind = "diagnose"
            slug = parse_channel_locator(args.channel)
            report = diagnose_channel(slug, timeout=args.request_timeout)
        else:
            report_kind = "authenticated"
            slug = parse_channel_locator(args.channel)
            session = _read_session_from_tty(args.send)
            try:
                report = validate_authenticated(
                    slug,
                    session,
                    send=args.send,
                    timeout=args.request_timeout,
                )
            finally:
                session = ""
    except UsageException:
        sys.stderr.write("Validation arguments or interactive confirmation were rejected; use --help.\n")
        return 2
    except Exception:
        report = (
            _internal_diagnostic_report()
            if report_kind == "diagnose"
            else _internal_report(report_kind)
        )

    if isinstance(report, DiagnosticReport):
        sys.stdout.write(render_diagnostic_report(report))
    else:
        sys.stdout.write(render_report(report))
    return exit_code(report.overall)


if __name__ == "__main__":
    raise SystemExit(main())
