#!/usr/bin/env python3
"""Compatibility entrypoint for chatterino-streamlink-follow.

The original follower predates tab-emit's stream_url field and always rebuilds
URLs from --url-template, which defaults to Twitch. This wrapper keeps the
existing follower implementation but makes event.stream_url authoritative, so
Twitch and Kick selected-channel events are resolved through the correct
Streamlink plugin.
"""

from __future__ import annotations

import importlib.util
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit

CHANNEL_KEY_RE = re.compile(r"^[A-Za-z0-9_][A-Za-z0-9_.-]{0,63}$")
PLUGIN_DIR = Path(
    os.environ.get("CHATTERINO_STREAMLINK_PLUGIN_DIR", Path(__file__).with_name("streamlink-plugins"))
)
ALLOWED_STREAM_URL_RE = re.compile(
    r"^https?://(?:www\.)?(?:twitch\.tv|kick\.com)/[A-Za-z0-9_][A-Za-z0-9_.-]{0,63}(?:[/?#].*)?$",
    re.IGNORECASE,
)
RUMBLE_PATH_RE = re.compile(
    r"/(?:c/[A-Za-z0-9][A-Za-z0-9_-]{0,79}/?|"
    r"embed/v[a-z0-9]{1,127}/?|"
    r"v[a-z0-9]{1,127}(?:-[^/?#]*)?\.html)",
    re.IGNORECASE,
)
RUMBLE_RESULT_MARKER = "chatterino-rumble-result:"
RUMBLE_RESULT_CODES = frozenset({"no_live_stream", "resolver_failed"})


class FollowerStageError(RuntimeError):
    def __init__(self, stage: str, reason: str) -> None:
        super().__init__(f"{stage}:{reason}")
        self.stage = stage
        self.reason = reason


def is_rumble_url(value: str) -> bool:
    try:
        parsed = urlsplit(value)
        port = parsed.port
    except ValueError:
        return False
    return (
        parsed.scheme == "https"
        and parsed.netloc in {"rumble.com", "www.rumble.com"}
        and parsed.hostname in {"rumble.com", "www.rumble.com"}
        and parsed.username is None
        and parsed.password is None
        and port is None
        and not parsed.query
        and not parsed.fragment
        and not re.search(r"[\\\x00-\x20\x7f]", parsed.path)
        and not re.search(r"%(?![0-9A-Fa-f]{2})", parsed.path)
        and not re.search(r"%(?:2[fF]|5[cC]|00)", parsed.path)
        and bool(RUMBLE_PATH_RE.fullmatch(parsed.path))
    )


def is_allowed_stream_url(value: str) -> bool:
    return bool(ALLOWED_STREAM_URL_RE.fullmatch(value)) or is_rumble_url(value)


def safe_event(event: dict[str, Any]) -> dict[str, Any]:
    return {
        key: event.get(key)
        for key in ("event", "platform", "state_ok", "is_live", "reason")
        if key in event
    }


def emit_diagnostic(self: Any, stage: str, reason: str) -> None:
    print(
        json.dumps(
            {
                "event": "streamlink_diagnostic",
                "stage": stage,
                "reason": reason,
                "mode": self.args.mode,
                "backend": self.args.backend,
            },
            ensure_ascii=False,
            separators=(",", ":"),
        ),
        file=sys.stderr,
        flush=True,
    )


def streamlink_environment() -> dict[str, str]:
    environment = dict(os.environ)
    environment["CHATTERINO_STREAMLINK_DIAGNOSTICS"] = "1"
    return environment


def subprocess_failure(stderr: str) -> FollowerStageError:
    for code in RUMBLE_RESULT_CODES:
        if f"{RUMBLE_RESULT_MARKER}{code}" in stderr:
            return FollowerStageError("resolver", code)
    lowered = stderr.lower()
    if "no plugin can handle" in lowered or "plugin could not be loaded" in lowered:
        return FollowerStageError("plugin_load", "unavailable")
    return FollowerStageError("resolver", "resolver_failed")


