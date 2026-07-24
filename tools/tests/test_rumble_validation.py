from __future__ import annotations

import contextlib
import http.server
import io
import json
import socket
import threading
import time
import unittest
import unittest.mock
import urllib.parse
from typing import Any, Callable, Optional

from tools import rumble_validation as rv


Route = tuple[int, dict[str, str], bytes]


class FixtureState:
    def __init__(self) -> None:
        self.routes: dict[tuple[str, str], Route | Callable[[Any], Optional[Route]]] = {}
        self.requests: list[dict[str, Any]] = []

    def route(self, method: str, path: str, response: Route | Callable[[Any], Optional[Route]]) -> None:
        self.routes[(method, path)] = response


class FixtureHandler(http.server.BaseHTTPRequestHandler):
    server_version = "Fixture"
    protocol_version = "HTTP/1.1"

    @property
    def state(self) -> FixtureState:
        return self.server.fixture_state  # type: ignore[attr-defined]

    def log_message(self, format: str, *args: Any) -> None:
        return

    def do_GET(self) -> None:
        self._handle("GET")

    def do_POST(self) -> None:
        self._handle("POST")

    def _handle(self, method: str) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length) if length else b""
        parsed = urllib.parse.urlsplit(self.path)
        record = {
            "method": method,
            "path": parsed.path,
            "query": parsed.query,
            "headers": dict(self.headers.items()),
            "body": body,
        }
        self.state.requests.append(record)
        route = self.state.routes.get((method, parsed.path))
        if route is None:
            response: Optional[Route] = (404, {"Content-Type": "text/plain"}, b"missing")
        elif callable(route):
            response = route(record)
        else:
            response = route
        if response is None:
            with contextlib.suppress(OSError):
                self.connection.shutdown(socket.SHUT_RDWR)
            self.connection.close()
            return
        status, headers, response_body = response
        self.send_response(status)
        for name, value in headers.items():
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(response_body)))
        self.end_headers()
        if response_body:
            with contextlib.suppress(BrokenPipeError, ConnectionResetError):
                self.wfile.write(response_body)


class FixtureServer(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self) -> None:
        super().__init__(("127.0.0.1", 0), FixtureHandler)
        self.fixture_state = FixtureState()
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)

    @property
    def base_url(self) -> str:
        return f"http://127.0.0.1:{self.server_port}"

    def __enter__(self) -> "FixtureServer":
        self.thread.start()
        return self

    def __exit__(self, *args: Any) -> None:
        self.shutdown()
        self.server_close()
        self.thread.join(timeout=2)


def endpoints(server: FixtureServer) -> rv.Endpoints:
    return rv.Endpoints(
        page_base=server.base_url,
        embed_base=server.base_url,
        chat_base=server.base_url,
        service_base=server.base_url,
        allow_http_for_tests=True,
    )


def html_with_embed(embed_id: str, *, duration: int = 0) -> bytes:
    return (
        f'<html><title>Fixture channel</title>'
        f'<div class="videostream" duration="{duration}">'
        f'<iframe src="https://rumble.com/embed/{embed_id}/"></iframe>'
        f'</div></html>'
    ).encode()


def init_event(*message_ids: str) -> bytes:
    messages = [
        {
            "id": message_id,
            "user_id": "REMOTE_USER_ID_CANARY",
            "text": "REMOTE_MESSAGE_CANARY",
            "time": "2026-01-01T00:00:00Z",
        }
        for message_id in message_ids
    ]
    payload = {
        "type": "init",
        "data": {
            "users": [{
                "id": "REMOTE_USER_ID_CANARY",
                "username": "REMOTE_USERNAME_CANARY",
            }],
            "channels": [{"id": "REMOTE_CHANNEL_ID_CANARY"}],
            "config": {"badges": {}, "message_length_max": 200},
            "messages": messages,
        },
    }
    return b": heartbeat\n\ndata: " + json.dumps(payload).encode() + b"\n\n"


def opened_response(body: bytes, *, media: rv.Media = rv.Media.EVENT_STREAM,
                    timeout: float = 1) -> rv.OpenedResponse:
    return rv.OpenedResponse(
        response=io.BytesIO(body),
        status=200,
        media=media,
        timing=rv.Timing.UNDER_1,
        retry_after_present=False,
        deadline=time.monotonic() + timeout,
    )


def install_public_success(
    server: FixtureServer,
    *,
    live_slug: str = "LIVE_LOCATOR_CANARY",
    offline_slug: str = "OFFLINE_LOCATOR_CANARY",
    offline_at_sse: bool = False,
) -> None:
    state = server.fixture_state
    state.route(
        "GET",
        f"/c/{live_slug}/live/",
        (200, {"Content-Type": "text/html; charset=utf-8"}, html_with_embed("vlivesecret123")),
    )
    state.route(
        "GET",
        f"/c/{offline_slug}/live/",
        (
            200,
            {"Content-Type": "text/html"},
            html_with_embed(
                "vofflinesecret456",
                duration=0 if offline_at_sse else 3600,
            ),
        ),
    )

    def embed(record: dict[str, Any]) -> Route:
        query = urllib.parse.parse_qs(record["query"])
        embed_id = query.get("v", [""])[0]
        stream_id = 777777 if "live" in embed_id and "offline" not in embed_id else 888888
        return (
            200,
            {"Content-Type": "application/json"},
            json.dumps({"vid": stream_id, "title": "Fixture stream"}).encode(),
        )

    state.route("GET", "/embedJS/u3/", embed)
    def live_sse(record: dict[str, Any]) -> Route:
        headers = record["headers"]
        if (
            headers.get("Origin") != "https://rumble.com"
            or headers.get("Referer") != "https://rumble.com/"
            or "Cookie" in headers
            or "Authorization" in headers
        ):
            return (200, {"Content-Type": "text/plain"}, b"context rejected")
        return (
            200,
            {
                "Content-Type": (
                    "text/event-stream; charset=utf-8; charset=UTF-8"
                ),
            },
            init_event("REMOTE_MESSAGE_ID_CANARY"),
        )

    state.route("GET", "/chat/api/chat/777777/stream", live_sse)
    if offline_at_sse:
        def offline_sse(record: dict[str, Any]) -> Route:
            headers = record["headers"]
            if (
                headers.get("Origin") != "https://rumble.com"
                or headers.get("Referer") != "https://rumble.com/"
                or "Cookie" in headers
                or "Authorization" in headers
            ):
                return (200, {"Content-Type": "text/plain"}, b"context rejected")
            return (204, {"Content-Type": "text/event-stream"}, b"")

        state.route("GET", "/chat/api/chat/888888/stream", offline_sse)


class LocatorTests(unittest.TestCase):
    def test_accepts_slug_and_narrow_public_urls(self) -> None:
        self.assertEqual(rv.parse_channel_locator(" approved-channel "), "approved-channel")
        self.assertEqual(
            rv.parse_channel_locator("https://rumble.com/c/approved-channel/live/"),
            "approved-channel",
        )
        self.assertEqual(
            rv.parse_channel_locator("https://www.rumble.com/user/approved-channel"),
            "approved-channel",
        )
        self.assertEqual(
            rv.parse_channel_locator(
                "https://rumble.com/C/approved-channel/LIVE/?ignored=yes#ignored"
            ),
            "approved-channel",
        )

    def test_rejects_secret_or_arbitrary_url_forms(self) -> None:
        rejected = [
            "http://rumble.com/c/channel",
            "https://evil.example/c/channel",
            "https://rumble.com:443/c/channel",
            "https://rumble.com:/c/channel",
            "https://user:pass@rumble.com/c/channel",
            "https://rumble.com/account/livestream-api",
            "space containing slug",
            "channel.with.dot",
            "\x1capproved-channel\x1c",
            "a" * 81,
        ]
        for value in rejected:
            with self.subTest(value=value), self.assertRaises(rv.UsageException):
                rv.parse_channel_locator(value)

    def test_extracts_only_production_page_embed_reference_forms(self) -> None:
        cases = {
            b'<html><title>Iframe</title><iframe src="https://rumble.com/embed/vabc123/"></iframe></html>': "vabc123",
            b'<html><title>JSON</title><script type="application/json">'
            b'{"embedUrl":"https://rumble.com/embed/vserialized321/"}'
            b'</script></html>': "vserialized321",
        }
        for body, expected in cases.items():
            with self.subTest(expected=expected):
                self.assertEqual(rv.extract_embed_id(body), expected)

        decoys = (
            b'<html><title>Query</title><a href="/watch?v=vquery456"></a></html>',
            b'<html><title>Text</title>path=%2Fembed%2Fvpercent789</html>',
            b'<html><title>Untyped</title><script>'
            b'{"embedUrl":"https://rumble.com/embed/vscript123/"}'
            b'</script></html>',
        )
        for body in decoys:
            with self.subTest(body=body):
                self.assertIsNone(rv.extract_embed_id(body))


