from __future__ import annotations

import contextlib
import http.server
import importlib.util
import io
import json
import os
import sys
import tempfile
import threading
import time
import types
import unittest
import urllib.request
from pathlib import Path
from unittest import mock
from urllib.parse import urljoin


ROOT = Path(__file__).resolve().parents[2]
PLUGIN_PATH = ROOT / "streamlink-plugins" / "rumble.py"
FOLLOWER_PATH = ROOT / "chatterino-streamlink-follow-fixed.py"


class FakeHLSStream:
    variant_calls = []

    def __init__(self, session, url):
        self.session = session
        self.url = url

    @classmethod
    def parse_variant_playlist(cls, session, url, **kwargs):
        cls.variant_calls.append((session, url, kwargs))
        if "/master.m3u8" not in url:
            return {}
        return {
            "360p": cls(session, "https://cdn.example.com/media/360.m3u8"),
            "720p": cls(session, "https://cdn.example.com/media/720.m3u8"),
        }


class FakePlugin:
    def __init__(self, session, url, options=None):
        self.session = session
        self.url = url
        self.options = options or {}
        self.match = self.__class__._matcher.fullmatch(url)

    def streams(self):
        streams = self._get_streams() or {}
        if streams:
            ordered = list(streams)
            streams.setdefault("worst", streams[ordered[0]])
            streams.setdefault("best", streams[ordered[-1]])
        return streams


def fake_pluginmatcher(pattern):
    def decorate(cls):
        cls._matcher = pattern
        return cls

    return decorate


def load_plugin():
    requests = types.ModuleType("requests")
    streamlink = types.ModuleType("streamlink")
    plugin = types.ModuleType("streamlink.plugin")
    stream = types.ModuleType("streamlink.stream")
    hls = types.ModuleType("streamlink.stream.hls")
    plugin.Plugin = FakePlugin
    plugin.pluginmatcher = fake_pluginmatcher
    hls.HLSStream = FakeHLSStream
    requests.Session = object
    requests.RequestException = OSError
    with mock.patch.dict(
        sys.modules,
        {
            "requests": requests,
            "streamlink": streamlink,
            "streamlink.plugin": plugin,
            "streamlink.stream": stream,
            "streamlink.stream.hls": hls,
        },
    ):
        spec = importlib.util.spec_from_file_location("_test_rumble_streamlink_plugin", PLUGIN_PATH)
        assert spec and spec.loader
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    return module


PLUGIN = load_plugin()


class Response:
    def __init__(self, url, body, *, status=200, content_type="text/html", chunk=65_536):
        self.url = url
        self.body = body
        self.status_code = status
        self.headers = {"Content-Type": content_type}
        self.chunk = chunk
        self.closed = False

    def iter_content(self, chunk_size=65_536):
        for offset in range(0, len(self.body), self.chunk):
            yield self.body[offset : offset + self.chunk]

    def close(self):
        self.closed = True


class HTTPRoutes:
    def __init__(self, routes):
        self.routes = routes
        self.calls = []
        self.sessions = []

    def session(self):
        owner = self

        class Session:
            def __init__(self):
                self.trust_env = True
                self.auth = "inherited"
                self.cookies = mock.Mock()
                self.headers = {}
                owner.sessions.append(self)

            def get(self, url, **kwargs):
                owner.calls.append((url, kwargs))
                route = owner.routes[url]
                return route() if callable(route) else route

            def close(self):
                pass

        return Session()


def live_page(embed="vfixture"):
    return (
        "<!doctype html><html><head><title>Live</title></head><body>"
        f'<div class="tile videostream" duration="0"><iframe src="https://rumble.com/embed/{embed}/"></iframe></div>'
        "</body></html>"
    ).encode()


def player_data(*, live=2, host="cdn.example.com"):
    return json.dumps(
        {
            "live": live,
            "ua": {
                "hls": {
                    "720": {"url": f"https://{host}/signed/720.m3u8?token=secret", "meta": {"h": 720}},
                    "1080": {"url": f"https://{host}/signed/1080.m3u8?token=secret", "meta": {"h": 1080}},
                }
            },
        }
    ).encode()


def player_data_with_master(*, live=2):
    return json.dumps(
        {
            "live": live,
            "ua": {
                "hls": {
                    "720": {
                        "url": "https://cdn.example.com/master.m3u8?token=secret",
                        "meta": {"h": 720},
                    },
                },
            },
        }
    ).encode()