def load_original() -> Any:
    original_path = Path(__file__).with_name("chatterino-streamlink-follow.py")
    spec = importlib.util.spec_from_file_location("_chatterino_streamlink_follow_original", original_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load {original_path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


orig = load_original()


def channel_from_event(self: Any, event: dict[str, Any]) -> str | None:
    stream_url = str(event.get("stream_url") or "").strip()
    if str(event.get("platform") or "").lower() == "rumble" or is_rumble_url(stream_url):
        return "rumble" if is_rumble_url(stream_url) else None
    channel = str(event.get("channel") or "").strip().lstrip("#")
    if not channel or channel.startswith("/"):
        return None
    channel = channel.lower()
    if not CHANNEL_KEY_RE.fullmatch(channel):
        return None
    return channel


def stream_url_from_event(self: Any, event: dict[str, Any], channel: str) -> str:
    stream_url = str(event.get("stream_url") or "").strip()
    if stream_url and is_allowed_stream_url(stream_url):
        return stream_url
    return self.args.url_template.format(channel=channel)


def channel_url(self: Any, channel: str, event: dict[str, Any] | None = None) -> str:
    stream_url = ""
    if event is not None:
        stream_url = str(event.get("_streamlink_url") or event.get("stream_url") or "").strip()
    if not stream_url:
        stream_url = str(getattr(self, "_channel_urls", {}).get(channel, "")).strip()
    if stream_url and is_allowed_stream_url(stream_url):
        return stream_url
    return self.args.url_template.format(channel=channel)


def streamlink_command(self: Any, channel: str, *, stdout: bool = False, stream_url: bool = False) -> list[str]:
    url = channel_url(self, channel)
    cmd = [self.args.streamlink]
    cmd.extend(self.streamlink_args())
    cmd.extend(["--plugin-dir", str(PLUGIN_DIR)])
    if stdout:
        cmd.append("--stdout")
    if stream_url:
        cmd.append("--stream-url")
    cmd.extend([url, self.args.quality])
    return cmd


def api_session_obj(self: Any) -> Any:
    if self.api_session is not None:
        return self.api_session
    try:
        from streamlink import Streamlink

        session = Streamlink()
        if not session.plugins.load_path(PLUGIN_DIR):
            raise FollowerStageError("plugin_load", "unavailable")
        if not self.args.no_streamlink_low_latency_defaults:
            for key, value in orig.API_SESSION_OPTIONS.items():
                session.set_option(key, value)
        self.api_session = session
        return session
    except FollowerStageError:
        raise
    except Exception as exc:
        raise FollowerStageError("plugin_load", "unavailable") from exc


def api_stream(self: Any, channel: str) -> Any:
    preflight = getattr(self, "_preflight_api_stream", None)
    if preflight is not None and preflight[0] == channel:
        self._preflight_api_stream = None
        return preflight[1]
    try:
        session = self.api_session_obj()
        url = channel_url(self, channel)
        plugin_name, plugin_class, resolved_url = session.resolve_url_no_redirect(url)
        options = dict(orig.API_PLUGIN_OPTIONS.get(plugin_name, {}))
        plugin = plugin_class(session, resolved_url, options=options)
        streams = plugin.streams()
    except FollowerStageError:
        raise
    except Exception as exc:
        raise FollowerStageError("resolver", "resolver_failed") from exc
    if not streams:
        reason = getattr(plugin, "chatterino_result", None)
        if reason not in RUMBLE_RESULT_CODES:
            reason = "resolver_failed" if plugin_name == "rumble" else "no_live_stream"
        raise FollowerStageError("resolver", reason)
    stream = streams.get(self.args.quality)
    if stream is None:
        raise FollowerStageError("resolver", "quality_unavailable")
    return stream


def start_streamlink_subprocess_fifo(self: Any, channel: str, fifo: Path) -> subprocess.Popen[bytes]:
    cmd = self.streamlink_command(channel, stdout=True)
    print(
        f"starting streamlink subprocess fifo: quality={self.args.quality}",
        flush=True,
    )
    if self.args.dry_run:
        raise RuntimeError("start_streamlink_subprocess_fifo called in dry-run mode")
    return subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        start_new_session=(orig.os.name == "posix"),
        stderr=subprocess.DEVNULL,
        env=streamlink_environment(),
    )


def resolve_stream_url(self: Any, channel: str) -> str | None:
    cmd = self.streamlink_command(channel, stream_url=True)
    if self.args.verbose or self.args.dry_run:
        print(f"resolving stream: channel={channel} quality={self.args.quality}", flush=True)
    if self.args.dry_run:
        return f"dry-run://{channel}/{self.args.quality}"
    try:
        result = subprocess.run(
            cmd,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=self.args.resolve_timeout,
            env=streamlink_environment(),
        )
    except subprocess.TimeoutExpired as exc:
        raise FollowerStageError("resolver", "timeout") from exc
    except OSError as exc:
        raise FollowerStageError("plugin_load", "unavailable") from exc
    if result.returncode != 0:
        raise subprocess_failure(result.stderr)
    stream_url = result.stdout.strip().splitlines()[-1] if result.stdout.strip() else ""
    if not stream_url:
        raise FollowerStageError("resolver", "empty_result")
    return stream_url


def load_fifo_mode(self: Any, channel: str) -> bool:
    generation, _started_at, owns_timing = self.ensure_handoff_timing()
    try:
        if self.args.dry_run:
            print(
                f"dry-run streamlink {self.args.backend} fifo: quality={self.args.quality}",
                flush=True,
            )
            return True
        # Resolve before the original path creates a FIFO or starts mpv. This
        # prevents rejected/offline selections from leaving a fresh player at
        # its idle splash. Use the selected backend for that check. Cache the
        # API result so its original handoff does not perform a duplicate
        # provider resolution; the subprocess check is intentionally
        # side-effect-free and its stdout is never logged.
        stream = None
        if self.args.backend == "api":
            stream = self.api_stream(channel)
            self._preflight_api_stream = (channel, stream)
        else:
            self.resolve_stream_url(channel)
        return orig._original_load_fifo_mode(self, channel)
    finally:
        self._preflight_api_stream = None
        if owns_timing:
            self.finish_handoff_timing(generation)


def switch_to(self: Any, channel: str, event: dict[str, Any]) -> None:
    url = channel_url(self, channel, event)
    if not hasattr(self, "_channel_urls"):
        self._channel_urls = {}
    self._channel_urls[channel] = url

    with self.stream_lock:
        now = time.monotonic()
        subprocess_alive = self.stream_proc is not None and self.stream_proc.poll() is None
        api_alive = self.api_worker is not None and self.api_worker.thread.is_alive()
        current_stream_alive = subprocess_alive or api_alive
        if (
            self.current_channel == channel
            and getattr(self, "current_url", None) == url
            and (self.args.mode == "url" or current_stream_alive)
        ):
            if self.args.verbose:
                print("already following selected destination", flush=True)
            return
        if now - self.last_switch_at < self.args.debounce:
            if self.args.verbose:
                print("debounced destination switch", flush=True)
            return
        self.last_switch_at = now
        old = self.current_channel
        print(json.dumps({
            "event": "streamlink_switch",
            "mode": self.args.mode,
            "backend": self.args.backend,
            "handoff": self.args.handoff,
            "platform": event.get("platform"),
            "destination_changed": old != channel,
            "source_event": event.get("event"),
            "reason": event.get("reason"),
        }, ensure_ascii=False, separators=(",", ":")), flush=True)
        generation, _started_at = self.begin_handoff_timing()
        try:
            ok = self.load_url_mode(channel) if self.args.mode == "url" else self.load_fifo_mode(channel)
        except FollowerStageError as exc:
            self.emit_diagnostic(exc.stage, exc.reason)
            return
        except Exception:
            self.emit_diagnostic("player_handoff", "failed")
            return
        finally:
            self.finish_handoff_timing(generation)
        if ok:
            self.current_channel = channel
            self.current_url = url
            self.last_catchup_reload_at = 0.0


def stop_for_unavailable(self: Any) -> None:
    with self.stream_lock:
        if self.current_channel is None:
            return
        self.stop_current_stream()
        fifo = self.current_fifo
        self.current_fifo = None
        if fifo is not None:
            try:
                fifo.unlink()
            except FileNotFoundError:
                pass
        if (
            self.mpv_proc is not None
            and self.mpv_proc.poll() is None
            and self.ipc_path.exists()
        ):
            try:
                self.mpv_command(["stop"])
            except Exception:
                pass
        self.current_channel = None
        self.current_url = None
        self.last_catchup_reload_at = 0.0
        self.reset_playback_reference()
        print(
            json.dumps(
                {
                    "event": "streamlink_unavailable",
                    "platform": "rumble",
                    "reason": "confirmed_offline",
                },
                ensure_ascii=False,
                separators=(",", ":"),
            ),
            flush=True,
        )


def handle_event(self: Any, event: dict[str, Any]) -> None:
    if not self.should_handle(event):
        if self.args.print_all:
            print("recv: " + json.dumps(safe_event(event), ensure_ascii=False, separators=(",", ":")), flush=True)
        return
    if (
        str(event.get("platform") or "").lower() == "rumble"
        and event.get("is_live") is False
    ):
        if self.channel_from_event(event) is None:
            self.emit_diagnostic("event_rejection", "invalid_locator")
            return
        self.stop_for_unavailable()
        return
    channel = self.channel_from_event(event)
    if channel is None:
        self.emit_diagnostic("event_rejection", "invalid_locator")
        return
    url = self.stream_url_from_event(event, channel)
    if not hasattr(self, "_channel_urls"):
        self._channel_urls = {}
    self._channel_urls[channel] = url
    patched_event = dict(event)
    patched_event["_streamlink_url"] = url
    self.switch_to(channel, patched_event)


orig._original_load_fifo_mode = orig.MpvStreamlinkFollower.load_fifo_mode
orig.MpvStreamlinkFollower.emit_diagnostic = emit_diagnostic
orig.MpvStreamlinkFollower.channel_from_event = channel_from_event
orig.MpvStreamlinkFollower.stream_url_from_event = stream_url_from_event
orig.MpvStreamlinkFollower.streamlink_command = streamlink_command
orig.MpvStreamlinkFollower.api_session_obj = api_session_obj
orig.MpvStreamlinkFollower.api_stream = api_stream
orig.MpvStreamlinkFollower.start_streamlink_subprocess_fifo = start_streamlink_subprocess_fifo
orig.MpvStreamlinkFollower.resolve_stream_url = resolve_stream_url
orig.MpvStreamlinkFollower.load_fifo_mode = load_fifo_mode
orig.MpvStreamlinkFollower.switch_to = switch_to
orig.MpvStreamlinkFollower.stop_for_unavailable = stop_for_unavailable
orig.MpvStreamlinkFollower.handle_event = handle_event


if __name__ == "__main__":
    raise SystemExit(orig.main(sys.argv[1:]))