class ResolverContractTests(unittest.TestCase):
    def test_first_video_duration_is_authoritative_for_channel_status(self) -> None:
        live = rv.parse_page_contract(
            b'<html><title>Live channel</title>'
            b'<div class="tile videostream" duration="0">'
            b'<iframe src="https://rumble.com/embed/vcurrent/"></iframe>'
            b'</div></html>'
        )
        self.assertEqual(live.state, rv.PageState.LIVE)
        self.assertTrue(live.first_video_live)
        self.assertEqual(live.embed_id, "vcurrent")

        offline = rv.parse_page_contract(
            b'<html><title>Offline channel</title>'
            b'<div class="videostream tile" duration="3600">'
            b'<iframe src="https://rumble.com/embed/vstale/"></iframe></div>'
            b'<div class="videostream" duration="0">'
            b'<iframe src="https://rumble.com/embed/vdecoy/"></iframe></div>'
            b'</html>'
        )
        self.assertEqual(offline.state, rv.PageState.OFFLINE)
        self.assertFalse(offline.first_video_live)
        self.assertIsNone(offline.embed_id)

        malformed = (
            b'<html><title>Missing duration</title>'
            b'<div class="videostream"></div></html>',
            b'<html><title>Conflicting duration</title>'
            b'<div class="videostream" duration="0" duration="1">'
            b'<iframe src="https://rumble.com/embed/vbad/"></iframe>'
            b'</div></html>',
            b'<html><title>Wrong duration attribute</title>'
            b'<div class="videostream" data-duration="0">'
            b'<iframe src="https://rumble.com/embed/vbad/"></iframe>'
            b'</div></html>',
            b'<html><title>Invalid duration</title>'
            b'<div class="videostream" duration="00">'
            b'<iframe src="https://rumble.com/embed/vbad/"></iframe>'
            b'</div></html>',
            b'<html><title>Live without target</title>'
            b'<div class="videostream" duration="0"></div></html>',
        )
        for body in malformed:
            with self.subTest(body=body):
                self.assertEqual(
                    rv.parse_page_contract(body).state,
                    rv.PageState.MALFORMED,
                )

    def test_page_requires_one_title_and_one_distinct_embed(self) -> None:
        missing_title = rv.parse_page_contract(
            b'<html><iframe src="https://rumble.com/embed/vone/"></iframe></html>'
        )
        self.assertEqual(missing_title.state, rv.PageState.MALFORMED)
        self.assertEqual(missing_title.reason, rv.PageReason.TITLE_MISSING)

        ambiguous = rv.parse_page_contract(
            b'<html><title>Channel</title>'
            b'<iframe src="https://rumble.com/embed/vone/"></iframe>'
            b'<iframe src="https://rumble.com/embed/vtwo/"></iframe></html>'
        )
        self.assertEqual(ambiguous.state, rv.PageState.MALFORMED)
        self.assertEqual(ambiguous.reason, rv.PageReason.EMBED_AMBIGUOUS)

        duplicate_same_id = rv.parse_page_contract(
            b'<html><title>Channel</title>'
            b'<iframe src="https://rumble.com/embed/vone/"></iframe>'
            b'<script type="application/json">'
            b'{"videoUrl":"https://rumble.com/embed/vone/"}'
            b'</script></html>'
        )
        self.assertEqual(duplicate_same_id.state, rv.PageState.LIVE)

        production_malformed = (
            b'<html><title>Unterminated comment</title><!-- unfinished',
            b'<html><title/>Ignored close</title>'
            b'<iframe src="https://rumble.com/embed/vone/"></iframe></html>',
            b'<html><title>Self-closing script</title>'
            b'<script type="application/json"/>'
            b'{"embedUrl":"https://rumble.com/embed/vone/"}'
            b'</script></html>',
            b'<html><template><div broken=></div></template>'
            b'<title>Malformed inert markup</title>'
            b'<iframe src="https://rumble.com/embed/vone/"></iframe></html>',
            b'<html><title>Closing junk</title>'
            b'<iframe src="https://rumble.com/embed/vone/"></iframe junk>'
            b'</html>',
            b'<html><title>Overlong tag</title><iframe '
            + b'a' * (rv.MAX_HTML_TAG_CHARS + 1)
            + b' src="https://rumble.com/embed/vone/"></iframe></html>',
        )
        for body in production_malformed:
            with self.subTest(body=body):
                parsed = rv.parse_page_contract(body)
                self.assertEqual(parsed.state, rv.PageState.MALFORMED)
                self.assertEqual(parsed.reason, rv.PageReason.SCHEMA)

        template_raw_child = rv.parse_page_contract(
            b'<html><template><noscript></template>'
            b'<title>Still inert</title>'
            b'<iframe src="https://rumble.com/embed/vbad/"></iframe>'
            b'</noscript></template></html>'
        )
        self.assertEqual(template_raw_child.state, rv.PageState.MALFORMED)
        self.assertEqual(template_raw_child.reason, rv.PageReason.TITLE_MISSING)

        template_title = rv.parse_page_contract(
            b'<html><template><title>Inert title</title></template>'
            b'<iframe src="https://rumble.com/embed/vbad/"></iframe></html>'
        )
        self.assertEqual(template_title.state, rv.PageState.MALFORMED)
        self.assertEqual(template_title.reason, rv.PageReason.TITLE_MISSING)

        template_script = rv.parse_page_contract(
            b'<html><title>Outer title</title>'
            b'<script type="application/json">{}</script>'
            b'<template><script>'
            b'{"embedUrl":"https://rumble.com/embed/vbad/"}'
            b'</script></template></html>'
        )
        self.assertEqual(template_script.state, rv.PageState.OFFLINE)

        encoded_attribute = rv.parse_page_contract(
            b'<html><title>Encoded attribute stays literal</title>'
            b'<iframe src="https://rumble.com&#x2f;embed&#x2f;vone/">'
            b'</iframe></html>'
        )
        self.assertEqual(encoded_attribute.state, rv.PageState.OFFLINE)

        uppercase_id = rv.parse_page_contract(
            b'<html><title>Uppercase ID is not canonical</title>'
            b'<iframe src="https://rumble.com/embed/vUPPER/">'
            b'</iframe></html>'
        )
        self.assertEqual(uppercase_id.state, rv.PageState.OFFLINE)

    def test_page_tolerates_bounded_unrelated_hydration_markup(self) -> None:
        padding = "x" * (rv.MAX_HTML_TAG_CHARS + 1)
        current_shape = (
            '<html><head><title>Current channel shape</title></head><body>'
            '<div data-hydration="' + padding
            + " &lt;iframe src='https://rumble.com/embed/vdecoy/'&gt;"
            + '" data-layout="first" data-layout="second"></div>'
            '<script type="application/ld+json">'
            '{"video":{"embedUrl":"https://rumble.com/embed/vcurrent/"}}'
            '</script></body></html>'
        ).encode()
        parsed = rv.parse_page_contract(current_shape)
        self.assertEqual(parsed.state, rv.PageState.LIVE)
        self.assertEqual(parsed.embed_id, "vcurrent")

        decoy_only = (
            '<html><title>Unrelated attribute stays inert</title>'
            '<div data-hydration="' + padding
            + " <iframe src='https://rumble.com/embed/vdecoy/'>"
            + '"></div></html>'
        ).encode()
        self.assertEqual(
            rv.parse_page_contract(decoy_only).state,
            rv.PageState.OFFLINE,
        )

        unrelated_closer = rv.parse_page_contract(
            b'<html><title>Quoted unrelated closer</title>'
            b'</div data=">">'
            b'<iframe src="https://rumble.com/embed/vcurrent/"></iframe>'
            b'</html>'
        )
        self.assertEqual(unrelated_closer.state, rv.PageState.LIVE)
        self.assertEqual(unrelated_closer.embed_id, "vcurrent")

        contract_near_misses = (
            b'<html><title>Contract prefix is not unrelated</title>'
            b'<iframe@x></iframe>'
            b'<iframe src="https://rumble.com/embed/vbad/"></iframe></html>',
            (
                '<html><title>Oversized iframe</title><iframe data-padding="'
                + padding
                + '" src="https://rumble.com/embed/vbad/"></iframe></html>'
            ).encode(),
            b'<html><title>Duplicate iframe source</title>'
            b'<iframe src="https://rumble.com/embed/vone/" '
            b'src="https://rumble.com/embed/vtwo/"></iframe></html>',
            (
                '<html><title>Oversized typed script</title>'
                '<script type="application/json" data-padding="' + padding
                + '">{"embedUrl":"https://rumble.com/embed/vbad/"}'
                '</script></html>'
            ).encode(),
            (
                '<html><title>Oversized challenge form</title>'
                '<form data-padding="' + padding
                + '" action="/cdn-cgi/challenge"></form></html>'
            ).encode(),
            b'<html><title>Unterminated unrelated closer</title>'
            b'</div data="unterminated>'
            b"<iframe src='https://rumble.com/embed/vbad/'></iframe></html>",
            b'<html><title>Nameless closing construct</title>'
            b'</ data="><iframe '
            b"src='https://rumble.com/embed/vbad/'></iframe>\"></html>",
            (
                '<html><template><div data-hydration="' + padding
                + '"></div></template><title>Strict inert scope</title>'
                '<iframe src="https://rumble.com/embed/vbad/"></iframe></html>'
            ).encode(),
            b'<html><template></div junk></template>'
            b'<title>Strict inert close</title>'
            b'<iframe src="https://rumble.com/embed/vbad/"></iframe></html>',
        )
        for body in contract_near_misses:
            with self.subTest(body_length=len(body)):
                rejected = rv.parse_page_contract(body)
                self.assertEqual(rejected.state, rv.PageState.MALFORMED)
                self.assertEqual(rejected.reason, rv.PageReason.SCHEMA)

    def test_body_and_svg_titles_do_not_ambiguate_document_title(self) -> None:
        padding = "x" * (rv.MAX_HTML_TAG_CHARS + 1)
        offline = rv.parse_page_contract(
            (
                '<html><head><title>Offline channel</title></head>'
                '<body data-hydration="' + padding + '">'
                '<svg><title>Decorative channel icon</title></svg>'
                '<title data-label="' + padding + '">Body fallback</title>'
                '</body></html>'
            ).encode()
        )
        self.assertEqual(offline.state, rv.PageState.OFFLINE)

        live = rv.parse_page_contract(
            b'<html><head><title>Live channel</title></head><body>'
            b'<svg><title>Decorative player icon</title></svg>'
            b'<script type="application/json">'
            b'{"embedUrl":"https://rumble.com/embed/vcurrent/"}'
            b'</script></body></html>'
        )
        self.assertEqual(live.state, rv.PageState.LIVE)
        self.assertEqual(live.embed_id, "vcurrent")

        body_title_only = rv.parse_page_contract(
            b'<html><body><svg><title>Not a document title</title></svg>'
            b'</body></html>'
        )
        self.assertEqual(body_title_only.state, rv.PageState.MALFORMED)
        self.assertEqual(body_title_only.reason, rv.PageReason.TITLE_MISSING)

        multiple_document_titles = rv.parse_page_contract(
            b'<html><head><title>First</title><title>Second</title></head>'
            b'<body><svg><title>Ignored</title></svg></body></html>'
        )
        self.assertEqual(
            multiple_document_titles.state,
            rv.PageState.MALFORMED,
        )
        self.assertEqual(
            multiple_document_titles.reason,
            rv.PageReason.TITLE_AMBIGUOUS,
        )

        unterminated_body_title = rv.parse_page_contract(
            b'<html><head><title>Document</title></head><body>'
            b'<svg><title>Unterminated</svg></body></html>'
        )
        self.assertEqual(
            unterminated_body_title.state,
            rv.PageState.MALFORMED,
        )
        self.assertEqual(unterminated_body_title.reason, rv.PageReason.SCHEMA)

    def test_typed_page_json_enforces_production_depth_and_key_limits(self) -> None:
        embed = "https://rumble.com/embed/vbounded/"

        allowed_nested: Any = {"leaf": True}
        for _ in range(rv.MAX_PAGE_JSON_DEPTH - 2):
            allowed_nested = [allowed_nested]
        accepted = rv.parse_page_contract(
            (
                '<html><title>Allowed bounds</title>'
                '<script type="application/json">'
                + json.dumps({"embedUrl": embed, "future": allowed_nested})
                + '</script></html>'
            ).encode()
        )
        self.assertEqual(accepted.state, rv.PageState.LIVE)

        overdeep: Any = {"leaf": True}
        for _ in range(rv.MAX_PAGE_JSON_DEPTH - 1):
            overdeep = [overdeep]
        rejected_depth = rv.parse_page_contract(
            (
                '<html><title>Rejected depth</title>'
                '<script type="application/json">'
                + json.dumps({"embedUrl": embed, "future": overdeep})
                + '</script></html>'
            ).encode()
        )
        self.assertEqual(rejected_depth.state, rv.PageState.OFFLINE)

        recursive_json = (
            b'{"embedUrl":"https://rumble.com/embed/vrecursive/","future":'
            + (b'[' * 1100)
            + b'0'
            + (b']' * 1100)
            + b'}'
        )
        recursive_page = rv.parse_page_contract(
            b'<html><title>Recursive JSON</title>'
            b'<script type="application/json">'
            + recursive_json
            + b'</script></html>'
        )
        self.assertEqual(recursive_page.state, rv.PageState.OFFLINE)

        for invalid_json in (
            b'{"embedUrl":"https://rumble.com/embed/vbad/","future":1e999999}',
            b'{"embedUrl":"https://rumble.com/embed/vbad/","future":"\\ud800"}',
        ):
            with self.subTest(invalid_json=invalid_json):
                parsed = rv.parse_page_contract(
                    b'<html><title>Invalid typed JSON</title>'
                    b'<script type="application/json">'
                    + invalid_json
                    + b'</script></html>'
                )
                self.assertEqual(parsed.state, rv.PageState.OFFLINE)

        allowed_keys = {
            **{f"k{index}": index for index in range(rv.MAX_PAGE_JSON_KEYS - 1)},
            "embedUrl": embed,
        }
        self.assertEqual(
            rv.parse_page_contract(
                ('<html><title>Allowed keys</title><script type="application/json">'
                 + json.dumps(allowed_keys) + '</script></html>').encode()
            ).state,
            rv.PageState.LIVE,
        )
        rejected_keys = dict(allowed_keys)
        rejected_keys["oneTooMany"] = True
        self.assertEqual(
            rv.parse_page_contract(
                ('<html><title>Rejected keys</title><script type="application/json">'
                 + json.dumps(rejected_keys) + '</script></html>').encode()
            ).state,
            rv.PageState.OFFLINE,
        )

    def test_qstring_limits_count_utf16_code_units(self) -> None:
        page = rv.parse_page_contract(
            (
                '<html><title>' + ('\U0001f642' * 3000) + '</title>'
                '<iframe src="https://rumble.com/embed/vone/"></iframe></html>'
            ).encode()
        )
        self.assertEqual(page.state, rv.PageState.MALFORMED)

        embed = rv.parse_embed_contract(
            json.dumps({"vid": 777, "title": "\U0001f642" * 3000}).encode()
        )
        self.assertEqual(embed.state, rv.EmbedState.MALFORMED)
        self.assertEqual(embed.reason, rv.EmbedReason.TITLE_MISSING)

    def test_page_interstitial_is_not_schema_or_offline(self) -> None:
        for body in (
            b'<html><title>Just a moment...</title></html>',
            b'<html><title>Rumble</title>'
            b'<form action="/cdn-cgi/challenge"></form></html>',
        ):
            with self.subTest(body=body):
                parsed = rv.parse_page_contract(body)
                self.assertEqual(parsed.state, rv.PageState.INTERSTITIAL)
                self.assertEqual(parsed.reason, rv.PageReason.INTERSTITIAL)

    def test_embed_live_requires_title_and_validates_optional_metadata(self) -> None:
        accepted = rv.parse_embed_contract(
            b'{"vid":777,"title":"Live",'
            b'"channel_id":"123","channel_title":"Channel",'
            b'"future":{"duplicate":1,"duplicate":2}}'
        )
        self.assertEqual(accepted.state, rv.EmbedState.LIVE)
        surrogate_metadata = rv.parse_embed_contract(
            b'{"vid":777,"title":"Live","future":"\\ud800"}'
        )
        self.assertEqual(surrogate_metadata.state, rv.EmbedState.LIVE)
        unknown_escape_metadata = rv.parse_embed_contract(
            b'{"vid":777,"title":"Live","future":"\\q"}'
        )
        self.assertEqual(unknown_escape_metadata.state, rv.EmbedState.LIVE)
        qt_nonspace_title = rv.parse_embed_contract(
            b'{"vid":777,"title":"\\u001c"}'
        )
        self.assertEqual(qt_nonspace_title.state, rv.EmbedState.LIVE)
        for permissive_metadata in (
            b'{"vid":777,"title":"Live","future":"raw\x01control"}',
            b'{"vid":777,"title":"Live","future":"escaped\\\x00control"}',
            b'{"vid":777,"title":"Live","future":.1}',
            b'{"vid":777,"title":"Live","future":1.}',
            b'{"vid":777,"title":"Live","future":1.e2}',
            b'{"vid":777,"title":"Live","future":-.1}',
        ):
            with self.subTest(permissive_metadata=permissive_metadata):
                self.assertEqual(
                    rv.parse_embed_contract(permissive_metadata).state,
                    rv.EmbedState.LIVE,
                )

        cases = (
            (b'{"vid":777}', rv.EmbedReason.TITLE_MISSING),
            (b'{"vid":777,"title":" "}', rv.EmbedReason.TITLE_MISSING),
            (b'{"vid":777,"title":"Live","channel_id":1.5}',
             rv.EmbedReason.CHANNEL_ID_INVALID),
            (b'{"vid":777,"title":"Live","channel_title":""}',
             rv.EmbedReason.CHANNEL_TITLE_INVALID),
            (b'{"vid":777,"vid":888,"title":"Live"}',
             rv.EmbedReason.DUPLICATE_KEY),
            (b'{"detail":NaN}', rv.EmbedReason.JSON_SCHEMA),
            (b'{"vid":777,"title":"Live","future":1e999999}',
             rv.EmbedReason.JSON_SCHEMA),
            (b'{"vid":777,"title":"Live","future":"\\\xc3\xa9"}',
             rv.EmbedReason.JSON_SCHEMA),
            (b'{"vid":777,"title":"Live","future":1e}',
             rv.EmbedReason.JSON_SCHEMA),
        )
        for body, reason in cases:
            with self.subTest(reason=reason):
                parsed = rv.parse_embed_contract(body)
                self.assertEqual(parsed.state, rv.EmbedState.MALFORMED)
                self.assertEqual(parsed.reason, reason)

        recursive = (
            b'{"vid":777,"title":"Live","future":'
            + (b'[' * 1100)
            + b'0'
            + (b']' * 1100)
            + b'}'
        )
        parsed = rv.parse_embed_contract(recursive)
        self.assertEqual(parsed.state, rv.EmbedState.MALFORMED)
        self.assertEqual(parsed.reason, rv.EmbedReason.JSON_SCHEMA)

        at_depth_limit = (
            b'{"vid":777,"title":"Live","future":'
            + (b'[' * (rv.MAX_EMBED_JSON_DEPTH - 1))
            + b'0'
            + (b']' * (rv.MAX_EMBED_JSON_DEPTH - 1))
            + b'}'
        )
        self.assertEqual(
            rv.parse_embed_contract(at_depth_limit).state,
            rv.EmbedState.LIVE,
        )
        over_depth_limit = (
            b'{"vid":777,"title":"Live","future":'
            + (b'[' * rv.MAX_EMBED_JSON_DEPTH)
            + b'0'
            + (b']' * rv.MAX_EMBED_JSON_DEPTH)
            + b'}'
        )
        over_depth = rv.parse_embed_contract(over_depth_limit)
        self.assertEqual(over_depth.state, rv.EmbedState.MALFORMED)
        self.assertEqual(over_depth.reason, rv.EmbedReason.JSON_SCHEMA)