class RumblePluginTests(unittest.TestCase):
    @staticmethod
    def channel_page(channel):
        return channel.rstrip("/") + "/live/"

    def plugin(self, url, routes):
        http = HTTPRoutes(routes)
        plugin = PLUGIN.Rumble(object(), url)
        return plugin, http

    def run_plugin(self, plugin, http):
        with mock.patch.object(PLUGIN.requests, "Session", side_effect=http.session):
            return plugin.streams()

    def test_live_channel_returns_stable_hls_qualities(self):
        channel = "https://rumble.com/c/Fixture"
        page = self.channel_page(channel)
        embed = "https://rumble.com/embedJS/u3/?request=video&ver=2&v=vfixture"
        plugin, http = self.plugin(
            channel,
            {
                page: Response(page, live_page()),
                embed: Response(embed, player_data(), content_type="application/json"),
            },
        )
        streams = self.run_plugin(plugin, http)
        self.assertEqual({"720p", "1080p", "best", "worst"}, set(streams))
        self.assertEqual("https://cdn.example.com/signed/1080.m3u8?token=secret", streams["1080p"].url)
        self.assertEqual([page, embed], [call[0] for call in http.calls])
        self.assertTrue(all(
            call[1]["headers"]["User-Agent"] == "chatterino-rumble/1"
            for call in http.calls
        ))
        for session in http.sessions:
            self.assertFalse(session.trust_env)
            self.assertIsNone(session.auth)
            session.cookies.clear.assert_called_once_with()

    def test_master_playlist_is_expanded_before_player_handoff(self):
        embed = "https://rumble.com/embedJS/u3/?request=video&ver=2&v=vfixture"
        channel = "https://rumble.com/c/Fixture"
        channel_page = self.channel_page(channel)
        video = "https://rumble.com/vfixture-title.html"
        cases = (
            (
                channel,
                {
                    channel_page: Response(channel_page, live_page()),
                    embed: Response(
                        embed,
                        player_data_with_master(),
                        content_type="application/json",
                    ),
                },
            ),
            (
                "https://rumble.com/embed/vfixture",
                {
                    embed: Response(
                        embed,
                        player_data_with_master(),
                        content_type="application/json",
                    ),
                },
            ),
            (
                video,
                {
                    video: Response(
                        video,
                        b'<html><head><title>Video</title></head><body>'
                        b'<iframe src="https://rumble.com/embed/vfixture/"></iframe>'
                        b"</body></html>",
                    ),
                    embed: Response(
                        embed,
                        player_data_with_master(),
                        content_type="application/json",
                    ),
                },
            ),
        )

        for locator, routes in cases:
            with self.subTest(locator=locator):
                plugin, http = self.plugin(locator, routes)
                FakeHLSStream.variant_calls.clear()
                streams = self.run_plugin(plugin, http)

                self.assertEqual({"360p", "720p", "best", "worst"}, set(streams))
                self.assertEqual(
                    "https://cdn.example.com/media/720.m3u8",
                    streams["720p"].url,
                )
                self.assertNotIn(
                    "https://cdn.example.com/master.m3u8?token=secret",
                    {stream.url for stream in streams.values()},
                )
                self.assertEqual(1, len(FakeHLSStream.variant_calls))
                self.assertTrue(FakeHLSStream.variant_calls[0][2]["check_streams"])
                self.assertEqual("success", plugin.chatterino_result)

    def test_offline_first_card_stops_without_embed_request(self):
        channel = "https://rumble.com/c/Fixture"
        page = self.channel_page(channel)
        body = live_page().replace(b'duration="0"', b'duration="42"')
        plugin, http = self.plugin(channel, {page: Response(page, body)})
        self.assertEqual({}, self.run_plugin(plugin, http))
        self.assertEqual("no_live_stream", plugin.chatterino_result)
        self.assertEqual([page], [call[0] for call in http.calls])

    def test_subprocess_marker_is_closed_vocabulary_and_redacted(self):
        channel = "https://rumble.com/c/Fixture"
        page = self.channel_page(channel)
        body = live_page().replace(b'duration="0"', b'duration="42"')
        plugin, http = self.plugin(channel, {page: Response(page, body)})
        stderr = io.StringIO()
        with (
            mock.patch.dict(
                PLUGIN.os.environ,
                {"CHATTERINO_STREAMLINK_DIAGNOSTICS": "1"},
            ),
            contextlib.redirect_stderr(stderr),
        ):
            self.assertEqual({}, self.run_plugin(plugin, http))
        self.assertEqual(
            "chatterino-rumble-result:no_live_stream\n",
            stderr.getvalue(),
        )
        self.assertNotIn("rumble.com", stderr.getvalue())

    def test_current_channel_without_duration_uses_live_embed_metadata(self):
        channel = "https://rumble.com/c/Fixture"
        page = self.channel_page(channel)
        body = (
            b'<html><head><title>Current live page</title></head><body>'
            b'<svg><title>Unrelated icon title</title></svg>'
            b'<script type="application/json">'
            b'{"video":{"embedUrl":"https://rumble.com/embed/vfixture/"}}'
            b"</script>"
            b"</body></html>"
        )
        embed = "https://rumble.com/embedJS/u3/?request=video&ver=2&v=vfixture"
        plugin, http = self.plugin(
            channel,
            {
                page: Response(page, body),
                embed: Response(
                    embed,
                    player_data(),
                    content_type="application/json",
                ),
            },
        )

        self.assertIn("best", self.run_plugin(plugin, http))
        self.assertEqual([page, embed], [call[0] for call in http.calls])

    def test_current_channel_without_duration_uses_offline_embed_metadata(self):
        channel = "https://rumble.com/c/Fixture"
        page = self.channel_page(channel)
        body = (
            b'<html><head><title>Current offline page</title></head><body>'
            b'<script type="application/ld+json">'
            b'{"videoUrl":"https://rumble.com/embed/vfixture/"}'
            b"</script>"
            b"</body></html>"
        )
        embed = "https://rumble.com/embedJS/u3/?request=video&ver=2&v=vfixture"
        plugin, http = self.plugin(
            channel,
            {
                page: Response(page, body),
                embed: Response(
                    embed,
                    player_data(live=0),
                    content_type="application/json",
                ),
            },
        )

        self.assertEqual({}, self.run_plugin(plugin, http))
        self.assertEqual([page, embed], [call[0] for call in http.calls])

    def test_stale_embed_on_offline_card_is_not_followed(self):
        channel = "https://rumble.com/c/Fixture"
        page = self.channel_page(channel)
        body = live_page("vstale").replace(b'duration="0"', b'duration="9"')
        plugin, http = self.plugin(channel, {page: Response(page, body)})
        self.assertEqual({}, self.run_plugin(plugin, http))
        self.assertEqual(1, len(http.calls))

    def test_direct_embed_skips_channel_page(self):
        url = "https://rumble.com/embed/vfixture"
        embed = "https://rumble.com/embedJS/u3/?request=video&ver=2&v=vfixture"
        plugin, http = self.plugin(url, {embed: Response(embed, player_data(), content_type="application/json")})
        self.assertIn("best", self.run_plugin(plugin, http))
        self.assertEqual([embed], [call[0] for call in http.calls])

    def test_video_page_extracts_embed_without_using_duration_as_status(self):
        url = "https://rumble.com/vfixture-title.html"
        body = b'<html><head><title>Video</title></head><body><iframe src="https://rumble.com/embed/vfixture/"></iframe></body></html>'
        embed = "https://rumble.com/embedJS/u3/?request=video&ver=2&v=vfixture"
        plugin, http = self.plugin(
            url,
            {url: Response(url, body), embed: Response(embed, player_data(), content_type="application/json")},
        )
        self.assertIn("720p", self.run_plugin(plugin, http))

    def test_malformed_interstitial_redirect_oversize_and_offline_metadata_fail_closed(self):
        channel = "https://rumble.com/c/Fixture"
        page = self.channel_page(channel)
        cases = [
            Response(
                page,
                b'<title>Live</title><div class="videostream"></div>',
            ),
            Response(page, b"<title>Just a moment...</title>" + live_page()),
            Response("https://rumble.com/login", live_page(), status=302),
            Response(page, b"x" * (PLUGIN._MAX_PAGE_BYTES + 1)),
            Response(page, live_page() + b"<script>unterminated"),
        ]
        for response in cases:
            with self.subTest(status=response.status_code, size=len(response.body)):
                plugin, http = self.plugin(channel, {page: response})
                self.assertEqual({}, self.run_plugin(plugin, http))
                self.assertEqual(1, len(http.calls))

        embed = "https://rumble.com/embedJS/u3/?request=video&ver=2&v=vfixture"
        plugin, http = self.plugin(
            channel,
            {page: Response(page, live_page()), embed: Response(embed, player_data(live=0), content_type="application/json")},
        )
        self.assertEqual({}, self.run_plugin(plugin, http))

    def test_unsafe_media_hosts_are_rejected(self):
        embed_url = "https://rumble.com/embed/vfixture"
        metadata_url = "https://rumble.com/embedJS/u3/?request=video&ver=2&v=vfixture"
        for host in ("localhost", "127.0.0.1", "metadata.internal"):
            with self.subTest(host=host):
                plugin, http = self.plugin(
                    embed_url,
                    {metadata_url: Response(metadata_url, player_data(host=host), content_type="application/json")},
                )
                self.assertEqual({}, self.run_plugin(plugin, http))

    def test_inert_embed_and_duplicate_metadata_keys_fail_closed(self):
        channel = "https://rumble.com/c/Fixture"
        page = self.channel_page(channel)
        body = (
            b'<html><head><title>Live</title></head><body><template>'
            b'<iframe src="https://rumble.com/embed/vfake/"></iframe>'
            b'<script type="application/json">'
            b'{"embedUrl":"https://rumble.com/embed/vfakejson/"}'
            b"</script></template>"
            + live_page()
            + b"</body></html>"
        )
        embed = "https://rumble.com/embedJS/u3/?request=video&ver=2&v=vfixture"
        duplicate = b'{"live":2,"live":0,"ua":{"hls":{}}}'
        plugin, http = self.plugin(
            channel,
            {page: Response(page, body), embed: Response(embed, duplicate, content_type="application/json")},
        )
        self.assertEqual({}, self.run_plugin(plugin, http))
        self.assertEqual([page, embed], [call[0] for call in http.calls])

    def test_ambiguous_typed_page_embed_metadata_fails_closed(self):
        channel = "https://rumble.com/c/Fixture"
        page = self.channel_page(channel)
        body = (
            b"<html><head><title>Ambiguous</title></head><body>"
            b'<script type="application/json">'
            b'{"embedUrl":"https://rumble.com/embed/vone/"}'
            b"</script>"
            b'<script type="application/ld+json">'
            b'{"videoUrl":"https://rumble.com/embed/vtwo/"}'
            b"</script></body></html>"
        )
        plugin, http = self.plugin(
            channel,
            {page: Response(page, body)},
        )

        self.assertEqual({}, self.run_plugin(plugin, http))
        self.assertEqual("resolver_failed", plugin.chatterino_result)
        self.assertEqual([page], [call[0] for call in http.calls])


