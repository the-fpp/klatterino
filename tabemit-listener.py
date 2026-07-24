#!/usr/bin/env python3
"""Local WebSocket listener for the Chatterino tab-emit plugin.

No third-party Python packages required. By default it prints every JSON/text
frame received from the plugin and sends periodic "tick" frames to drive polling.
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import hashlib
import json
import os
import shutil
import signal
import struct
import subprocess
import sys
from contextlib import suppress
from typing import Any

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
CLIENTS: set[asyncio.StreamWriter] = set()
CLIENT_TASKS: set[asyncio.Task[None]] = set()


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


async def broadcast(text: str) -> None:
    dead: list[asyncio.StreamWriter] = []
    for w in list(CLIENTS):
        try:
            await send_text(w, text)
        except OSError:
            dead.append(w)
    for w in dead:
        CLIENTS.discard(w)


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
        return f"<non-text opcode={opcode} len={len(payload)}>"
    return payload.decode("utf-8", errors="replace")


def event_env(event: dict[str, Any]) -> dict[str, str]:
    env = os.environ.copy()
    env["TABEMIT_JSON"] = json.dumps(event, ensure_ascii=False, separators=(",", ":"))
    for k in ("event", "reason", "channel", "display_name", "channel_type", "identity"):
        env["TABEMIT_" + k.upper()] = str(event.get(k) or "")
    env["TABEMIT_PAGE_INDEX"] = "" if event.get("page_index") is None else str(event.get("page_index"))
    env["TABEMIT_ALL_CHANNELS"] = ",".join(str(x) for x in event.get("all_channels") or [])
    return env


def handle(raw: str, args: argparse.Namespace) -> None:
    if raw == "":
        return
    try:
        event = json.loads(raw)
    except json.JSONDecodeError:
        print(f"recv raw: {raw}", flush=True)
        return
    print("recv: " + json.dumps(event, ensure_ascii=False, separators=(",", ":")), flush=True)

    if args.notify and event.get("event") in args.notify_events:
        notify_send = shutil.which("notify-send")
        if notify_send:
            title = f"Chatterino: {event.get('event')}"
            channel = event.get("display_name") or event.get("channel") or "<no channel>"
            subprocess.Popen([notify_send, title, f"tab={event.get('page_index')} channel={channel}"])
        else:
            print("notify-send not found", file=sys.stderr, flush=True)

    if args.command and event.get("event") in args.command_events:
        subprocess.Popen(args.command, shell=True, env=event_env(event))


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


def install_stdin_reader(loop: asyncio.AbstractEventLoop) -> callable | None:
    """Install a non-blocking stdin reader.

    The old implementation used run_in_executor(sys.stdin.readline), which could
    leave Python waiting on a blocked executor thread during Ctrl-C shutdown.
    On Linux/NixOS, add_reader avoids that completely.
    """
    try:
        fd = sys.stdin.fileno()
    except Exception:
        return None

    def on_stdin() -> None:
        line = sys.stdin.readline()
        if line == "":
            with suppress(Exception):
                loop.remove_reader(fd)
            return
        text = line.rstrip("\n")
        if text:
            print(f"send: {text}", flush=True)
            asyncio.create_task(broadcast(text))

    try:
        loop.add_reader(fd, on_stdin)
    except (NotImplementedError, RuntimeError, OSError):
        print("stdin commands unavailable on this event loop", file=sys.stderr, flush=True)
        return None

    print('stdin commands enabled: type "check", "ping", "probe", or any text to send to the plugin', flush=True)

    def cleanup() -> None:
        with suppress(Exception):
            loop.remove_reader(fd)

    return cleanup


async def client(reader: asyncio.StreamReader, writer: asyncio.StreamWriter, args: argparse.Namespace) -> None:
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
            msg = await read_frame(reader, writer)
            if msg is None:
                break
            handle(msg, args)
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


def track_client_task(task: asyncio.Task[None]) -> None:
    CLIENT_TASKS.add(task)
    task.add_done_callback(CLIENT_TASKS.discard)


async def amain(args: argparse.Namespace) -> None:
    async def on_client(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        await client(reader, writer, args)

    server = await asyncio.start_server(on_client, args.host, args.port)
    cleanup_stdin = None
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()

    for name in ("SIGINT", "SIGTERM"):
        sig = getattr(signal, name, None)
        if sig is not None:
            with suppress(NotImplementedError):
                loop.add_signal_handler(sig, stop.set)

    # Track tasks created by the server so we can cancel connected clients before
    # returning. asyncio.start_server does not expose them directly, so wrap the
    # factory with create_task when serving manually below.
    server.close()
    await server.wait_closed()

    async def tracked_client(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        task = asyncio.current_task()
        if task is not None:
            CLIENT_TASKS.add(task)  # type: ignore[arg-type]
        try:
            await client(reader, writer, args)
        finally:
            task = asyncio.current_task()
            if task is not None:
                CLIENT_TASKS.discard(task)  # type: ignore[arg-type]

    server = await asyncio.start_server(tracked_client, args.host, args.port)
    print("tabemit-listener listening on " + ", ".join(str(s.getsockname()) for s in server.sockets or []), flush=True)

    if args.stdin:
        cleanup_stdin = install_stdin_reader(loop)

    try:
        async with server:
            await stop.wait()
    finally:
        if cleanup_stdin:
            cleanup_stdin()
        server.close()
        await server.wait_closed()
        await close_all_clients()


def parse(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Listen for Chatterino tab/channel switch events")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8765)
    p.add_argument("--interval", type=float, default=0.20)
    p.add_argument("--stdin", action="store_true")
    p.add_argument("--verbose", action="store_true")
    p.add_argument("--notify", action="store_true")
    p.add_argument("--notify-events", nargs="*", default=["tab_changed"])
    p.add_argument("--command")
    p.add_argument("--command-events", nargs="*", default=["tab_changed"])
    return p.parse_args(argv)


def main(argv: list[str]) -> int:
    try:
        asyncio.run(amain(parse(argv)))
        return 0
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