class DiagnosticValidationTests(unittest.TestCase):
    def test_live_diagnostic_is_credential_free_and_fully_redacted(self) -> None:
        locator = "DIAGNOSTIC_LOCATOR_CANARY"
        with FixtureServer() as server:
            install_public_success(server, live_slug=locator)
            report = rv.diagnose_channel(
                locator,
                endpoints=endpoints(server),
                timeout=2,
            )
            output = rv.render_diagnostic_report(
                report,
                observed_date=rv.dt.date(2026, 7, 17),
            )

            self.assertEqual(report.overall, rv.Overall.PASS)
            self.assertEqual(report.stage, rv.DiagnosticStage.COMPLETE)
            self.assertEqual(report.reason, rv.DiagnosticReason.LIVE_ACCEPTED)
            self.assertIn("RUMBLE_DIAGNOSTIC_V1=PASS", output)
            for forbidden in (
                locator,
                "vLIVESECRET123",
                "777777",
                "Fixture channel",
                "Fixture stream",
                server.base_url,
            ):
                self.assertNotIn(forbidden, output)
            self.assertTrue(server.fixture_state.requests)
            self.assertTrue(all(
                "Cookie" not in request["headers"]
                for request in server.fixture_state.requests
            ))

    def test_diagnostic_distinguishes_page_failures_without_canaries(self) -> None:
        cases = (
            (
                b'<html>PAGE_TITLE_CANARY'
                b'<iframe src="https://rumble.com/embed/vsecret/"></iframe></html>',
                rv.DiagnosticReason.PAGE_TITLE_MISSING,
            ),
            (
                b'<html><title>PAGE_AMBIGUITY_CANARY</title>'
                b'<iframe src="https://rumble.com/embed/vsecretone/"></iframe>'
                b'<iframe src="https://rumble.com/embed/vsecrettwo/"></iframe></html>',
                rv.DiagnosticReason.PAGE_EMBED_AMBIGUOUS,
            ),
            (
                b'<html><head><title>PAGE_TITLE_ONE_CANARY</title>'
                b'<title>PAGE_TITLE_TWO_CANARY</title></head><body></body></html>',
                rv.DiagnosticReason.PAGE_TITLE_AMBIGUOUS,
            ),
            (
                b'<html><title>Verify you are human PAGE_INTERSTITIAL_CANARY</title>'
                b'</html>',
                rv.DiagnosticReason.PAGE_INTERSTITIAL,
            ),
        )
        for body, expected in cases:
            with self.subTest(expected=expected), FixtureServer() as server:
                server.fixture_state.route(
                    "GET",
                    "/c/DIAGNOSTIC_LOCATOR_CANARY/live/",
                    (200, {"Content-Type": "text/html"}, body),
                )
                report = rv.diagnose_channel(
                    "DIAGNOSTIC_LOCATOR_CANARY",
                    endpoints=endpoints(server),
                    timeout=2,
                )
                output = rv.render_diagnostic_report(report)
                self.assertEqual(report.reason, expected)
                self.assertEqual(report.stage, rv.DiagnosticStage.PAGE_CONTRACT)
                for forbidden in (
                    "DIAGNOSTIC_LOCATOR_CANARY",
                    "PAGE_TITLE_CANARY",
                    "PAGE_TITLE_ONE_CANARY",
                    "PAGE_TITLE_TWO_CANARY",
                    "PAGE_AMBIGUITY_CANARY",
                    "PAGE_INTERSTITIAL_CANARY",
                    "vsecretone",
                    "vsecrettwo",
                    server.base_url,
                ):
                    self.assertNotIn(forbidden, output)

    def test_oversized_404_body_fails_before_legacy_profile_fallback(self) -> None:
        with FixtureServer() as server:
            server.fixture_state.route(
                "GET",
                "/c/DIAGNOSTIC_LOCATOR_CANARY/live/",
                (
                    404,
                    {"Content-Type": "text/html"},
                    b"x" * (rv.MAX_PAGE_BYTES + 1),
                ),
            )
            server.fixture_state.route(
                "GET",
                "/user/DIAGNOSTIC_LOCATOR_CANARY/live/",
                (200, {"Content-Type": "text/html"}, html_with_embed("vvalid")),
            )
            report = rv.diagnose_channel(
                "DIAGNOSTIC_LOCATOR_CANARY",
                endpoints=endpoints(server),
                timeout=2,
            )
            self.assertEqual(report.overall, rv.Overall.CONTRACT_MISMATCH)
            self.assertEqual(report.stage, rv.DiagnosticStage.PAGE_REQUEST)
            self.assertEqual(
                report.reason,
                rv.DiagnosticReason.RESPONSE_TOO_LARGE,
            )
            self.assertFalse(any(
                request["path"].startswith("/user/")
                for request in server.fixture_state.requests
            ))

    def test_wrong_media_precedes_body_drain_for_successful_heads(self) -> None:
        cases = ("page", "embed")
        for stage in cases:
            with self.subTest(stage=stage), FixtureServer() as server:
                install_public_success(server)
                if stage == "page":
                    server.fixture_state.route(
                        "GET",
                        "/c/LIVE_LOCATOR_CANARY/live/",
                        (
                            200,
                            {"Content-Type": "text/plain"},
                            b"x" * (rv.MAX_PAGE_BYTES + 1),
                        ),
                    )
                    expected_stage = rv.DiagnosticStage.PAGE_CONTRACT
                else:
                    server.fixture_state.route(
                        "GET",
                        "/embedJS/u3/",
                        (
                            200,
                            {"Content-Type": "text/plain"},
                            b"x" * (rv.MAX_EMBED_BYTES + 1),
                        ),
                    )
                    expected_stage = rv.DiagnosticStage.EMBED_CONTRACT
                report = rv.diagnose_channel(
                    "LIVE_LOCATOR_CANARY",
                    endpoints=endpoints(server),
                    timeout=2,
                )
                self.assertEqual(report.overall, rv.Overall.CONTRACT_MISMATCH)
                self.assertEqual(report.stage, expected_stage)
                self.assertEqual(report.reason, rv.DiagnosticReason.MEDIA_TYPE)

    def test_non_200_success_status_is_a_contract_mismatch(self) -> None:
        cases = (
            ("page", 201),
            ("page", 204),
            ("embed", 201),
            ("embed", 204),
        )
        for stage, status in cases:
            with self.subTest(stage=stage, status=status), FixtureServer() as server:
                install_public_success(server)
                if stage == "page":
                    server.fixture_state.route(
                        "GET",
                        "/c/LIVE_LOCATOR_CANARY/live/",
                        (
                            status,
                            {"Content-Type": "text/html"},
                            b"" if status == 204 else b"unexpected",
                        ),
                    )
                    expected_stage = rv.DiagnosticStage.PAGE_CONTRACT
                else:
                    server.fixture_state.route(
                        "GET",
                        "/embedJS/u3/",
                        (
                            status,
                            {"Content-Type": "application/json"},
                            b"" if status == 204 else b"{}",
                        ),
                    )
                    expected_stage = rv.DiagnosticStage.EMBED_CONTRACT
                report = rv.diagnose_channel(
                    "LIVE_LOCATOR_CANARY",
                    endpoints=endpoints(server),
                    timeout=2,
                )
                self.assertEqual(report.overall, rv.Overall.CONTRACT_MISMATCH)
                self.assertEqual(report.stage, expected_stage)
                self.assertEqual(
                    report.reason,
                    rv.DiagnosticReason.UNEXPECTED_STATUS,
                )

    def test_diagnostic_distinguishes_embed_title_and_duplicate_key(self) -> None:
        cases = (
            (
                b'{"vid":777,"detail":"EMBED_TITLE_CANARY"}',
                rv.DiagnosticReason.EMBED_TITLE_MISSING,
            ),
            (
                b'{"vid":777,"vid":888,"title":"EMBED_DUPLICATE_CANARY"}',
                rv.DiagnosticReason.EMBED_DUPLICATE_KEY,
            ),
        )
        for body, expected in cases:
            with self.subTest(expected=expected), FixtureServer() as server:
                install_public_success(server)
                server.fixture_state.route(
                    "GET",
                    "/embedJS/u3/",
                    (200, {"Content-Type": "application/json"}, body),
                )
                report = rv.diagnose_channel(
                    "LIVE_LOCATOR_CANARY",
                    endpoints=endpoints(server),
                    timeout=2,
                )
                output = rv.render_diagnostic_report(report)
                self.assertEqual(report.overall, rv.Overall.CONTRACT_MISMATCH)
                self.assertEqual(report.stage, rv.DiagnosticStage.EMBED_CONTRACT)
                self.assertEqual(report.reason, expected)
                for forbidden in (
                    "LIVE_LOCATOR_CANARY",
                    "EMBED_TITLE_CANARY",
                    "EMBED_DUPLICATE_CANARY",
                    "777",
                    "888",
                ):
                    self.assertNotIn(forbidden, output)