def load_follower():
    spec = importlib.util.spec_from_file_location("_test_rumble_follower", FOLLOWER_PATH)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


FOLLOWER = load_follower()


class LocalHlsFixture:
    """A credential-free local HLS origin with controllable segment walls."""

    def __init__(self):
        self.segment_gates: dict[str, threading.Event] = {}
        self.requested: list[str] = []
        self.request_lock = threading.Lock()
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                path = self.path.split("?", 1)[0]
                with owner.request_lock:
                    owner.requested.append(path)
                parts = path.strip("/").split("/")
                if len(parts) != 2:
                    self.send_error(404)
                    return
                channel, resource = parts
                if resource == "index.m3u8":
                    body = (
                        "#EXTM3U\n"
                        "#EXT-X-VERSION:3\n"
                        "#EXT-X-TARGETDURATION:1\n"
                        "#EXT-X-MEDIA-SEQUENCE:1\n"
                        "#EXTINF:1.0,\n"
                        "segment.ts\n"
                        "#EXT-X-ENDLIST\n"
                    ).encode()
                    self.send_response(200)
                    self.send_header("Content-Type", "application/vnd.apple.mpegurl")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    return
                if resource == "segment.ts":
                    gate = owner.segment_gates.setdefault(channel, threading.Event())
                    gate.wait(timeout=2.0)
                    body = (channel.encode() + b"-media") * 16
                    self.send_response(200)
                    self.send_header("Content-Type", "video/mp2t")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    with contextlib.suppress(BrokenPipeError, ConnectionResetError):
                        self.wfile.write(body)
                    return
                self.send_error(404)

            def log_message(self, format, *args):
                pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(
            target=self.server.serve_forever,
            name="local-hls-fixture",
            daemon=True,
        )

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, exc_type, exc, traceback):
        for gate in self.segment_gates.values():
            gate.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2.0)

    def stream(self, channel: str):
        base = f"http://127.0.0.1:{self.server.server_port}/{channel}/index.m3u8"

        class Stream:
            @staticmethod
            def open():
                with urllib.request.urlopen(base, timeout=2.0) as response:
                    playlist = response.read().decode()
                segment = next(
                    line.strip()
                    for line in playlist.splitlines()
                    if line.strip() and not line.startswith("#")
                )
                return urllib.request.urlopen(urljoin(base, segment), timeout=2.0)

        return Stream()

    def wait_for_request(self, path: str, timeout: float = 1.0) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with self.request_lock:
                if path in self.requested:
                    return True
            time.sleep(0.005)
        return False

    def release(self, channel: str) -> None:
        self.segment_gates.setdefault(channel, threading.Event()).set()


