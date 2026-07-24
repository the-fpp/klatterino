#!/usr/bin/env python3
"""Follow Chatterino tab changes with Streamlink and a persistent mpv window.

Default mode uses Streamlink's Python API in the already-running controller:
  Streamlink API -> FIFO -> persistent mpv

That avoids launching a fresh Streamlink CLI process on every tab switch while
keeping Streamlink in the data path for low-latency HLS behavior.
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import hashlib
import json
import os
import re
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from contextlib import suppress
from pathlib import Path
from typing import Any, BinaryIO

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
TWITCH_LOGIN_RE = re.compile(r"^[A-Za-z0-9_]{1,25}$")

DEFAULT_STREAMLINK_ARGS = [
    "--twitch-low-latency",
    "--twitch-proxy-playlist-exclude=knut",
    "--twitch-proxy-playlist=https://eu.luminous.dev,https://lb-eu.cdn-perfprod.com",
    "--kick-low-latency",
    "--hls-segment-stream-data",
    "--hls-live-edge", "1",
    "--stream-segment-timeout", "1",
    "--hls-playlist-reload-time", "live-edge",
    "--stream-segment-threads", "2",
]

DEFAULT_MPV_ARGS = [
    "--panscan=1",
    "--cache=no",
    "--profile=low-latency",
    "--audio-pitch-correction=yes",
    "--video-sync=audio",
]

API_SESSION_OPTIONS: dict[str, Any] = {
    "hls-segment-stream-data": True,
    "hls-live-edge": 1,
    "stream-segment-timeout": 1,
    "hls-playlist-reload-time": "live-edge",
    "stream-segment-threads": 2,
}

API_PLUGIN_OPTIONS: dict[str, dict[str, Any]] = {
    "twitch": {
        "low-latency": True,
        "proxy-playlist-exclude": ["knut"],
        "proxy-playlist": [
            "https://eu.luminous.dev",
            "https://lb-eu.cdn-perfprod.com",
        ],
    },
    "kick": {
        "low-latency": True,
    },
}

CLIENTS: set[asyncio.StreamWriter] = set()
CLIENT_TASKS: set[asyncio.Task[None]] = set()


def shell_join(args: list[str]) -> str:
    return " ".join(subprocess.list2cmdline([a]) for a in args)


def clamp(value: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, value))


class ApiStreamWorker:
    def __init__(
        self,
        owner: "MpvStreamlinkFollower",
        channel: str,
        fifo: Path,
        stream: Any | None = None,
        reader: BinaryIO | None = None,
    ) -> None:
        self.owner = owner
        self.channel = channel
        self.fifo = fifo
        self.stream = stream
        self.reader = reader
        self.generation = getattr(owner, "_starting_reader_generation", 0)
        self.handoff_started_at = (
            getattr(owner, "_active_handoff_started_at", None)
            or time.monotonic()
        )
        self.stop_event = threading.Event()
        self.thread = threading.Thread(
            target=self.run,
            name=f"streamlink-api-generation-{self.generation}",
            daemon=True,
        )
        self.fd: BinaryIO | None = None
        self.error: BaseException | None = None
        self.first_media_reported = False
        self.lag_lock = threading.Lock()
        self.write_lag_score = 0.0
        self.last_write_block = 0.0

    def start(self) -> None:
        self.thread.start()

    def request_stop(self) -> None:
        self.stop_event.set()
        fd = self.fd
        if fd is not None:
            with suppress(Exception):
                fd.close()

    def wait_until_stopped(self, timeout: float | None = None) -> bool:
        self.thread.join(timeout=timeout)
        return not self.thread.is_alive()

    def stop(self, timeout: float) -> None:
        self.request_stop()
        if not self.wait_until_stopped(timeout) and self.owner.args.verbose:
            print("api worker still exiting", file=sys.stderr, flush=True)

    def add_write_block_lag(self, duration: float) -> None:
        excess = duration - self.owner.args.catchup_write_block_threshold
        if excess <= 0:
            return
        with self.lag_lock:
            self.write_lag_score = min(
                self.owner.args.catchup_max_lag_score,
                self.write_lag_score + excess,
            )
            self.last_write_block = time.monotonic()

    def lag_score(self) -> float:
        with self.lag_lock:
            return self.write_lag_score

    def reduce_lag_score(self, amount: float) -> None:
        if amount <= 0:
            return
        with self.lag_lock:
            self.write_lag_score = max(0.0, self.write_lag_score - amount)

    def clear_lag_score(self) -> None:
        with self.lag_lock:
            self.write_lag_score = 0.0

    def run(self) -> None:
        try:
            self.owner.emit_handoff_timing(
                "new_reader_started",
                self.generation,
                self.handoff_started_at,
            )
            if self.reader is not None:
                fd = self.reader
            else:
                stream = self.stream if self.stream is not None else self.owner.api_stream(self.channel)
                if self.stop_event.is_set():
                    return
                fd = stream.open()
            self.fd = fd
            with open(self.fifo, "wb", buffering=0) as out:
                while not self.stop_event.is_set():
                    data = fd.read(self.owner.args.api_read_size)
                    if not data:
                        break
                    if not self.first_media_reported:
                        self.first_media_reported = True
                        self.owner.emit_handoff_timing(
                            "first_media_bytes_received",
                            self.generation,
                            self.handoff_started_at,
                        )
                    start = time.monotonic()
                    out.write(data)
                    self.add_write_block_lag(time.monotonic() - start)
        except BrokenPipeError:
            pass
        except ValueError:
            if not self.stop_event.is_set():
                raise
        except BaseException as exc:
            self.error = exc
            print("streamlink api worker failed", file=sys.stderr, flush=True)
        finally:
            fd = self.fd
            self.fd = None
            if fd is not None:
                with suppress(Exception):
                    fd.close()


class MpvStreamlinkFollower:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.current_channel: str | None = None
        self.last_switch_at = 0.0
        self.last_catchup_reload_at = 0.0
        self.last_catchup_status_at = 0.0
        self.current_mpv_speed = 1.0
        self.playback_ref_wall: float | None = None
        self.playback_ref_pos: float | None = None
        self.avsync_bad_since: float | None = None
        self.mpv_proc: subprocess.Popen[bytes] | None = None
        self.stream_proc: subprocess.Popen[bytes] | None = None
        self.api_worker: ApiStreamWorker | None = None
        self.current_fifo: Path | None = None
        self.switch_seq = 0
        self.handoff_generation = 0
        self.current_reader_generation = 0
        self._active_handoff_generation: int | None = None
        self._active_handoff_started_at: float | None = None
        self._starting_reader_generation = 0
        self.cleanup_threads: set[threading.Thread] = set()
        self.cleanup_lock = threading.Lock()
        self.api_session: Any | None = None
        self.stream_lock = threading.RLock()
        self.catchup_stop_event = threading.Event()
        self.catchup_thread: threading.Thread | None = None
        self.ipc_path = Path(args.mpv_ipc).expanduser() if args.mpv_ipc else self.default_ipc_path()
        self.fifo_dir = Path(args.fifo_dir).expanduser() if args.fifo_dir else self.default_fifo_dir()

    def emit_handoff_timing(
        self,
        stage: str,
        generation: int,
        started_at: float,
    ) -> None:
        elapsed_ms = max(0, round((time.monotonic() - started_at) * 1000))
        print(
            json.dumps(
                {
                    "event": "streamlink_handoff_timing",
                    "stage": stage,
                    "generation": generation,
                    "elapsed_ms": elapsed_ms,
                    "mode": self.args.mode,
                    "backend": self.args.backend,
                },
                ensure_ascii=False,
                separators=(",", ":"),
            ),
            flush=True,
        )

    def begin_handoff_timing(self) -> tuple[int, float]:
        self.handoff_generation += 1
        generation = self.handoff_generation
        started_at = time.monotonic()
        self._active_handoff_generation = generation
        self._active_handoff_started_at = started_at
        self.emit_handoff_timing(
            "switch_event_received",
            generation,
            started_at,
        )
        return generation, started_at

    def ensure_handoff_timing(self) -> tuple[int, float, bool]:
        if (
            self._active_handoff_generation is not None
            and self._active_handoff_started_at is not None
        ):
            return (
                self._active_handoff_generation,
                self._active_handoff_started_at,
                False,
            )
        generation, started_at = self.begin_handoff_timing()
        return generation, started_at, True

    def finish_handoff_timing(self, generation: int) -> None:
        if self._active_handoff_generation == generation:
            self._active_handoff_generation = None
            self._active_handoff_started_at = None

    @staticmethod
    def default_ipc_path() -> Path:
        runtime_dir = os.environ.get("XDG_RUNTIME_DIR")
        if runtime_dir:
            return Path(runtime_dir) / "chatterino-streamlink-follow.mpv.sock"
        return Path(tempfile.gettempdir()) / f"chatterino-streamlink-follow-{os.getuid()}.mpv.sock"

    @staticmethod
    def default_fifo_dir() -> Path:
        runtime_dir = os.environ.get("XDG_RUNTIME_DIR")
        if runtime_dir:
            return Path(runtime_dir) / "chatterino-streamlink-follow"
        return Path(tempfile.gettempdir()) / f"chatterino-streamlink-follow-{os.getuid()}"

    def should_handle(self, event: dict[str, Any]) -> bool:
        return event.get("event") in self.args.events and bool(event.get("state_ok"))

    def channel_from_event(self, event: dict[str, Any]) -> str | None:
        channel = str(event.get("channel") or "").strip().lstrip("#")
        if not channel or channel.startswith("/"):
            return None
        channel = channel.lower()
        if not TWITCH_LOGIN_RE.fullmatch(channel):
            return None
        return channel

    def streamlink_args(self) -> list[str]:
        args: list[str] = []
        if not self.args.no_streamlink_low_latency_defaults:
            args.extend(DEFAULT_STREAMLINK_ARGS)
        args.extend(self.args.streamlink_arg or [])
        return args

    def mpv_args(self) -> list[str]:
        args: list[str] = []
        if not self.args.no_mpv_low_latency_defaults:
            args.extend(DEFAULT_MPV_ARGS)
        args.extend(self.args.mpv_arg or [])
        return args

    def streamlink_command(self, channel: str, *, stdout: bool = False, stream_url: bool = False) -> list[str]:
        url = self.args.url_template.format(channel=channel)
        cmd = [self.args.streamlink]
        cmd.extend(self.streamlink_args())
        if stdout:
            cmd.append("--stdout")
        if stream_url:
            cmd.append("--stream-url")
        cmd.extend([url, self.args.quality])
        return cmd

    def api_session_obj(self) -> Any:
        if self.api_session is not None:
            return self.api_session
        from streamlink import Streamlink

        session = Streamlink()
        if not self.args.no_streamlink_low_latency_defaults:
            for key, value in API_SESSION_OPTIONS.items():
                session.set_option(key, value)
        self.api_session = session
        return session

    def api_stream(self, channel: str) -> Any:
        session = self.api_session_obj()
        url = self.args.url_template.format(channel=channel)
        plugin_name, plugin_class, resolved_url = session.resolve_url(url)
        options = dict(API_PLUGIN_OPTIONS.get(plugin_name, {}))
        plugin = plugin_class(session, resolved_url, options=options)
        streams = plugin.streams()
        if not streams:
            raise RuntimeError(f"no streams for {channel}")
        stream = streams.get(self.args.quality)
        if stream is None:
            choices = ",".join(sorted(streams.keys()))
            raise RuntimeError(f"quality {self.args.quality!r} not found for {channel}; choices={choices}")
        return stream

    def ensure_mpv(self) -> None:
        if self.args.dry_run:
            return
        if self.mpv_proc is not None and self.mpv_proc.poll() is None and self.ipc_path.exists():
            return
        self.shutdown_mpv()
        with suppress(FileNotFoundError):
            self.ipc_path.unlink()
        cmd = [
            self.args.mpv,
            "--idle=yes",
            "--force-window=yes",
            "--pause=no",
            "--keep-open=no",
            f"--input-ipc-server={self.ipc_path}",
        ]
        cmd.extend(self.mpv_args())
        print("starting mpv: " + shell_join(cmd), flush=True)
        self.mpv_proc = subprocess.Popen(cmd, start_new_session=(os.name == "posix"))
        self.wait_for_ipc()
        self.current_mpv_speed = 1.0
        self.reset_playback_reference()

    def wait_for_ipc(self) -> None:
        deadline = time.monotonic() + self.args.mpv_start_timeout
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            if self.mpv_proc is not None and self.mpv_proc.poll() is not None:
                raise RuntimeError(f"mpv exited while starting, status={self.mpv_proc.returncode}")
            try:
                self.mpv_command(["get_property", "mpv-version"], read_response=True)
                return
            except (FileNotFoundError, ConnectionRefusedError, TimeoutError, OSError) as exc:
                last_error = exc
                time.sleep(0.05)
        raise TimeoutError(f"mpv IPC socket did not become ready at {self.ipc_path}: {last_error}")

    def mpv_command(self, command: list[Any], *, read_response: bool = False) -> Any:
        payload = json.dumps({"command": command}, separators=(",", ":")).encode("utf-8") + b"\n"
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(self.args.mpv_ipc_timeout)
            s.connect(str(self.ipc_path))
            s.sendall(payload)
            if not read_response:
                return None
            data = bytearray()
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                data.extend(chunk)
                if b"\n" in chunk:
                    break
            line = bytes(data).splitlines()[0] if data else b""
            if not line:
                raise TimeoutError("mpv IPC returned no response")
            return json.loads(line.decode("utf-8", errors="replace"))

    def mpv_get_property(self, name: str) -> Any:
        response = self.mpv_command(["get_property", name], read_response=True)
        if isinstance(response, dict) and response.get("error") == "success":
            return response.get("data")
        return None

    def mpv_show_text(self, text: str, duration_ms: int) -> None:
        self.mpv_command(["show-text", text, duration_ms])

    def mpv_set_speed(self, speed: float, *, lag: float | None = None, source: str | None = None, avsync: float | None = None) -> None:
        speed = round(speed, 3)
        if abs(speed - self.current_mpv_speed) < 0.002:
            return
        self.mpv_command(["set_property", "speed", speed])
        self.current_mpv_speed = speed
        if self.args.verbose:
            print(f"catchup: mpv speed={speed:.3f}", flush=True)
        if self.args.catchup_osd:
            parts = [f"speed {speed:.3f}x"]
            if lag is not None:
                parts.append(f"lag {lag:.2f}s")
            if avsync is not None:
                parts.append(f"av {avsync:+.3f}s")
            if source:
                parts.append(source)
            with suppress(Exception):
                self.mpv_show_text("  ".join(parts), self.args.catchup_osd_ms)

    def mpv_unpause(self) -> None:
        if self.args.force_unpause:
            with suppress(Exception):
                self.mpv_command(["set_property", "pause", False])

    def reset_playback_reference(self) -> None:
        self.playback_ref_wall = None
        self.playback_ref_pos = None
        self.avsync_bad_since = None

    def mpv_loadfile_replace(self, path: str) -> None:
        # mpv IPC loadfile args are: url, flags, playlist-index, options.
        # Use -1 for the playlist-index placeholder so the per-file options are
        # parsed as options instead of as the index argument.
        if self.args.force_unpause:
            self.mpv_command(["loadfile", path, "replace", -1, "pause=no,keep-open=no"])
            self.mpv_unpause()
        else:
            self.mpv_command(["loadfile", path, "replace"])
        self.current_mpv_speed = 1.0
        self.reset_playback_reference()

    def make_fifo(self, channel: str) -> Path:
        self.fifo_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
        self.switch_seq += 1
        fifo = self.fifo_dir / f"stream-{self.switch_seq}-{channel}.fifo"
        with suppress(FileNotFoundError):
            fifo.unlink()
        os.mkfifo(fifo, 0o600)
        return fifo

    def start_streamlink_subprocess_fifo(self, channel: str, fifo: Path) -> subprocess.Popen[bytes]:
        cmd = self.streamlink_command(channel, stdout=True)
        print("starting streamlink subprocess fifo: " + shell_join(cmd), flush=True)
        if self.args.dry_run:
            raise RuntimeError("start_streamlink_subprocess_fifo called in dry-run mode")
        return subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            start_new_session=(os.name == "posix"),
        )

    def start_streamlink_subprocess_fifo_worker(
        self,
        channel: str,
        fifo: Path,
        proc: subprocess.Popen[bytes],
    ) -> ApiStreamWorker:
        if proc.stdout is None:
            raise RuntimeError("streamlink subprocess stdout pipe unavailable")
        worker = ApiStreamWorker(self, channel, fifo, reader=proc.stdout)
        worker.start()
        return worker

    def start_streamlink_api_fifo(self, channel: str, fifo: Path, stream: Any | None = None) -> ApiStreamWorker:
        print(
            f"starting streamlink api fifo: generation={self._starting_reader_generation} quality={self.args.quality}",
            flush=True,
        )
        worker = ApiStreamWorker(self, channel, fifo, stream=stream)
        worker.start()
        return worker

    def request_process_stop(self, proc: subprocess.Popen[bytes] | None) -> None:
        if proc is None or proc.poll() is not None:
            return
        with suppress(ProcessLookupError):
            if os.name == "posix":
                os.killpg(proc.pid, signal.SIGTERM)
            else:
                proc.terminate()

    def wait_process_stopped(
        self,
        proc: subprocess.Popen[bytes] | None,
        label: str,
    ) -> None:
        if proc is None or proc.poll() is not None:
            return
        try:
            proc.wait(timeout=self.args.stop_timeout)
        except subprocess.TimeoutExpired:
            print(
                f"{label} did not exit after {self.args.stop_timeout:.1f}s; killing",
                file=sys.stderr,
                flush=True,
            )
            with suppress(ProcessLookupError):
                if os.name == "posix":
                    os.killpg(proc.pid, signal.SIGKILL)
                else:
                    proc.kill()
            with suppress(Exception):
                proc.wait(timeout=1)

    def request_reader_cleanup(
        self,
        proc: subprocess.Popen[bytes] | None,
        worker: ApiStreamWorker | None,
        fifo: Path | None,
        *,
        reader_generation: int,
        handoff_generation: int | None = None,
        handoff_started_at: float | None = None,
        report_old_reader_handoff: bool = False,
        delay: float = 0.0,
    ) -> threading.Thread | None:
        if proc is None and worker is None:
            if fifo is not None:
                with suppress(FileNotFoundError):
                    fifo.unlink()
            return None

        def request_stop() -> None:
            if (
                report_old_reader_handoff
                and handoff_generation is not None
                and handoff_started_at is not None
            ):
                self.emit_handoff_timing(
                    "old_reader_cancellation_requested",
                    handoff_generation,
                    handoff_started_at,
                )
            self.request_process_stop(proc)
            if worker is not None:
                worker.request_stop()

        if delay <= 0:
            request_stop()

        def reap() -> None:
            try:
                if delay > 0:
                    time.sleep(delay)
                    request_stop()
                self.wait_process_stopped(proc, "stale streamlink subprocess")
                if worker is not None:
                    if not worker.wait_until_stopped(self.args.stop_timeout):
                        worker.wait_until_stopped()
                if fifo is not None:
                    with suppress(FileNotFoundError):
                        fifo.unlink()
                if (
                    report_old_reader_handoff
                    and handoff_generation is not None
                    and handoff_started_at is not None
                ):
                    self.emit_handoff_timing(
                        "old_reader_teardown_completed",
                        handoff_generation,
                        handoff_started_at,
                    )
            finally:
                with self.cleanup_lock:
                    self.cleanup_threads.discard(threading.current_thread())

        thread = threading.Thread(
            target=reap,
            name=f"streamlink-reader-reaper-{reader_generation}",
            daemon=True,
        )
        with self.cleanup_lock:
            self.cleanup_threads.add(thread)
        thread.start()
        return thread

    def wait_for_cleanup_threads(self, timeout: float | None = None) -> None:
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            with self.cleanup_lock:
                threads = list(self.cleanup_threads)
            if not threads:
                return
            for thread in threads:
                remaining = (
                    None
                    if deadline is None
                    else max(0.0, deadline - time.monotonic())
                )
                thread.join(timeout=remaining)
            if deadline is not None and time.monotonic() >= deadline:
                return

    def stop_process(self, proc: subprocess.Popen[bytes] | None, label: str) -> None:
        if proc is None or proc.poll() is not None:
            return
        if self.args.verbose:
            print(f"stopping {label} pid={proc.pid}", flush=True)
        self.request_process_stop(proc)
        self.wait_process_stopped(proc, label)

    def stop_current_stream(self) -> None:
        proc = self.stream_proc
        worker = self.api_worker
        fifo = self.current_fifo
        generation = self.current_reader_generation
        self.stream_proc = None
        self.api_worker = None
        self.current_fifo = None
        self.current_reader_generation = 0
        cleanup = self.request_reader_cleanup(
            proc,
            worker,
            fifo,
            reader_generation=generation,
        )
        if cleanup is not None:
            cleanup.join(timeout=self.args.stop_timeout + 1.0)

    def resolve_stream_url(self, channel: str) -> str | None:
        cmd = self.streamlink_command(channel, stream_url=True)
        if self.args.verbose or self.args.dry_run:
            print("resolving: " + shell_join(cmd), flush=True)
        if self.args.dry_run:
            return f"dry-run://{channel}/{self.args.quality}"
        try:
            result = subprocess.run(cmd, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=self.args.resolve_timeout)
        except subprocess.TimeoutExpired:
            print(f"streamlink URL resolve timed out for {channel}", file=sys.stderr, flush=True)
            return None
        if result.returncode != 0:
            print(f"streamlink URL resolve failed for {channel}: {result.stderr.strip()}", file=sys.stderr, flush=True)
            return None
        stream_url = result.stdout.strip().splitlines()[-1] if result.stdout.strip() else ""
        if not stream_url:
            print(f"streamlink returned no stream URL for {channel}", file=sys.stderr, flush=True)
            return None
        return stream_url

    def monitor_playback_ready(
        self,
        generation: int,
        handoff_started_at: float,
        expected_path: str,
    ) -> None:
        if self.args.dry_run:
            return

        def monitor() -> None:
            try:
                deadline = time.monotonic() + self.args.mpv_start_timeout
                while time.monotonic() < deadline:
                    if self.current_reader_generation != generation:
                        return
                    try:
                        current_path = self.mpv_get_property("path")
                        position = (
                            self.mpv_get_property("time-pos")
                            if current_path == expected_path
                            else None
                        )
                    except Exception:
                        position = None
                    if isinstance(position, (int, float)):
                        self.emit_handoff_timing(
                            "first_playback_ready",
                            generation,
                            handoff_started_at,
                        )
                        return
                    time.sleep(0.05)
                if self.current_reader_generation == generation:
                    self.emit_handoff_timing(
                        "first_playback_timeout",
                        generation,
                        handoff_started_at,
                    )
            finally:
                with self.cleanup_lock:
                    self.cleanup_threads.discard(threading.current_thread())

        thread = threading.Thread(
            target=monitor,
            name=f"mpv-playback-ready-{generation}",
            daemon=True,
        )
        with self.cleanup_lock:
            self.cleanup_threads.add(thread)
        thread.start()

    def load_url_mode(self, channel: str) -> bool:
        generation, started_at, owns_timing = self.ensure_handoff_timing()
        try:
            stream_url = self.resolve_stream_url(channel)
            if stream_url is None:
                return False
            self.emit_handoff_timing(
                "new_stream_resolved",
                generation,
                started_at,
            )
            if self.args.dry_run:
                print(f"dry-run mpv loadfile replace: {stream_url}", flush=True)
                return True
            self.ensure_mpv()
            self.emit_handoff_timing(
                "mpv_replacement_issued",
                generation,
                started_at,
            )
            self.mpv_loadfile_replace(stream_url)
            self.current_reader_generation = generation
            self.monitor_playback_ready(generation, started_at, stream_url)
            return True
        finally:
            if owns_timing:
                self.finish_handoff_timing(generation)

    def load_fifo_mode(self, channel: str) -> bool:
        generation, started_at, owns_timing = self.ensure_handoff_timing()
        try:
            if self.args.dry_run:
                if self.args.backend == "api":
                    print(
                        f"dry-run streamlink api fifo: generation={generation} quality={self.args.quality}",
                        flush=True,
                    )
                else:
                    print(
                        "dry-run streamlink subprocess fifo: "
                        + shell_join(self.streamlink_command(channel, stdout=True)),
                        flush=True,
                    )
                return True

            new_api_stream: Any | None = None
            if self.args.backend == "api":
                try:
                    new_api_stream = self.api_stream(channel)
                except Exception:
                    print(
                        "not switching: stream unavailable/offline",
                        file=sys.stderr,
                        flush=True,
                    )
                    return False
            self.emit_handoff_timing(
                "new_stream_resolved",
                generation,
                started_at,
            )

            self.ensure_mpv()
            old_proc = self.stream_proc
            old_worker = self.api_worker
            old_fifo = self.current_fifo
            old_generation = self.current_reader_generation

            if self.args.handoff == "single":
                self.stream_proc = None
                self.api_worker = None
                self.current_fifo = None
                self.current_reader_generation = 0
                self.request_reader_cleanup(
                    old_proc,
                    old_worker,
                    old_fifo,
                    reader_generation=old_generation,
                    handoff_generation=generation,
                    handoff_started_at=started_at,
                    report_old_reader_handoff=True,
                )
                old_proc = None
                old_worker = None
                old_fifo = None

            fifo = self.make_fifo(channel)
            new_proc: subprocess.Popen[bytes] | None = None
            new_worker: ApiStreamWorker | None = None
            self._starting_reader_generation = generation
            try:
                if self.args.backend == "api":
                    new_worker = self.start_streamlink_api_fifo(
                        channel,
                        fifo,
                        stream=new_api_stream,
                    )
                else:
                    new_proc = self.start_streamlink_subprocess_fifo(channel, fifo)
                    new_worker = self.start_streamlink_subprocess_fifo_worker(
                        channel,
                        fifo,
                        new_proc,
                    )
                self.emit_handoff_timing(
                    "mpv_replacement_issued",
                    generation,
                    started_at,
                )
                self.mpv_loadfile_replace(str(fifo))
            except Exception:
                self.request_reader_cleanup(
                    new_proc,
                    new_worker,
                    fifo,
                    reader_generation=generation,
                    handoff_generation=generation,
                    handoff_started_at=started_at,
                )
                raise
            finally:
                self._starting_reader_generation = 0

            self.stream_proc = new_proc
            self.api_worker = new_worker
            self.current_fifo = fifo
            self.current_reader_generation = generation
            self.monitor_playback_ready(generation, started_at, str(fifo))

            if self.args.handoff == "overlap":
                self.request_reader_cleanup(
                    old_proc,
                    old_worker,
                    old_fifo,
                    reader_generation=old_generation,
                    handoff_generation=generation,
                    handoff_started_at=started_at,
                    report_old_reader_handoff=True,
                )
            return True
        finally:
            if owns_timing:
                self.finish_handoff_timing(generation)

    def switch_to(self, channel: str, event: dict[str, Any]) -> None:
        with self.stream_lock:
            now = time.monotonic()
            subprocess_alive = self.stream_proc is not None and self.stream_proc.poll() is None
            api_alive = self.api_worker is not None and self.api_worker.thread.is_alive()
            current_stream_alive = subprocess_alive or api_alive
            if self.current_channel == channel and (self.args.mode == "url" or current_stream_alive):
                if self.args.verbose:
                    print(f"already following {channel}", flush=True)
                return
            if now - self.last_switch_at < self.args.debounce:
                if self.args.verbose:
                    print(f"debounced switch to {channel}", flush=True)
                return
            self.last_switch_at = now
            old = self.current_channel
            print(json.dumps({
                "event": "streamlink_switch",
                "mode": self.args.mode,
                "backend": self.args.backend,
                "handoff": self.args.handoff,
                "old_channel": old,
                "channel": channel,
                "display_name": event.get("display_name"),
                "source_event": event.get("event"),
                "reason": event.get("reason"),
            }, ensure_ascii=False, separators=(",", ":")), flush=True)
            ok = self.load_url_mode(channel) if self.args.mode == "url" else self.load_fifo_mode(channel)
            if ok:
                self.current_channel = channel
                self.last_catchup_reload_at = 0.0

    def handle_event(self, event: dict[str, Any]) -> None:
        if not self.should_handle(event):
            if self.args.print_all:
                print("recv: " + json.dumps(event, ensure_ascii=False, separators=(",", ":")), flush=True)
            return
        channel = self.channel_from_event(event)
        if channel is None:
            if self.args.verbose:
                print("ignoring non-channel event: " + json.dumps(event, ensure_ascii=False, separators=(",", ":")), flush=True)
            return
        self.switch_to(channel, event)

    def mpv_cache_duration(self) -> float | None:
        if self.mpv_proc is None or self.mpv_proc.poll() is not None or not self.ipc_path.exists():
            return None
        for prop in ("demuxer-cache-duration", "cache-duration"):
            with suppress(Exception):
                value = self.mpv_get_property(prop)
                if isinstance(value, (int, float)) and value >= 0:
                    return float(value)
        with suppress(Exception):
            state = self.mpv_get_property("demuxer-cache-state")
            if isinstance(state, dict):
                value = state.get("cache-duration")
                if isinstance(value, (int, float)) and value >= 0:
                    return float(value)
        return None

    def mpv_avsync(self) -> float | None:
        if self.mpv_proc is None or self.mpv_proc.poll() is not None or not self.ipc_path.exists():
            return None
        with suppress(Exception):
            value = self.mpv_get_property("avsync")
            if isinstance(value, (int, float)):
                return float(value)
        return None

    def mpv_drift_lag(self) -> float | None:
        if self.mpv_proc is None or self.mpv_proc.poll() is not None or not self.ipc_path.exists():
            return None
        with suppress(Exception):
            pos = self.mpv_get_property("time-pos")
            if not isinstance(pos, (int, float)) or pos < 0:
                return None
            now = time.monotonic()
            if self.playback_ref_wall is None or self.playback_ref_pos is None:
                self.playback_ref_wall = now
                self.playback_ref_pos = float(pos)
                return 0.0
            lag = (now - self.playback_ref_wall) - (float(pos) - self.playback_ref_pos)
            if lag < -0.75 or float(pos) + 0.5 < self.playback_ref_pos:
                # time-pos jumped or the file changed; start a new baseline.
                self.playback_ref_wall = now
                self.playback_ref_pos = float(pos)
                return 0.0
            return min(max(0.0, lag), self.args.catchup_max_lag_score)
        return None

    def lag_components(self) -> dict[str, float | None]:
        worker = self.api_worker
        fifo_lag = worker.lag_score() if worker is not None else None
        cache_lag = self.mpv_cache_duration()
        drift_lag = self.mpv_drift_lag()
        avsync = self.mpv_avsync()
        return {
            "drift": drift_lag,
            "fifo": fifo_lag,
            "cache": cache_lag,
            "avsync": avsync,
        }

    def active_lag_score(self, components: dict[str, float | None]) -> tuple[float, str]:
        candidates: list[tuple[float, str]] = []
        for key in ("drift", "fifo"):
            value = components.get(key)
            if isinstance(value, (int, float)):
                candidates.append((float(value), key))
        cache_value = components.get("cache")
        if self.args.catchup_use_mpv_cache and isinstance(cache_value, (int, float)):
            candidates.append((float(cache_value), "cache"))
        if not candidates:
            return 0.0, "none"
        return max(candidates, key=lambda item: item[0])

    def catchup_speed_for_lag(self, lag: float) -> float:
        if lag < self.args.catchup_speed_start_lag:
            return 1.0
        speed = 1.0 + ((lag - self.args.catchup_speed_start_lag) * self.args.catchup_speed_coeff)
        return clamp(speed, 1.0, self.args.catchup_max_speed)

    def maybe_print_catchup_status(self, lag: float, source: str, components: dict[str, float | None]) -> None:
        if self.args.catchup_status_interval <= 0:
            return
        if self.current_channel is None:
            return
        now = time.monotonic()
        if now - self.last_catchup_status_at < self.args.catchup_status_interval:
            return
        self.last_catchup_status_at = now
        actual_speed = None
        with suppress(Exception):
            value = self.mpv_get_property("speed")
            if isinstance(value, (int, float)):
                actual_speed = float(value)
                self.current_mpv_speed = actual_speed
        print(json.dumps({
            "event": "streamlink_catchup_status",
            "channel": self.current_channel,
            "speed": round(actual_speed if actual_speed is not None else self.current_mpv_speed, 3),
            "lag": round(lag, 3),
            "source": source,
            "drift_lag": None if components.get("drift") is None else round(float(components["drift"]), 3),
            "fifo_lag": None if components.get("fifo") is None else round(float(components["fifo"]), 3),
            "cache_duration": None if components.get("cache") is None else round(float(components["cache"]), 3),
            "avsync": None if components.get("avsync") is None else round(float(components["avsync"]), 3),
        }, ensure_ascii=False, separators=(",", ":")), flush=True)

    def avsync_has_been_bad(self, avsync: float | None) -> tuple[bool, float]:
        if self.args.no_avsync_reload or avsync is None:
            self.avsync_bad_since = None
            return False, 0.0
        if abs(avsync) < self.args.avsync_reload_threshold:
            self.avsync_bad_since = None
            return False, 0.0
        now = time.monotonic()
        if self.avsync_bad_since is None:
            self.avsync_bad_since = now
            return False, 0.0
        age = now - self.avsync_bad_since
        return age >= self.args.avsync_reload_duration, age

    def reload_current_stream_for_catchup(self, lag: float, source: str, *, avsync: float | None = None) -> bool:
        with self.stream_lock:
            channel = self.current_channel
            if not channel or self.args.mode != "fifo":
                return False
            now = time.monotonic()
            if now - self.last_catchup_reload_at < self.args.catchup_reload_cooldown:
                return False
            self.last_catchup_reload_at = now
            print(json.dumps({
                "event": "streamlink_catchup_reload",
                "channel": channel,
                "lag": round(lag, 3),
                "source": source,
                "avsync": None if avsync is None else round(avsync, 3),
            }, ensure_ascii=False, separators=(",", ":")), flush=True)
            with suppress(Exception):
                self.mpv_set_speed(1.0, lag=lag, source=source, avsync=avsync)
            ok = self.load_fifo_mode(channel)
            if ok:
                self.current_channel = channel
            return ok

    def catchup_once(self, interval: float) -> None:
        if self.args.mode != "fifo" or self.args.backend != "api":
            return
        if self.mpv_proc is None or self.mpv_proc.poll() is not None:
            return

        worker = self.api_worker
        if worker is not None:
            # If speed is >1, the extra playback rate is the estimated amount of
            # stream time recovered per wall-clock second.
            worker.reduce_lag_score(max(0.0, self.current_mpv_speed - 1.0) * interval)

        components = self.lag_components()
        avsync = components.get("avsync")
        lag, source = self.active_lag_score(components)
        self.maybe_print_catchup_status(lag, source, components)

        if isinstance(avsync, (int, float)):
            bad, age = self.avsync_has_been_bad(float(avsync))
            if bad:
                if self.reload_current_stream_for_catchup(abs(float(avsync)), f"avsync:{age:.1f}s", avsync=float(avsync)):
                    if worker is not None:
                        worker.clear_lag_score()
                    return
        else:
            self.avsync_bad_since = None

        if lag >= self.args.catchup_reload_lag:
            if self.reload_current_stream_for_catchup(lag, source, avsync=float(avsync) if isinstance(avsync, (int, float)) else None):
                if worker is not None:
                    worker.clear_lag_score()
                return

        speed = self.catchup_speed_for_lag(lag)
        try:
            self.mpv_set_speed(speed, lag=lag, source=source, avsync=float(avsync) if isinstance(avsync, (int, float)) else None)
        except Exception as exc:
            if self.args.verbose:
                print(f"catchup: failed to set speed: {exc}", file=sys.stderr, flush=True)

        if self.args.verbose and (lag >= self.args.catchup_speed_start_lag or (isinstance(avsync, (int, float)) and abs(avsync) >= self.args.avsync_reload_threshold)):
            avsync_s = "none" if avsync is None else f"{float(avsync):+.3f}s"
            print(f"catchup: lag={lag:.3f}s source={source} speed={speed:.3f} avsync={avsync_s}", flush=True)

    def catchup_loop(self) -> None:
        interval = self.args.catchup_interval
        while not self.catchup_stop_event.wait(interval):
            try:
                self.catchup_once(interval)
            except BaseException as exc:
                if self.args.verbose:
                    print(f"catchup: loop error: {exc}", file=sys.stderr, flush=True)

    def start_catchup_loop(self) -> None:
        if self.args.no_catchup or self.args.dry_run:
            return
        if self.args.mode != "fifo" or self.args.backend != "api":
            return
        if self.catchup_thread is not None:
            return
        self.catchup_thread = threading.Thread(target=self.catchup_loop, name="mpv-catchup", daemon=True)
        self.catchup_thread.start()

    def stop_catchup_loop(self) -> None:
        self.catchup_stop_event.set()
        thread = self.catchup_thread
        if thread is not None:
            thread.join(timeout=1.0)
        self.catchup_thread = None

    def shutdown_mpv(self) -> None:
        proc = self.mpv_proc
        if proc is None:
            return
        self.mpv_proc = None
        if proc.poll() is None:
            with suppress(Exception):
                if self.ipc_path.exists():
                    self.mpv_command(["quit"])
            try:
                proc.wait(timeout=self.args.stop_timeout)
            except subprocess.TimeoutExpired:
                with suppress(ProcessLookupError):
                    if os.name == "posix":
                        os.killpg(proc.pid, signal.SIGTERM)
                    else:
                        proc.terminate()
                with suppress(Exception):
                    proc.wait(timeout=1)
        with suppress(FileNotFoundError):
            self.ipc_path.unlink()

    def shutdown(self) -> None:
        self.stop_catchup_loop()
        with self.stream_lock:
            with suppress(Exception):
                self.mpv_set_speed(1.0)
            self.stop_current_stream()
            if self.current_fifo is not None:
                with suppress(FileNotFoundError):
                    self.current_fifo.unlink()
            if self.fifo_dir.exists():
                with suppress(OSError):
                    self.fifo_dir.rmdir()
            if not self.args.leave_mpv:
                self.shutdown_mpv()
        self.wait_for_cleanup_threads(self.args.stop_timeout + 1.0)


def accept_key(key: str) -> str:
    return base64.b64encode(hashlib.sha1((key + GUID).encode("ascii")).digest()).decode("ascii")


async def read_headers(reader: asyncio.StreamReader) -> tuple[str, dict[str, str]]:
    raw = await reader.readuntil(b"\r\n\r\n")
    lines = raw.decode("iso-8859-1").split("\r\n")
    headers: dict[str, str] = {}
    for line in lines[1:]:
        if ":" in line:
            k, v = line.split(":", 1)
            headers[k.strip().lower()] = v.strip()
    return lines[0], headers


async def handshake(reader: asyncio.StreamReader, writer: asyncio.StreamWriter, verbose: bool) -> None:
    request, headers = await read_headers(reader)
    if verbose:
        print(f"handshake: {request}", flush=True)
    key = headers.get("sec-websocket-key")
    if not key:
        writer.write(b"HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n")
        await writer.drain()
        raise ConnectionError("missing Sec-WebSocket-Key")
    writer.write((
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Accept: {accept_key(key)}\r\n"
        "\r\n"
    ).encode("ascii"))
    await writer.drain()


def frame(payload: bytes, opcode: int = 1) -> bytes:
    first = 0x80 | opcode
    n = len(payload)
    if n < 126:
        return bytes([first, n]) + payload
    if n <= 0xFFFF:
        return bytes([first, 126]) + struct.pack("!H", n) + payload
    return bytes([first, 127]) + struct.pack("!Q", n) + payload


async def send_text(writer: asyncio.StreamWriter, text: str) -> None:
    writer.write(frame(text.encode("utf-8")))
    await writer.drain()


async def exact(reader: asyncio.StreamReader, n: int) -> bytes:
    data = await reader.readexactly(n)
    if len(data) != n:
        raise EOFError("unexpected EOF")
    return data


async def read_frame(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> str | None:
    b1, b2 = await exact(reader, 1), await exact(reader, 1)
    b1, b2 = b1[0], b2[0]
    opcode = b1 & 0x0F
    masked = bool(b2 & 0x80)
    length = b2 & 0x7F
    if length == 126:
        length = struct.unpack("!H", await exact(reader, 2))[0]
    elif length == 127:
        length = struct.unpack("!Q", await exact(reader, 8))[0]
    mask = await exact(reader, 4) if masked else b""
    payload = await exact(reader, length) if length else b""
    if masked:
        payload = bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))
    if opcode == 8:
        return None
    if opcode == 9:
        writer.write(frame(payload, opcode=10))
        await writer.drain()
        return ""
    if opcode == 10:
        return ""
    if opcode != 1:
        return ""
    return payload.decode("utf-8", errors="replace")


async def ticker(writer: asyncio.StreamWriter, interval: float) -> None:
    while not writer.is_closing():
        await send_text(writer, "tick")
        await asyncio.sleep(interval)


async def close_all_clients() -> None:
    for writer in list(CLIENTS):
        writer.close()
    for writer in list(CLIENTS):
        with suppress(Exception):
            await writer.wait_closed()
    for task in list(CLIENT_TASKS):
        task.cancel()
    if CLIENT_TASKS:
        await asyncio.gather(*CLIENT_TASKS, return_exceptions=True)


async def client(reader: asyncio.StreamReader, writer: asyncio.StreamWriter, args: argparse.Namespace, follower: MpvStreamlinkFollower) -> None:
    peer = writer.get_extra_info("peername")
    tick_task: asyncio.Task[None] | None = None
    try:
        await handshake(reader, writer, args.verbose)
        CLIENTS.add(writer)
        print(f"client connected: {peer}", flush=True)
        await send_text(writer, "ping")
        await send_text(writer, "probe")
        await send_text(writer, "check")
        tick_task = asyncio.create_task(ticker(writer, args.interval))
        while True:
            raw = await read_frame(reader, writer)
            if raw is None:
                break
            if raw == "":
                continue
            try:
                event = json.loads(raw)
            except json.JSONDecodeError:
                if args.verbose:
                    print(f"recv raw: invalid JSON frame ({len(raw)} characters)", flush=True)
                continue
            follower.handle_event(event)
    except asyncio.CancelledError:
        raise
    except (asyncio.IncompleteReadError, ConnectionError, EOFError, OSError) as exc:
        print(f"client disconnected: {peer}: {exc}", file=sys.stderr, flush=True)
    finally:
        if tick_task:
            tick_task.cancel()
            with suppress(asyncio.CancelledError):
                await tick_task
        CLIENTS.discard(writer)
        writer.close()
        with suppress(Exception):
            await writer.wait_closed()


async def amain(args: argparse.Namespace) -> None:
    follower = MpvStreamlinkFollower(args)
    follower.start_catchup_loop()
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()

    for name in ("SIGINT", "SIGTERM"):
        sig = getattr(signal, name, None)
        if sig is not None:
            with suppress(NotImplementedError):
                loop.add_signal_handler(sig, stop.set)

    async def tracked_client(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        task = asyncio.current_task()
        if task is not None:
            CLIENT_TASKS.add(task)  # type: ignore[arg-type]
        try:
            await client(reader, writer, args, follower)
        finally:
            task = asyncio.current_task()
            if task is not None:
                CLIENT_TASKS.discard(task)  # type: ignore[arg-type]

    server = await asyncio.start_server(tracked_client, args.host, args.port)
    print("chatterino-streamlink-follow listening on " + ", ".join(str(s.getsockname()) for s in server.sockets or []), flush=True)

    try:
        async with server:
            await stop.wait()
    finally:
        server.close()
        await server.wait_closed()
        await close_all_clients()
        follower.shutdown()


def parse(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Follow Chatterino tab changes with Streamlink + persistent mpv")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8765)
    p.add_argument("--interval", type=float, default=0.20, help="poll interval sent to the Chatterino plugin")
    p.add_argument("--events", nargs="*", default=["tab_changed", "manual_check_ctx"], help="plugin event names to follow")
    p.add_argument("--mode", choices=["fifo", "url"], default="fifo", help="fifo keeps Streamlink in the data path; url uses --stream-url")
    p.add_argument("--backend", choices=["api", "subprocess"], default="api", help="api avoids launching Streamlink per switch; subprocess exactly uses the CLI")
    p.add_argument("--handoff", choices=["single", "overlap"], default="single", help="single cancels old after resolve; overlap starts the selected reader first, then immediately cancels old")
    p.add_argument("--streamlink", default="streamlink")
    p.add_argument("--streamlink-arg", action="append", help="extra argument passed to Streamlink subprocess backend; use --streamlink-arg=--flag for flag values")
    p.add_argument("--no-streamlink-low-latency-defaults", action="store_true")
    p.add_argument("--quality", default="best")
    p.add_argument("--url-template", default="https://www.twitch.tv/{channel}")
    p.add_argument("--mpv", default="mpv")
    p.add_argument("--mpv-arg", action="append", help="extra argument passed to mpv at startup; use --mpv-arg=--flag for flag values")
    p.add_argument("--no-mpv-low-latency-defaults", action="store_true")
    p.add_argument("--no-force-unpause", dest="force_unpause", action="store_false", default=True, help="do not send mpv pause=no on loadfile")
    p.add_argument("--mpv-ipc", help="mpv IPC socket path")
    p.add_argument("--mpv-ipc-timeout", type=float, default=1.0)
    p.add_argument("--fifo-dir", help="directory for per-stream FIFOs")
    p.add_argument("--api-read-size", type=int, default=262144)
    p.add_argument("--mpv-start-timeout", type=float, default=5.0)
    p.add_argument("--resolve-timeout", type=float, default=12.0)
    p.add_argument("--debounce", type=float, default=0.35)
    p.add_argument("--handoff-delay", type=float, default=0.20, help="deprecated compatibility option; old-reader cancellation is always immediate")
    p.add_argument("--stop-timeout", type=float, default=3.0)
    p.add_argument("--no-catchup", dest="no_catchup", action="store_true", help="disable adaptive mpv speed/reload catch-up loop")
    p.add_argument("--catchup-interval", type=float, default=0.50)
    p.add_argument("--catchup-status-interval", type=float, default=5.0, help="seconds between JSON catch-up status lines; 0 disables")
    p.add_argument("--no-catchup-osd", dest="catchup_osd", action="store_false", default=True, help="do not show speed changes on mpv OSD")
    p.add_argument("--catchup-osd-ms", type=int, default=1000)
    p.add_argument("--catchup-use-mpv-cache", action="store_true", help="also treat mpv demuxer cache duration as catch-up lag")
    p.add_argument("--catchup-write-block-threshold", type=float, default=0.75, help="FIFO write time above this counts as producer-ahead lag")
    p.add_argument("--catchup-max-lag-score", type=float, default=20.0)
    p.add_argument("--catchup-speed-start-lag", type=float, default=0.50)
    p.add_argument("--catchup-speed-coeff", type=float, default=0.035)
    p.add_argument("--catchup-max-speed", type=float, default=1.10)
    p.add_argument("--catchup-reload-lag", type=float, default=6.0)
    p.add_argument("--catchup-reload-cooldown", type=float, default=20.0)
    p.add_argument("--no-avsync-reload", action="store_true", help="disable reloads caused by persistent mpv avsync error")
    p.add_argument("--avsync-reload-threshold", type=float, default=0.30, help="absolute mpv avsync seconds that must persist before reloading")
    p.add_argument("--avsync-reload-duration", type=float, default=2.0, help="how long avsync must stay past threshold before reloading")
    p.add_argument("--leave-mpv", action="store_true", help="do not close mpv when this controller exits")
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--print-all", action="store_true")
    p.add_argument("--verbose", action="store_true")
    return p.parse_args(argv)


def main(argv: list[str]) -> int:
    try:
        asyncio.run(amain(parse(argv)))
        return 0
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