class PublicValidationTests(unittest.TestCase):
    def test_response_header_limits_match_native_bounds(self) -> None:
        production_sized = [
            ("Content-Type", "text/html"),
            ("Content-Security-Policy", "x" * (20 * 1024)),
        ]
        self.assertTrue(rv._headers_within_limits(production_sized))

        exact_bytes = [
            ("X", "x" * (rv.MAX_HEADER_BYTES - len("X") - 4)),
        ]
        self.assertTrue(rv._headers_within_limits(exact_bytes))
        self.assertFalse(rv._headers_within_limits([
            ("X", exact_bytes[0][1] + "x"),
        ]))

        exact_count = [
            (f"X-Fixture-{index}", "")
            for index in range(rv.MAX_HEADERS)
        ]
        self.assertTrue(rv._headers_within_limits(exact_count))
        self.assertFalse(rv._headers_within_limits(
            exact_count + [("X-Fixture-Overflow", "")]
        ))
        self.assertFalse(rv._headers_within_limits([("", "x")]))
        self.assertFalse(rv._headers_within_limits([
            ("X-Fixture", "line\r\nbreak"),
        ]))

    def test_live_reconnect_and_page_offline_pass_with_redacted_markdown(self) -> None:
        live = "LIVE_LOCATOR_CANARY"
        offline = "OFFLINE_LOCATOR_CANARY"
        with FixtureServer() as server:
            install_public_success(server, live_slug=live, offline_slug=offline)
            report = rv.validate_public(live, offline, endpoints=endpoints(server), timeout=2)
            output = rv.render_report(report, observed_date=rv.dt.date(2026, 7, 14))

            self.assertEqual(report.overall, rv.Overall.PASS)
            self.assertIn("RUMBLE_PUBLIC_VALIDATION_V1=PASS", output)
            self.assertIn("first video duration marks live", output)
            self.assertIn("first video duration marks offline", output)
            self.assertIn(
                "reconnect init valid; initial-window overlap present",
                output,
            )
            for forbidden in (
                live,
                offline,
                "vLIVESECRET123",
                "777777",
                "REMOTE_MESSAGE_ID_CANARY",
                "REMOTE_MESSAGE_CANARY",
                "REMOTE_USER_ID_CANARY",
                "REMOTE_USERNAME_CANARY",
            ):
                self.assertNotIn(forbidden, output)

            public_requests = [r for r in server.fixture_state.requests if r["method"] == "GET"]
            self.assertTrue(public_requests)
            self.assertTrue(all("Cookie" not in r["headers"] for r in public_requests))
            self.assertEqual(
                sum(
                    request["path"] == "/chat/api/chat/777777/stream"
                    for request in public_requests
                ),
                2,
            )
            self.assertFalse(any(
                "vofflinesecret456" in request["query"]
                for request in public_requests
            ))

    def test_sse_204_is_a_narrow_offline_success(self) -> None:
        with FixtureServer() as server:
            install_public_success(server, offline_at_sse=True)
            report = rv.validate_public(
                "LIVE_LOCATOR_CANARY",
                "OFFLINE_LOCATOR_CANARY",
                endpoints=endpoints(server),
                timeout=2,
            )
            output = rv.render_report(report)

            self.assertEqual(report.overall, rv.Overall.PASS)
            self.assertIn("confirmed offline at anonymous SSE stage", output)
            self.assertIn(
                "| offline | anonymous SSE | offline-confirmed | 204 |",
                output,
            )

    def test_live_sse_init_cannot_pass_as_the_offline_control(self) -> None:
        with FixtureServer() as server:
            install_public_success(server, offline_at_sse=True)
            server.fixture_state.route(
                "GET",
                "/chat/api/chat/888888/stream",
                (
                    200,
                    {"Content-Type": "text/event-stream"},
                    init_event("OFFLINE_CONTROL_MESSAGE_CANARY"),
                ),
            )
            report = rv.validate_public(
                "LIVE_LOCATOR_CANARY",
                "OFFLINE_LOCATOR_CANARY",
                endpoints=endpoints(server),
                timeout=2,
            )
            output = rv.render_report(report)

            self.assertEqual(
                report.overall,
                rv.Overall.INCONCLUSIVE_INPUT_OR_REMOTE,
            )
            self.assertIn(
                "| offline | anonymous SSE | input-or-remote-failure | 200 |",
                output,
            )
            self.assertNotIn("OFFLINE_CONTROL_MESSAGE_CANARY", output)

    def test_malformed_sse_is_reported_and_redacted_by_public_validation(self) -> None:
        raw = "RAW_MALFORMED_REMOTE_BODY_CANARY"
        with FixtureServer() as server:
            install_public_success(server)
            server.fixture_state.route(
                "GET",
                "/chat/api/chat/777777/stream",
                (200, {"Content-Type": "text/event-stream"}, f"data: {raw}\n\n".encode()),
            )
            report = rv.validate_public(
                "LIVE_LOCATOR_CANARY",
                "OFFLINE_LOCATOR_CANARY",
                endpoints=endpoints(server),
                timeout=2,
            )
            output = rv.render_report(report)
            self.assertEqual(report.overall, rv.Overall.CONTRACT_MISMATCH)
            self.assertNotIn(raw, output)
            self.assertIn("reachable response did not match the expected shape", output)

    def test_wrong_sse_media_shape_is_public_contract_mismatch(self) -> None:
        with FixtureServer() as server:
            install_public_success(server)
            server.fixture_state.route(
                "GET",
                "/chat/api/chat/777777/stream",
                (200, {"Content-Type": "text/plain"}, init_event()),
            )
            report = rv.validate_public(
                "LIVE_LOCATOR_CANARY",
                "OFFLINE_LOCATOR_CANARY",
                endpoints=endpoints(server),
                timeout=2,
            )
            self.assertEqual(report.overall, rv.Overall.CONTRACT_MISMATCH)

    def test_malformed_embed_json_is_mismatch_and_redacted(self) -> None:
        raw = "MALFORMED_EMBED_BODY_CANARY"
        with FixtureServer() as server:
            install_public_success(server)
            server.fixture_state.route(
                "GET",
                "/embedJS/u3/",
                (200, {"Content-Type": "application/json"}, raw.encode()),
            )
            report = rv.validate_public(
                "LIVE_LOCATOR_CANARY",
                "OFFLINE_LOCATOR_CANARY",
                endpoints=endpoints(server),
                timeout=2,
            )
            output = rv.render_report(report)
            self.assertEqual(report.overall, rv.Overall.CONTRACT_MISMATCH)
            self.assertNotIn(raw, output)

    def test_access_and_server_statuses_are_not_misreported_as_offline(self) -> None:
        for status in (403, 503):
            with self.subTest(status=status), FixtureServer() as server:
                install_public_success(server)
                server.fixture_state.route(
                    "GET",
                    "/c/OFFLINE_LOCATOR_CANARY/live/",
                    (status, {"Content-Type": "text/plain"}, b"REMOTE_BODY_CANARY"),
                )
                report = rv.validate_public(
                    "LIVE_LOCATOR_CANARY",
                    "OFFLINE_LOCATOR_CANARY",
                    endpoints=endpoints(server),
                    timeout=2,
                )
                output = rv.render_report(report)
                self.assertEqual(report.overall, rv.Overall.INCONCLUSIVE_INPUT_OR_REMOTE)
                self.assertNotIn("offline-confirmed", output)
                self.assertNotIn("REMOTE_BODY_CANARY", output)

    def test_missing_live_embed_is_inconclusive_not_a_false_pass(self) -> None:
        with FixtureServer() as server:
            install_public_success(server)
            server.fixture_state.route(
                "GET",
                "/c/LIVE_LOCATOR_CANARY/live/",
                (200, {"Content-Type": "text/html"},
                 b"<html><title>Offline fixture</title>NO_EMBED_CANARY</html>"),
            )
            server.fixture_state.route(
                "GET",
                "/user/LIVE_LOCATOR_CANARY/live/",
                (404, {"Content-Type": "text/html"}, b"missing"),
            )
            report = rv.validate_public(
                "LIVE_LOCATOR_CANARY",
                "OFFLINE_LOCATOR_CANARY",
                endpoints=endpoints(server),
                timeout=2,
            )
            output = rv.render_report(report)
            self.assertEqual(report.overall, rv.Overall.INCONCLUSIVE_INPUT_OR_REMOTE)
            self.assertNotIn("NO_EMBED_CANARY", output)

    def test_primary_offline_page_never_falls_through_to_legacy_profile(self) -> None:
        with FixtureServer() as server:
            server.fixture_state.route(
                "GET",
                "/c/OFFLINE_LOCATOR_CANARY/live/",
                (
                    200,
                    {"Content-Type": "text/html"},
                    b"<html><title>Authoritative offline</title></html>",
                ),
            )
            server.fixture_state.route(
                "GET",
                "/user/OFFLINE_LOCATOR_CANARY/live/",
                (
                    200,
                    {"Content-Type": "text/html"},
                    html_with_embed("vshouldnotresolve"),
                ),
            )
            transport = rv.NetworkTransport(endpoints(server), timeout=2)
            resolved = rv.resolve_stream(
                transport,
                "OFFLINE_LOCATOR_CANARY",
                rv.Scenario.OFFLINE,
                expect_live=False,
            )
            self.assertIsNone(resolved.stream_id)
            self.assertTrue(resolved.offline_confirmed)
            self.assertEqual(resolved.overall, rv.Overall.PASS)
            self.assertEqual(
                [request["path"] for request in server.fixture_state.requests],
                ["/c/OFFLINE_LOCATOR_CANARY/live/"],
            )

    def test_public_resolution_drains_non_success_bodies_before_fallback(self) -> None:
        with FixtureServer() as server:
            install_public_success(server)
            server.fixture_state.route(
                "GET",
                "/c/LIVE_LOCATOR_CANARY/live/",
                (
                    404,
                    {"Content-Type": "text/html"},
                    b"x" * (rv.MAX_PAGE_BYTES + 1),
                ),
            )
            report = rv.validate_public(
                "LIVE_LOCATOR_CANARY",
                "OFFLINE_LOCATOR_CANARY",
                endpoints=endpoints(server),
                timeout=2,
            )
            self.assertEqual(report.overall, rv.Overall.CONTRACT_MISMATCH)
            self.assertFalse(any(
                request["path"] == "/user/LIVE_LOCATOR_CANARY/live/"
                for request in server.fixture_state.requests
            ))
            self.assertIn(
                "response exceeded the fixed safety bound",
                rv.render_report(report),
            )

        with FixtureServer() as server:
            install_public_success(server)
            server.fixture_state.route(
                "GET",
                "/embedJS/u3/",
                (
                    503,
                    {"Content-Type": "application/json"},
                    b"x" * (rv.MAX_EMBED_BYTES + 1),
                ),
            )
            report = rv.validate_public(
                "LIVE_LOCATOR_CANARY",
                "OFFLINE_LOCATOR_CANARY",
                endpoints=endpoints(server),
                timeout=2,
            )
            self.assertEqual(report.overall, rv.Overall.CONTRACT_MISMATCH)
            self.assertIn(
                "response exceeded the fixed safety bound",
                rv.render_report(report),
            )

    def test_finite_body_limit_has_closed_failure_shape(self) -> None:
        opened = opened_response(b"TOO_LARGE_CANARY", media=rv.Media.JSON)
        with self.assertRaises(rv.ValidationException) as caught:
            rv.NetworkTransport.read_bounded(opened, 4)
        self.assertEqual(caught.exception.kind, rv.FailureKind.TOO_LARGE)

    def test_channel_page_reader_stops_before_oversized_inline_tail(self) -> None:
        decisive = (
            b"<html><head><title>Fixture channel</title></head><body>"
            b'<div class="videostream" duration="0">'
            b'<iframe src="https://rumble.com/embed/vprefix/"></iframe>'
            b"</div>"
        )
        body = (
            decisive
            + b'<script>'
            + b"x" * (rv.MAX_PAGE_BYTES + 1)
            + b"</script></html>"
        )
        opened = opened_response(body, media=rv.Media.HTML)
        parsed = rv.NetworkTransport.read_channel_page_contract(opened)
        self.assertEqual(parsed.state, rv.PageState.LIVE)
        self.assertEqual(parsed.embed_id, "vprefix")
        self.assertTrue(parsed.first_video_live)
        self.assertLessEqual(opened.response.tell(), rv.PAGE_READ_CHUNK_BYTES)

    def test_finite_read_has_a_hard_wall_deadline_against_slow_drip(self) -> None:
        class SlowDrip:
            def read(self, limit: int) -> bytes:
                value = bytearray()
                for _ in range(100):
                    time.sleep(0.02)
                    value.extend(b"x")
                return bytes(value)

            def close(self) -> None:
                return

        opened = rv.OpenedResponse(
            response=SlowDrip(),
            status=200,
            media=rv.Media.JSON,
            timing=rv.Timing.UNDER_1,
            retry_after_present=False,
            deadline=time.monotonic() + 0.05,
        )
        started = time.monotonic()
        with self.assertRaises(rv.ValidationException) as caught:
            rv.NetworkTransport.read_bounded(opened, 1024)
        self.assertEqual(caught.exception.kind, rv.FailureKind.TIMEOUT)
        self.assertLess(time.monotonic() - started, 0.3)

    def test_rate_limit_reports_presence_not_value_or_body(self) -> None:
        with FixtureServer() as server:
            install_public_success(server)
            server.fixture_state.route(
                "GET",
                "/c/LIVE_LOCATOR_CANARY/live/",
                (
                    429,
                    {"Content-Type": "text/plain", "Retry-After": "SECRET_DELAY_CANARY"},
                    b"SECRET_RATE_LIMIT_BODY_CANARY",
                ),
            )
            report = rv.validate_public(
                "LIVE_LOCATOR_CANARY",
                "OFFLINE_LOCATOR_CANARY",
                endpoints=endpoints(server),
                timeout=2,
            )
            output = rv.render_report(report)
            self.assertEqual(report.overall, rv.Overall.INCONCLUSIVE_INPUT_OR_REMOTE)
            self.assertIn("remote rate limit included Retry-After", output)
            self.assertNotIn("SECRET_DELAY_CANARY", output)
            self.assertNotIn("SECRET_RATE_LIMIT_BODY_CANARY", output)

    def test_cross_host_redirect_is_rejected_without_location(self) -> None:
        location = "https://evil.example/REDIRECT_LOCATION_CANARY"
        with FixtureServer() as server:
            install_public_success(server)
            server.fixture_state.route(
                "GET",
                "/c/LIVE_LOCATOR_CANARY/live/",
                (302, {"Location": location, "Content-Type": "text/html"}, b""),
            )
            report = rv.validate_public(
                "LIVE_LOCATOR_CANARY",
                "OFFLINE_LOCATOR_CANARY",
                endpoints=endpoints(server),
                timeout=2,
            )
            output = rv.render_report(report)
            self.assertEqual(report.overall, rv.Overall.CONTRACT_MISMATCH)
            self.assertIn("redirect violated the fixed endpoint policy", output)
            self.assertNotIn(location, output)

    def test_same_host_redirect_cannot_change_resource_selector(self) -> None:
        with FixtureServer() as server:
            server.fixture_state.route(
                "GET",
                "/c/LIVE_LOCATOR_CANARY/live/",
                (
                    302,
                    {
                        "Content-Type": "text/html",
                        "Location": "/user/LIVE_LOCATOR_CANARY/live/",
                    },
                    b"",
                ),
            )
            report = rv.validate_public(
                "LIVE_LOCATOR_CANARY",
                "OFFLINE_LOCATOR_CANARY",
                endpoints=endpoints(server),
                timeout=2,
            )
            self.assertEqual(report.overall, rv.Overall.CONTRACT_MISMATCH)
            self.assertEqual(
                sum(
                    request["path"] == "/c/LIVE_LOCATOR_CANARY/live/"
                    for request in server.fixture_state.requests
                ),
                1,
            )
            self.assertFalse(any(
                request["path"] == "/user/LIVE_LOCATOR_CANARY/live/"
                for request in server.fixture_state.requests
            ))
            self.assertIn("redirect violated", rv.render_report(report))

    def test_each_redirect_body_is_drained_under_the_reply_limit(self) -> None:
        with FixtureServer() as server:
            server.fixture_state.route(
                "GET",
                "/same-resource",
                (
                    302,
                    {"Content-Type": "text/html", "Location": "/same-resource"},
                    b"12345",
                ),
            )
            transport = rv.NetworkTransport(endpoints(server), timeout=2)
            with self.assertRaises(rv.ValidationException) as caught:
                transport.open(
                    "GET",
                    server.base_url + "/same-resource",
                    allowed_hosts=endpoints(server).hosts_for(server.base_url),
                    max_body_bytes=4,
                )
            self.assertEqual(caught.exception.kind, rv.FailureKind.TOO_LARGE)

    def test_timeout_is_bounded_and_does_not_emit_exception_text(self) -> None:
        def slow(_: dict[str, Any]) -> Route:
            time.sleep(0.2)
            return (200, {"Content-Type": "text/html"}, html_with_embed("vtooslow"))

        with FixtureServer() as server:
            install_public_success(server)
            server.fixture_state.route("GET", "/c/LIVE_LOCATOR_CANARY/live/", slow)
            report = rv.validate_public(
                "LIVE_LOCATOR_CANARY",
                "OFFLINE_LOCATOR_CANARY",
                endpoints=endpoints(server),
                timeout=0.05,
            )
            output = rv.render_report(report)
            self.assertEqual(report.overall, rv.Overall.INCONCLUSIVE_ENVIRONMENT)
            self.assertIn("bounded request timed out", output)
            self.assertNotIn(server.base_url, output)