class FollowerIntegrationTests(unittest.TestCase):
    def follower(self, *extra):
        args = FOLLOWER.orig.parse(["--dry-run", "--debounce", "0", *extra])
        return FOLLOWER.orig.MpvStreamlinkFollower(args)

    def live_follower(self, directory: str, *extra):
        args = FOLLOWER.orig.parse(
            [
                "--debounce",
                "0",
                "--no-catchup",
                "--fifo-dir",
                directory,
                "--mpv-start-timeout",
                "0.2",
                *extra,
            ]
        )
        return FOLLOWER.orig.MpvStreamlinkFollower(args)

    def install_fifo_handoff(self, follower):
        readers = []

        def handoff(path):
            readers.append(os.open(path, os.O_RDONLY | os.O_NONBLOCK))

        follower.ensure_mpv = mock.Mock()
        follower.mpv_loadfile_replace = mock.Mock(side_effect=handoff)
        follower.monitor_playback_ready = mock.Mock()
        return readers

    @staticmethod
    def close_readers(readers):
        for reader in readers:
            with contextlib.suppress(OSError):
                os.close(reader)

    def test_url_policy_accepts_canonical_rumble_only(self):
        self.assertTrue(FOLLOWER.is_rumble_url("https://rumble.com/c/Fixture"))
        self.assertTrue(FOLLOWER.is_rumble_url("https://www.rumble.com/embed/vfixture"))
        self.assertTrue(FOLLOWER.is_rumble_url("https://rumble.com/vfixture-title.html"))
        for value in (
            "http://rumble.com/c/Fixture",
            "https://rumble.com:443/c/Fixture",
            "https://user@rumble.com/c/Fixture",
            "https://rumble.com/c/Fixture?token=x",
            "https://example.com/c/Fixture",
        ):
            with self.subTest(value=value):
                self.assertFalse(FOLLOWER.is_rumble_url(value))

    def test_subprocess_command_automatically_loads_plugin(self):
        follower = self.follower("--backend", "subprocess")
        event = {"platform": "rumble", "stream_url": "https://rumble.com/c/Fixture"}
        channel = follower.channel_from_event(event)
        self.assertEqual("rumble", channel)
        follower._channel_urls = {channel: event["stream_url"]}
        command = follower.streamlink_command(channel, stdout=True)
        index = command.index("--plugin-dir")
        self.assertEqual(str(FOLLOWER.PLUGIN_DIR), command[index + 1])
        self.assertEqual(event["stream_url"], command[-2])

    def test_subprocess_backend_exposes_stdout_for_measured_fifo_forwarding(self):
        follower = self.follower("--backend", "subprocess")
        follower.args.dry_run = False
        fifo = Path("/tmp/controlled-rumble.fifo")
        process = mock.Mock()

        with mock.patch.object(
            FOLLOWER.subprocess,
            "Popen",
            return_value=process,
        ) as popen:
            self.assertIs(
                process,
                follower.start_streamlink_subprocess_fifo("rumble", fifo),
            )

        command = follower.streamlink_command("rumble", stdout=True)
        popen.assert_called_once_with(
            command,
            stdout=FOLLOWER.subprocess.PIPE,
            start_new_session=(FOLLOWER.orig.os.name == "posix"),
            stderr=FOLLOWER.subprocess.DEVNULL,
            env=mock.ANY,
        )

    def test_subprocess_fifo_worker_measures_first_forwarded_media_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            follower = self.live_follower(directory, "--backend", "subprocess")
            fifo = Path(directory) / "measured.fifo"
            os.mkfifo(fifo)
            reader = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
            process = mock.Mock()
            process.stdout = io.BytesIO(b"synthetic-media")
            follower._starting_reader_generation = 3
            follower._active_handoff_started_at = time.monotonic()
            output = io.StringIO()
            try:
                with contextlib.redirect_stdout(output):
                    worker = follower.start_streamlink_subprocess_fifo_worker(
                        "rumble",
                        fifo,
                        process,
                    )
                    self.assertTrue(worker.wait_until_stopped(1.0))
                self.assertEqual(b"synthetic-media", os.read(reader, 1024))
            finally:
                os.close(reader)

            events = [
                json.loads(line)
                for line in output.getvalue().splitlines()
                if line.startswith('{"event":"streamlink_handoff_timing"')
            ]
            self.assertEqual(
                ["new_reader_started", "first_media_bytes_received"],
                [event["stage"] for event in events],
            )
            self.assertEqual(3, events[0]["generation"])
            self.assertNotIn("synthetic-media", output.getvalue())

    def test_api_session_automatically_loads_plugin(self):
        loaded = []

        class Plugins:
            def load_path(self, path):
                loaded.append(path)
                return True

        class Streamlink:
            def __init__(self):
                self.plugins = Plugins()

            def set_option(self, key, value):
                pass

        module = types.ModuleType("streamlink")
        module.Streamlink = Streamlink
        with mock.patch.dict(sys.modules, {"streamlink": module}):
            follower = self.follower()
            follower.api_session_obj()
        self.assertEqual([FOLLOWER.PLUGIN_DIR], loaded)

    def test_api_preflight_stream_is_reused_for_player_handoff(self):
        follower = self.follower("--backend", "api")
        stream = object()
        follower._preflight_api_stream = ("rumble", stream)

        with mock.patch.object(follower, "api_session_obj") as session:
            self.assertIs(stream, follower.api_stream("rumble"))

        session.assert_not_called()
        self.assertIsNone(follower._preflight_api_stream)

    def test_segment_boundary_teardown_never_blocks_new_hls_handoff(self):
        for release_delay in (0.20, 0.40):
            with self.subTest(release_delay=release_delay):
                with tempfile.TemporaryDirectory() as directory, LocalHlsFixture() as hls:
                    follower = self.live_follower(
                        directory,
                        "--backend",
                        "api",
                        "--stop-timeout",
                        "1.0",
                    )
                    readers = self.install_fifo_handoff(follower)
                    follower.api_stream = lambda channel: hls.stream(channel)
                    output = io.StringIO()
                    timer = None
                    old_fifo = None
                    old_worker = None
                    try:
                        with contextlib.redirect_stdout(output):
                            self.assertTrue(follower.load_fifo_mode("old"))
                            self.assertTrue(hls.wait_for_request("/old/segment.ts"))
                            old_fifo = follower.current_fifo
                            old_worker = follower.api_worker
                            self.assertIsNotNone(old_worker)

                            hls.release("new")
                            timer = threading.Timer(release_delay, hls.release, args=("old",))
                            timer.start()
                            started = time.monotonic()
                            self.assertTrue(follower.load_fifo_mode("new"))
                            elapsed = time.monotonic() - started

                            self.assertLess(elapsed, 0.15)
                            self.assertTrue(old_worker.stop_event.is_set())
                            self.assertEqual(2, follower.current_reader_generation)
                            self.assertIsNot(old_worker, follower.api_worker)
                            timer.join(timeout=1.0)
                            follower.wait_for_cleanup_threads(1.0)
                            self.assertFalse(old_worker.thread.is_alive())
                            self.assertFalse(old_fifo.exists())
                    finally:
                        hls.release("old")
                        hls.release("new")
                        if timer is not None:
                            timer.join(timeout=1.0)
                        with contextlib.redirect_stdout(output):
                            follower.stop_current_stream()
                            follower.wait_for_cleanup_threads(1.0)
                        self.close_readers(readers)

                    timing = [
                        json.loads(line)
                        for line in output.getvalue().splitlines()
                        if line.startswith('{"event":"streamlink_handoff_timing"')
                    ]
                    stages = {event["stage"] for event in timing}
                    self.assertTrue(
                        {
                            "switch_event_received",
                            "new_stream_resolved",
                            "old_reader_cancellation_requested",
                            "old_reader_teardown_completed",
                            "new_reader_started",
                            "first_media_bytes_received",
                            "mpv_replacement_issued",
                        }.issubset(stages)
                    )
                    self.assertNotIn("127.0.0.1", output.getvalue())
                    self.assertNotIn("/old/", output.getvalue())
                    self.assertNotIn("/new/", output.getvalue())
                    second_handoff = {
                        event["stage"]: event["elapsed_ms"]
                        for event in timing
                        if event["generation"] == 2
                    }
                    for stage in (
                        "new_stream_resolved",
                        "old_reader_cancellation_requested",
                        "new_reader_started",
                        "first_media_bytes_received",
                        "mpv_replacement_issued",
                    ):
                        self.assertLess(second_handoff[stage], 150)
                    self.assertLessEqual(
                        second_handoff["new_stream_resolved"],
                        second_handoff["old_reader_cancellation_requested"],
                    )
                    self.assertLessEqual(
                        second_handoff["old_reader_cancellation_requested"],
                        second_handoff["new_reader_started"],
                    )
                    self.assertLessEqual(
                        second_handoff["new_reader_started"],
                        second_handoff["first_media_bytes_received"],
                    )
                    self.assertGreaterEqual(
                        second_handoff["old_reader_teardown_completed"],
                        round(release_delay * 1000) - 30,
                    )
                    self.assertGreater(
                        second_handoff["old_reader_teardown_completed"],
                        second_handoff["mpv_replacement_issued"],
                    )

    def test_rapid_hls_switches_cancel_and_reap_every_stale_generation(self):
        with tempfile.TemporaryDirectory() as directory, LocalHlsFixture() as hls:
            follower = self.live_follower(
                directory,
                "--backend",
                "api",
                "--stop-timeout",
                "1.0",
            )
            readers = self.install_fifo_handoff(follower)
            follower.api_stream = lambda channel: hls.stream(channel)
            output = io.StringIO()
            stale_workers = []
            stale_fifos = []
            try:
                with contextlib.redirect_stdout(output):
                    self.assertTrue(follower.load_fifo_mode("a"))
                    self.assertTrue(hls.wait_for_request("/a/segment.ts"))
                    stale_workers.append(follower.api_worker)
                    stale_fifos.append(follower.current_fifo)

                    self.assertTrue(follower.load_fifo_mode("b"))
                    self.assertTrue(hls.wait_for_request("/b/segment.ts"))
                    stale_workers.append(follower.api_worker)
                    stale_fifos.append(follower.current_fifo)

                    hls.release("c")
                    self.assertTrue(follower.load_fifo_mode("c"))
                    self.assertEqual(3, follower.current_reader_generation)
                    self.assertTrue(all(worker.stop_event.is_set() for worker in stale_workers))

                    hls.release("a")
                    hls.release("b")
                    follower.wait_for_cleanup_threads(1.0)
                    self.assertTrue(all(not worker.thread.is_alive() for worker in stale_workers))
                    self.assertTrue(all(not fifo.exists() for fifo in stale_fifos))
            finally:
                hls.release("a")
                hls.release("b")
                hls.release("c")
                with contextlib.redirect_stdout(output):
                    follower.stop_current_stream()
                    follower.wait_for_cleanup_threads(1.0)
                self.close_readers(readers)

            with hls.request_lock:
                requested = list(hls.requested)
            self.assertNotIn("/unused/index.m3u8", requested)
            self.assertNotIn("/unused/segment.ts", requested)
            cancellation_events = [
                json.loads(line)
                for line in output.getvalue().splitlines()
                if '"stage":"old_reader_cancellation_requested"' in line
            ]
            self.assertGreaterEqual(len(cancellation_events), 2)

    def test_failed_player_handoff_reaps_old_and_new_hls_readers(self):
        with tempfile.TemporaryDirectory() as directory, LocalHlsFixture() as hls:
            follower = self.live_follower(
                directory,
                "--backend",
                "api",
                "--stop-timeout",
                "1.0",
            )
            readers = self.install_fifo_handoff(follower)
            follower.api_stream = lambda channel: hls.stream(channel)
            output = io.StringIO()
            old_worker = None
            new_worker = None
            old_fifo = None
            new_fifo = None
            try:
                with contextlib.redirect_stdout(output):
                    self.assertTrue(follower.load_fifo_mode("old"))
                    self.assertTrue(hls.wait_for_request("/old/segment.ts"))
                    old_worker = follower.api_worker
                    old_fifo = follower.current_fifo

                    original_start = follower.start_streamlink_api_fifo

                    def capture_start(channel, fifo, stream=None):
                        nonlocal new_worker, new_fifo
                        new_fifo = fifo
                        new_worker = original_start(channel, fifo, stream=stream)
                        return new_worker

                    def fail_handoff(path):
                        readers.append(os.open(path, os.O_RDONLY | os.O_NONBLOCK))
                        raise RuntimeError("synthetic player handoff failure")

                    follower.start_streamlink_api_fifo = capture_start
                    follower.mpv_loadfile_replace.side_effect = fail_handoff
                    with self.assertRaises(RuntimeError):
                        follower.load_fifo_mode("new")

                    self.assertTrue(old_worker.stop_event.is_set())
                    self.assertTrue(new_worker.stop_event.is_set())
                    self.assertEqual(0, follower.current_reader_generation)
                    self.assertIsNone(follower.api_worker)

                    hls.release("old")
                    hls.release("new")
                    follower.wait_for_cleanup_threads(1.0)
                    self.assertFalse(old_worker.thread.is_alive())
                    self.assertFalse(new_worker.thread.is_alive())
                    self.assertFalse(old_fifo.exists())
                    self.assertFalse(new_fifo.exists())
            finally:
                hls.release("old")
                hls.release("new")
                with contextlib.redirect_stdout(output):
                    follower.stop_current_stream()
                    follower.wait_for_cleanup_threads(1.0)
                self.close_readers(readers)

            timing_stages = [
                json.loads(line)["stage"]
                for line in output.getvalue().splitlines()
                if line.startswith('{"event":"streamlink_handoff_timing"')
            ]
            self.assertEqual(1, timing_stages.count("old_reader_cancellation_requested"))
            self.assertEqual(1, timing_stages.count("old_reader_teardown_completed"))

    def test_slow_subprocess_reap_does_not_hold_switch_lock(self):
        class Process:
            def __init__(self):
                self.cancelled = threading.Event()
                self.released = threading.Event()

        with tempfile.TemporaryDirectory() as directory:
            follower = self.live_follower(
                directory,
                "--backend",
                "subprocess",
                "--stop-timeout",
                "1.0",
            )
            old = Process()
            new = Process()
            new_worker = mock.Mock()
            new_worker.wait_until_stopped.return_value = True
            old_fifo = Path(directory) / "old.fifo"
            new_fifo = Path(directory) / "new.fifo"
            old_fifo.touch()
            new_fifo.touch()
            follower.stream_proc = old
            follower.current_fifo = old_fifo
            follower.current_reader_generation = 1
            follower.resolve_stream_url = mock.Mock(return_value="opaque-result")
            follower.ensure_mpv = mock.Mock()
            follower.make_fifo = mock.Mock(return_value=new_fifo)
            follower.start_streamlink_subprocess_fifo = mock.Mock(return_value=new)
            follower.start_streamlink_subprocess_fifo_worker = mock.Mock(
                return_value=new_worker,
            )
            follower.mpv_loadfile_replace = mock.Mock()
            follower.monitor_playback_ready = mock.Mock()
            follower.request_process_stop = lambda proc: proc.cancelled.set()
            follower.wait_process_stopped = lambda proc, _label: proc.released.wait(1.0)
            output = io.StringIO()
            try:
                with contextlib.redirect_stdout(output):
                    started = time.monotonic()
                    self.assertTrue(follower.load_fifo_mode("new"))
                    elapsed = time.monotonic() - started
                    self.assertLess(elapsed, 0.15)
                    self.assertTrue(old.cancelled.is_set())
                    self.assertIs(new, follower.stream_proc)

                    old.released.set()
                    follower.wait_for_cleanup_threads(1.0)
                    self.assertFalse(old_fifo.exists())
            finally:
                new.released.set()
                with contextlib.redirect_stdout(output):
                    follower.stop_current_stream()
                    follower.wait_for_cleanup_threads(1.0)

            self.assertTrue(new.cancelled.is_set())
            self.assertFalse(new_fifo.exists())

    def test_overlap_cancels_old_reader_immediately_after_handoff(self):
        class Process:
            def __init__(self):
                self.cancelled = threading.Event()
                self.released = threading.Event()

        with tempfile.TemporaryDirectory() as directory:
            follower = self.live_follower(
                directory,
                "--backend",
                "subprocess",
                "--handoff",
                "overlap",
                "--handoff-delay",
                "5.0",
                "--stop-timeout",
                "1.0",
            )
            old = Process()
            new = Process()
            new_worker = mock.Mock()
            new_worker.wait_until_stopped.return_value = True
            old_fifo = Path(directory) / "old.fifo"
            new_fifo = Path(directory) / "new.fifo"
            old_fifo.touch()
            new_fifo.touch()
            follower.stream_proc = old
            follower.current_fifo = old_fifo
            follower.current_reader_generation = 1
            follower.resolve_stream_url = mock.Mock(return_value="opaque-result")
            follower.ensure_mpv = mock.Mock()
            follower.make_fifo = mock.Mock(return_value=new_fifo)
            follower.start_streamlink_subprocess_fifo = mock.Mock(return_value=new)
            follower.start_streamlink_subprocess_fifo_worker = mock.Mock(
                return_value=new_worker,
            )
            follower.mpv_loadfile_replace = mock.Mock()
            follower.monitor_playback_ready = mock.Mock()
            follower.request_process_stop = lambda proc: proc.cancelled.set()
            follower.wait_process_stopped = lambda proc, _label: proc.released.wait(1.0)
            output = io.StringIO()
            try:
                with contextlib.redirect_stdout(output):
                    started = time.monotonic()
                    self.assertTrue(follower.load_fifo_mode("new"))
                    elapsed = time.monotonic() - started

                    self.assertLess(elapsed, 0.15)
                    self.assertTrue(old.cancelled.is_set())
                    self.assertIs(new, follower.stream_proc)

                    old.released.set()
                    follower.wait_for_cleanup_threads(1.0)
                    self.assertFalse(old_fifo.exists())
            finally:
                new.released.set()
                with contextlib.redirect_stdout(output):
                    follower.stop_current_stream()
                    follower.wait_for_cleanup_threads(1.0)

            self.assertTrue(new.cancelled.is_set())
            self.assertFalse(new_fifo.exists())

    def test_playback_readiness_timing_is_generation_scoped_and_sanitized(self):
        with tempfile.TemporaryDirectory() as directory:
            follower = self.live_follower(directory)
            follower.current_reader_generation = 7
            expected_path = str(Path(directory) / "stream-7.fifo")
            paths = iter(["old-input", expected_path, expected_path])
            positions = iter([None, 0.0])

            def get_property(name):
                if name == "path":
                    return next(paths)
                self.assertEqual("time-pos", name)
                return next(positions)

            follower.mpv_get_property = mock.Mock(side_effect=get_property)
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                follower.monitor_playback_ready(
                    7,
                    time.monotonic(),
                    expected_path,
                )
                follower.wait_for_cleanup_threads(1.0)

            events = [
                json.loads(line)
                for line in output.getvalue().splitlines()
                if line.startswith('{"event":"streamlink_handoff_timing"')
            ]
            self.assertEqual(["first_playback_ready"], [event["stage"] for event in events])
            self.assertEqual(7, events[0]["generation"])
            self.assertEqual(
                {"event", "stage", "generation", "elapsed_ms", "mode", "backend"},
                set(events[0]),
            )
            self.assertGreaterEqual(events[0]["elapsed_ms"], 90)
            self.assertLess(events[0]["elapsed_ms"], 500)
            self.assertNotIn(expected_path, output.getvalue())

    def test_non_handoff_cleanup_does_not_emit_old_reader_timing(self):
        with tempfile.TemporaryDirectory() as directory:
            follower = self.live_follower(directory)
            fifo = Path(directory) / "current.fifo"
            fifo.touch()
            worker = mock.Mock()
            worker.wait_until_stopped.return_value = True
            follower.api_worker = worker
            follower.current_fifo = fifo
            follower.current_reader_generation = 4
            output = io.StringIO()

            with contextlib.redirect_stdout(output):
                follower.stop_current_stream()
                follower.wait_for_cleanup_threads(1.0)

            worker.request_stop.assert_called_once_with()
            self.assertFalse(fifo.exists())
            self.assertNotIn("old_reader_cancellation_requested", output.getvalue())
            self.assertNotIn("old_reader_teardown_completed", output.getvalue())

    def test_twitch_rumble_kick_twitch_switches_reuse_follower_and_redact_urls(self):
        follower = self.follower()
        events = [
            {"event": "tab_changed", "state_ok": True, "platform": "twitch", "channel": "alpha", "stream_url": "https://www.twitch.tv/alpha"},
            {"event": "tab_changed", "state_ok": True, "platform": "rumble", "channel": "https://rumble.com/c/Fixture", "stream_url": "https://rumble.com/c/Fixture"},
            {"event": "tab_changed", "state_ok": True, "platform": "kick", "channel": "gamma", "stream_url": "https://kick.com/gamma"},
            {"event": "tab_changed", "state_ok": True, "platform": "twitch", "channel": "beta", "stream_url": "https://www.twitch.tv/beta"},
        ]
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            for event in events:
                follower.handle_event(event)
        rendered = output.getvalue()
        self.assertEqual("beta", follower.current_channel)
        self.assertNotIn("rumble.com", rendered)
        self.assertNotIn("twitch.tv", rendered)
        self.assertNotIn("kick.com", rendered)
        self.assertEqual(4, rendered.count('"event":"streamlink_switch"'))

    def test_rumble_dry_run_supports_fifo_and_url_with_both_backends(self):
        event = {
            "event": "tab_changed",
            "state_ok": True,
            "platform": "rumble",
            "channel": "https://rumble.com/c/Fixture",
            "stream_url": "https://rumble.com/c/Fixture",
        }
        for mode in ("fifo", "url"):
            for backend in ("api", "subprocess"):
                with self.subTest(mode=mode, backend=backend):
                    follower = self.follower("--mode", mode, "--backend", backend)
                    output = io.StringIO()
                    with contextlib.redirect_stdout(output):
                        follower.handle_event(event)
                    self.assertEqual("rumble", follower.current_channel)
                    self.assertNotIn("rumble.com", output.getvalue())

    def test_confirmed_offline_stops_once_without_resolving_or_leaking(self):
        follower = self.follower()
        follower.current_channel = "rumble"
        follower.current_url = "https://rumble.com/c/Fixture"
        follower.mpv_proc = mock.Mock()
        follower.mpv_proc.poll.return_value = None
        event = {
            "event": "tab_changed",
            "state_ok": True,
            "is_live": False,
            "platform": "rumble",
            "channel": "https://rumble.com/c/Fixture",
            "stream_url": "https://rumble.com/c/Fixture",
            "display_name": "private-channel-canary",
        }
        output = io.StringIO()
        with tempfile.TemporaryDirectory() as directory:
            follower.ipc_path = Path(directory) / "mpv.sock"
            follower.ipc_path.touch()
            with (
                mock.patch.object(follower, "stop_current_stream") as stop,
                mock.patch.object(follower, "mpv_command") as idle_player,
                mock.patch.object(follower, "load_fifo_mode") as load_fifo,
                mock.patch.object(follower, "load_url_mode") as load_url,
                contextlib.redirect_stdout(output),
            ):
                follower.handle_event(event)
                follower.handle_event(event)

        stop.assert_called_once_with()
        idle_player.assert_called_once_with(["stop"])
        load_fifo.assert_not_called()
        load_url.assert_not_called()
        self.assertIsNone(follower.current_channel)
        self.assertIsNone(follower.current_url)
        rendered = output.getvalue()
        self.assertEqual(1, rendered.count('"event":"streamlink_unavailable"'))
        self.assertIn('"reason":"confirmed_offline"', rendered)
        self.assertNotIn("private-channel-canary", rendered)
        self.assertNotIn("rumble.com", rendered)

    def test_malformed_offline_event_cannot_stop_current_playback(self):
        follower = self.follower()
        follower.current_channel = "alpha"
        follower.current_url = "https://www.twitch.tv/alpha"
        event = {
            "event": "tab_changed",
            "state_ok": True,
            "is_live": False,
            "platform": "rumble",
            "stream_url": "https://rumble.com/c/Fixture?secret=drop",
        }
        stderr = io.StringIO()
        with (
            mock.patch.object(follower, "stop_current_stream") as stop,
            contextlib.redirect_stderr(stderr),
        ):
            follower.handle_event(event)

        stop.assert_not_called()
        self.assertEqual("alpha", follower.current_channel)
        self.assertIn('"stage":"event_rejection"', stderr.getvalue())
        self.assertNotIn("secret=drop", stderr.getvalue())

    def test_subprocess_and_api_failures_do_not_log_urls_or_signed_values(self):
        follower = self.follower()
        follower.args.dry_run = False
        follower._channel_urls = {"rumble": "https://rumble.com/c/Fixture"}
        failure = types.SimpleNamespace(returncode=1, stdout="", stderr="https://cdn.example/signed.m3u8?token=secret")
        stderr = io.StringIO()
        with mock.patch.object(FOLLOWER.subprocess, "run", return_value=failure), contextlib.redirect_stderr(stderr):
            with self.assertRaises(FOLLOWER.FollowerStageError) as raised:
                follower.resolve_stream_url("rumble")
        self.assertEqual(("resolver", "resolver_failed"), (raised.exception.stage, raised.exception.reason))
        rendered = stderr.getvalue()
        self.assertNotIn("rumble.com", rendered)
        self.assertNotIn("token=secret", rendered)
        self.assertNotIn("token=secret", str(raised.exception))

        class BrokenPlugin:
            def __init__(self, *args, **kwargs):
                pass

            def streams(self):
                raise RuntimeError("https://cdn.example/signed.m3u8?token=secret")

        follower.api_session = types.SimpleNamespace(
            resolve_url_no_redirect=lambda url: ("rumble", BrokenPlugin, url)
        )
        with self.assertRaises(FOLLOWER.FollowerStageError) as raised:
            follower.api_stream("rumble")
        self.assertEqual(("resolver", "resolver_failed"), (raised.exception.stage, raised.exception.reason))
        self.assertNotIn("token=secret", str(raised.exception))

    def test_both_fifo_backends_preflight_and_reach_player_handoff(self):
        for backend in ("api", "subprocess"):
            with self.subTest(backend=backend):
                follower = self.follower("--backend", backend)
                follower.args.dry_run = False
                follower._channel_urls = {
                    "rumble": "https://rumble.com/c/Fixture",
                }
                stream = object()
                fifo = Path("/tmp/controlled-rumble.fifo")
                process = mock.Mock()
                process.poll.return_value = None
                worker = mock.Mock()

                with (
                    mock.patch.object(follower, "api_stream", return_value=stream) as resolve,
                    mock.patch.object(
                        follower,
                        "resolve_stream_url",
                        return_value="https://media.example/controlled.m3u8",
                    ) as resolve_subprocess,
                    mock.patch.object(follower, "ensure_mpv") as ensure_mpv,
                    mock.patch.object(follower, "stop_current_stream"),
                    mock.patch.object(follower, "make_fifo", return_value=fifo),
                    mock.patch.object(
                        follower,
                        "start_streamlink_api_fifo",
                        return_value=worker,
                    ) as start_api,
                    mock.patch.object(
                        follower,
                        "start_streamlink_subprocess_fifo",
                        return_value=process,
                    ) as start_subprocess,
                    mock.patch.object(
                        follower,
                        "start_streamlink_subprocess_fifo_worker",
                        return_value=worker,
                    ) as start_subprocess_worker,
                    mock.patch.object(follower, "mpv_loadfile_replace") as handoff,
                ):
                    self.assertTrue(follower.load_fifo_mode("rumble"))

                self.assertEqual(2 if backend == "api" else 0, resolve.call_count)
                self.assertTrue(all(
                    call == mock.call("rumble")
                    for call in resolve.call_args_list
                ))
                if backend == "subprocess":
                    resolve_subprocess.assert_called_once_with("rumble")
                else:
                    resolve_subprocess.assert_not_called()
                ensure_mpv.assert_called_once_with()
                handoff.assert_called_once_with(str(fifo))
                if backend == "api":
                    start_api.assert_called_once_with("rumble", fifo, stream=stream)
                    start_subprocess.assert_not_called()
                    start_subprocess_worker.assert_not_called()
                else:
                    start_subprocess.assert_called_once_with("rumble", fifo)
                    start_subprocess_worker.assert_called_once_with(
                        "rumble",
                        fifo,
                        process,
                    )
                    start_api.assert_not_called()

    def test_no_live_stream_is_diagnostic_and_does_not_start_player(self):
        follower = self.follower()
        follower.args.dry_run = False
        event = {
            "event": "tab_changed",
            "state_ok": True,
            "platform": "rumble",
            "channel": "https://rumble.com/c/Fixture",
            "stream_url": "https://rumble.com/c/Fixture",
            "display_name": "private-channel-canary",
        }
        stderr = io.StringIO()
        with (
            mock.patch.object(
                follower,
                "api_stream",
                side_effect=FOLLOWER.FollowerStageError(
                    "resolver",
                    "no_live_stream",
                ),
            ),
            mock.patch.object(follower, "ensure_mpv") as ensure_mpv,
            contextlib.redirect_stderr(stderr),
            contextlib.redirect_stdout(io.StringIO()) as stdout,
        ):
            follower.handle_event(event)

        ensure_mpv.assert_not_called()
        rendered = stderr.getvalue()
        self.assertIn('"stage":"resolver"', rendered)
        self.assertIn('"reason":"no_live_stream"', rendered)
        self.assertNotIn("private-channel-canary", rendered)
        self.assertNotIn("rumble.com", rendered)
        self.assertNotIn("private-channel-canary", stdout.getvalue())
        self.assertNotIn("rumble.com", stdout.getvalue())

    def test_event_plugin_resolver_and_player_failures_have_bounded_stages(self):
        follower = self.follower("--backend", "subprocess")
        invalid = {
            "event": "tab_changed",
            "state_ok": True,
            "platform": "rumble",
            "stream_url": "https://rumble.com/c/Fixture?secret=drop",
            "display_name": "private-channel-canary",
        }
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            follower.handle_event(invalid)
        rendered = stderr.getvalue()
        self.assertIn('"stage":"event_rejection"', rendered)
        self.assertNotIn("private-channel-canary", rendered)
        self.assertNotIn("rumble.com", rendered)

        for raw, expected in (
            (
                f"noise\n{FOLLOWER.RUMBLE_RESULT_MARKER}no_live_stream\n",
                ("resolver", "no_live_stream"),
            ),
            (
                f"{FOLLOWER.RUMBLE_RESULT_MARKER}resolver_failed\n",
                ("resolver", "resolver_failed"),
            ),
            ("No plugin can handle URL: secret-canary", ("plugin_load", "unavailable")),
        ):
            with self.subTest(expected=expected):
                failure = FOLLOWER.subprocess_failure(raw)
                self.assertEqual(expected, (failure.stage, failure.reason))
                self.assertNotIn("secret-canary", str(failure))

        event = {
            "event": "tab_changed",
            "state_ok": True,
            "platform": "rumble",
            "channel": "https://rumble.com/c/Fixture",
            "stream_url": "https://rumble.com/c/Fixture",
        }
        stderr = io.StringIO()
        with (
            mock.patch.object(
                follower,
                "load_fifo_mode",
                side_effect=RuntimeError("private-player-canary"),
            ),
            contextlib.redirect_stderr(stderr),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            follower.handle_event(event)
        rendered = stderr.getvalue()
        self.assertIn('"stage":"player_handoff"', rendered)
        self.assertIn('"reason":"failed"', rendered)
        self.assertNotIn("private-player-canary", rendered)

    def test_plugin_load_failure_is_typed(self):
        class Plugins:
            @staticmethod
            def load_path(path):
                return False

        class Streamlink:
            def __init__(self):
                self.plugins = Plugins()

        module = types.ModuleType("streamlink")
        module.Streamlink = Streamlink
        with mock.patch.dict(sys.modules, {"streamlink": module}):
            follower = self.follower()
            with self.assertRaises(FOLLOWER.FollowerStageError) as raised:
                follower.api_session_obj()
        self.assertEqual(("plugin_load", "unavailable"), (raised.exception.stage, raised.exception.reason))


if __name__ == "__main__":
    unittest.main()