class AuthenticatedValidationTests(unittest.TestCase):
    SESSION = "SESSION_SECRET_CANARY"
    MESSAGE = "GENERATED_MESSAGE_CANARY"
    REQUEST_ID = "GENERATED_REQUEST_ID_CANARY"

    def install_auth_success(self, server: FixtureServer) -> None:
        install_public_success(server)
        server.fixture_state.route(
            "GET",
            "/service.php",
            (
                200,
                {"Content-Type": "application/json"},
                json.dumps({
                    "user": {
                        "id": "REMOTE_AUTH_USER_ID_CANARY",
                        "username": "REMOTE_AUTH_USERNAME_CANARY",
                    }
                }).encode(),
            ),
        )
        server.fixture_state.route(
            "POST",
            "/chat/api/chat/777777/message",
            (
                200,
                {"Content-Type": "application/json"},
                json.dumps({"data": {"id": "REMOTE_SENT_MESSAGE_ID_CANARY"}}).encode(),
            ),
        )

    def run_auth(self, server: FixtureServer, *, send: bool = True) -> rv.Report:
        return rv.validate_authenticated(
            "LIVE_LOCATOR_CANARY",
            self.SESSION,
            send=send,
            endpoints=endpoints(server),
            timeout=2,
            request_id_factory=lambda: self.REQUEST_ID,
            message_factory=lambda: self.MESSAGE,
        )

    def test_probe_and_one_shot_send_pass_and_report_is_redacted(self) -> None:
        with FixtureServer() as server:
            self.install_auth_success(server)
            report = self.run_auth(server)
            output = rv.render_report(report, observed_date=rv.dt.date(2026, 7, 14))

            self.assertEqual(report.overall, rv.Overall.PASS)
            self.assertEqual(report.send_attempts, 1)
            self.assertIn("RUMBLE_AUTHENTICATED_VALIDATION_V1=PASS", output)
            for forbidden in (
                self.SESSION,
                self.MESSAGE,
                self.REQUEST_ID,
                "LIVE_LOCATOR_CANARY",
                "777777",
                "REMOTE_AUTH_USER_ID_CANARY",
                "REMOTE_AUTH_USERNAME_CANARY",
                "REMOTE_SENT_MESSAGE_ID_CANARY",
            ):
                self.assertNotIn(forbidden, output)

            sends = [r for r in server.fixture_state.requests if r["method"] == "POST"]
            self.assertEqual(len(sends), 1)
            sent_document = json.loads(sends[0]["body"])
            self.assertEqual(sent_document["data"]["request_id"], self.REQUEST_ID)
            self.assertEqual(sent_document["data"]["message"]["text"], self.MESSAGE)
            self.assertEqual(sends[0]["headers"].get("Cookie"), f"u_s={self.SESSION}")
            self.assertEqual(sends[0]["headers"].get("Origin"), "https://rumble.com")

    def test_probe_only_never_sends(self) -> None:
        with FixtureServer() as server:
            self.install_auth_success(server)
            report = self.run_auth(server, send=False)
            self.assertEqual(report.overall, rv.Overall.PASS)
            self.assertEqual(report.send_attempts, 0)
            self.assertFalse(any(r["method"] == "POST" for r in server.fixture_state.requests))
            self.assertIn("mutation not requested", rv.render_report(report))

    def test_rejected_session_stops_before_send_and_redacts_body(self) -> None:
        raw = "RAW_SESSION_REJECTION_CANARY"
        for status in (401, 403):
            with self.subTest(status=status), FixtureServer() as server:
                self.install_auth_success(server)
                server.fixture_state.route(
                    "GET",
                    "/service.php",
                    (status, {"Content-Type": "application/json"}, raw.encode()),
                )
                report = self.run_auth(server)
                output = rv.render_report(report)
                self.assertEqual(report.overall, rv.Overall.INCONCLUSIVE_INPUT_OR_REMOTE)
                self.assertEqual(report.send_attempts, 0)
                self.assertFalse(any(r["method"] == "POST" for r in server.fixture_state.requests))
                self.assertIn("session was not accepted", output)
                self.assertNotIn(raw, output)
                self.assertNotIn(self.SESSION, output)

    def test_ambiguous_post_is_attempted_once_and_never_retried(self) -> None:
        with FixtureServer() as server:
            self.install_auth_success(server)
            server.fixture_state.route(
                "POST",
                "/chat/api/chat/777777/message",
                lambda _: None,
            )
            report = self.run_auth(server)
            output = rv.render_report(report)
            self.assertEqual(report.overall, rv.Overall.INCONCLUSIVE_INPUT_OR_REMOTE)
            self.assertEqual(report.send_attempts, 1)
            self.assertEqual(
                sum(r["method"] == "POST" for r in server.fixture_state.requests),
                1,
            )
            self.assertIn("delivery outcome ambiguous after one attempt; not retried", output)
            self.assertNotIn(self.SESSION, output)

    def test_send_rate_limit_is_not_retried_and_does_not_emit_values(self) -> None:
        with FixtureServer() as server:
            self.install_auth_success(server)
            server.fixture_state.route(
                "POST",
                "/chat/api/chat/777777/message",
                (
                    429,
                    {"Content-Type": "application/json", "Retry-After": "RATE_VALUE_CANARY"},
                    b"RATE_BODY_CANARY",
                ),
            )
            report = self.run_auth(server)
            output = rv.render_report(report)
            self.assertEqual(report.overall, rv.Overall.INCONCLUSIVE_INPUT_OR_REMOTE)
            self.assertEqual(sum(r["method"] == "POST" for r in server.fixture_state.requests), 1)
            self.assertIn("remote rate limit included Retry-After", output)
            self.assertNotIn("RATE_VALUE_CANARY", output)
            self.assertNotIn("RATE_BODY_CANARY", output)

    def test_malformed_send_success_is_contract_mismatch_and_redacted(self) -> None:
        raw = "MALFORMED_SEND_SUCCESS_CANARY"
        with FixtureServer() as server:
            self.install_auth_success(server)
            server.fixture_state.route(
                "POST",
                "/chat/api/chat/777777/message",
                (200, {"Content-Type": "application/json"}, json.dumps({"data": {"detail": raw}}).encode()),
            )
            report = self.run_auth(server)
            output = rv.render_report(report)
            self.assertEqual(report.overall, rv.Overall.CONTRACT_MISMATCH)
            self.assertEqual(sum(r["method"] == "POST" for r in server.fixture_state.requests), 1)
            self.assertIn("acknowledgement shape mismatched; delivery ambiguous; not retried", output)
            self.assertNotIn(raw, output)

    def test_server_error_after_post_is_ambiguous_and_not_retried(self) -> None:
        with FixtureServer() as server:
            self.install_auth_success(server)
            server.fixture_state.route(
                "POST",
                "/chat/api/chat/777777/message",
                (503, {"Content-Type": "application/json"}, b'{"detail":"REMOTE_5XX_CANARY"}'),
            )
            report = self.run_auth(server)
            output = rv.render_report(report)
            self.assertEqual(report.overall, rv.Overall.INCONCLUSIVE_INPUT_OR_REMOTE)
            self.assertEqual(sum(r["method"] == "POST" for r in server.fixture_state.requests), 1)
            self.assertIn("delivery outcome ambiguous after one attempt; not retried", output)
            self.assertNotIn("REMOTE_5XX_CANARY", output)

    def test_authenticated_redirect_is_rejected_without_forwarding_cookie(self) -> None:
        location = "https://evil.example/AUTH_REDIRECT_CANARY"
        with FixtureServer() as server:
            self.install_auth_success(server)
            server.fixture_state.route(
                "GET",
                "/service.php",
                (302, {"Content-Type": "text/html", "Location": location}, b""),
            )
            report = self.run_auth(server)
            output = rv.render_report(report)
            self.assertEqual(report.overall, rv.Overall.CONTRACT_MISMATCH)
            service_requests = [r for r in server.fixture_state.requests if r["path"] == "/service.php"]
            self.assertEqual(len(service_requests), 1)
            self.assertNotIn(location, output)
            self.assertNotIn(self.SESSION, output)

    def test_authenticated_probe_timeout_stops_before_send(self) -> None:
        def slow(_: dict[str, Any]) -> Route:
            time.sleep(0.2)
            return (200, {"Content-Type": "application/json"}, b'{"user":{"id":"late"}}')

        with FixtureServer() as server:
            self.install_auth_success(server)
            server.fixture_state.route("GET", "/service.php", slow)
            report = rv.validate_authenticated(
                "LIVE_LOCATOR_CANARY",
                self.SESSION,
                send=True,
                endpoints=endpoints(server),
                timeout=0.05,
                request_id_factory=lambda: self.REQUEST_ID,
                message_factory=lambda: self.MESSAGE,
            )
            self.assertEqual(report.overall, rv.Overall.INCONCLUSIVE_ENVIRONMENT)
            self.assertEqual(report.send_attempts, 0)
            self.assertFalse(any(r["method"] == "POST" for r in server.fixture_state.requests))


class ParserAndExitTests(unittest.TestCase):
    def test_event_stream_media_type_accepts_only_observed_utf8_parameters(
        self,
    ) -> None:
        accepted = (
            "text/event-stream",
            "text/event-stream; charset=utf-8",
            "text/event-stream; charset=utf-8; charset=UTF-8",
        )
        for value in accepted:
            with self.subTest(value=value):
                self.assertEqual(
                    rv.media_shape({"Content-Type": value}, 200),
                    rv.Media.EVENT_STREAM,
                )

        rejected = (
            "text/event-stream;",
            "text/event-stream; charset=iso-8859-1",
            'text/event-stream; charset="utf-8"',
            "text/event-stream; boundary=secret",
            "text/event-stream; charset=utf-8; charset=utf-8; charset=utf-8",
        )
        for value in rejected:
            with self.subTest(value=value):
                self.assertEqual(
                    rv.media_shape({"Content-Type": value}, 200),
                    rv.Media.OTHER,
                )

    def test_unknown_sse_event_before_init_is_ignored(self) -> None:
        unknown = b'data: {"type":"future","data":{"secret":"CANARY"}}\n\n'
        with FixtureServer() as server:
            install_public_success(server)
            server.fixture_state.route(
                "GET",
                "/chat/api/chat/777777/stream",
                (200, {"Content-Type": "text/event-stream"}, unknown + init_event()),
            )
            transport = rv.NetworkTransport(endpoints(server), timeout=2)
            _init, row, overall = rv._validate_sse_once(
                transport,
                777777,
                rv.Scenario.LIVE,
                rv.Stage.SSE,
            )
            self.assertEqual(overall, rv.Overall.PASS)
            self.assertNotIn(
                "CANARY",
                rv.render_report(rv.Report("public", overall, (row,))),
            )

    def test_sse_line_and_total_bounds_are_enforced(self) -> None:
        oversized_line = b"data: " + b"x" * (rv.MAX_SSE_LINE_BYTES + 1) + b"\n\n"
        with self.assertRaises(rv.ValidationException) as line_error:
            rv.parse_sse_init(opened_response(oversized_line))
        self.assertEqual(line_error.exception.kind, rv.FailureKind.TOO_LARGE)

        with unittest.mock.patch.object(rv, "MAX_SSE_BYTES", 20):
            with self.assertRaises(rv.ValidationException) as total_error:
                rv.parse_sse_init(opened_response(b": keepalive\n" * 4))
        self.assertEqual(total_error.exception.kind, rv.FailureKind.TOO_LARGE)

    def test_init_requires_observed_container_types(self) -> None:
        valid_data = {
            "users": [],
            "channels": [],
            "config": {"badges": {}},
            "messages": [],
        }
        mutations = (
            ("users", None),
            ("users", ["wrong"]),
            ("channels", "wrong"),
            ("channels", ["wrong"]),
            ("config", {"badges": []}),
            ("messages", ["wrong"]),
        )
        for key, wrong_value in mutations:
            data = dict(valid_data)
            data[key] = wrong_value
            event = json.dumps({"type": "init", "data": data}).encode()
            with self.subTest(key=key), self.assertRaises(rv.ValidationException) as caught:
                rv.parse_sse_init(opened_response(b"data: " + event + b"\n\n"))
            self.assertEqual(caught.exception.kind, rv.FailureKind.SHAPE)

    def test_exit_code_contract(self) -> None:
        self.assertEqual(rv.exit_code(rv.Overall.PASS), 0)
        self.assertEqual(rv.exit_code(rv.Overall.INCONCLUSIVE_ENVIRONMENT), 10)
        self.assertEqual(rv.exit_code(rv.Overall.INCONCLUSIVE_INPUT_OR_REMOTE), 11)
        self.assertEqual(rv.exit_code(rv.Overall.CONTRACT_MISMATCH), 12)
        self.assertEqual(rv.exit_code(rv.Overall.INTERNAL_ERROR), 70)

    def test_cli_rejection_never_echoes_the_rejected_locator(self) -> None:
        canary = "REJECTED_LOCATOR_CANARY"
        stdout = io.StringIO()
        stderr = io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            result = rv.main([
                "public",
                "--live-channel",
                f"https://rumble.com:443/c/{canary}",
                "--offline-channel",
                "offline",
            ])
        self.assertEqual(result, 2)
        self.assertNotIn(canary, stdout.getvalue())
        self.assertNotIn(canary, stderr.getvalue())
        self.assertNotIn(":443", stderr.getvalue())

    def test_request_id_uses_observed_unpadded_shape(self) -> None:
        request_id = rv._make_request_id()
        self.assertEqual(len(request_id), 43)
        self.assertRegex(request_id, r"^[A-Za-z0-9_-]{43}$")

    def test_parser_exposes_no_session_argument(self) -> None:
        parser = rv.build_parser()
        with self.assertRaises(rv.UsageException):
            parser.parse_args([
                "authenticated",
                "--channel",
                "approved",
                "--session",
                "SECRET_CANARY",
            ])


if __name__ == "__main__":
    unittest.main()
