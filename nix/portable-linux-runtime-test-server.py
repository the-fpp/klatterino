#!/usr/bin/env python3

import argparse
import base64
import hashlib
import os
import signal
import socket
import ssl
import threading
from pathlib import Path


def read_headers(connection: ssl.SSLSocket) -> bytes:
    request = bytearray()
    while b"\r\n\r\n" not in request and len(request) < 65536:
        chunk = connection.recv(4096)
        if not chunk:
            break
        request.extend(chunk)
    return bytes(request)


def serve_http(connection: ssl.SSLSocket) -> None:
    read_headers(connection)
    connection.sendall(
        b"HTTP/1.1 200 OK\r\n"
        b"Content-Type: text/plain\r\n"
        b"Content-Length: 3\r\n"
        b"Connection: close\r\n\r\nok\n"
    )


def serve_websocket(connection: ssl.SSLSocket) -> None:
    request = read_headers(connection)
    headers = {}
    for line in request.decode("latin-1").split("\r\n")[1:]:
        if ":" in line:
            name, value = line.split(":", 1)
            headers[name.strip().lower()] = value.strip()

    key = headers.get("sec-websocket-key")
    if not key:
        return

    accept = base64.b64encode(
        hashlib.sha1(
            (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")
        ).digest()
    )
    connection.sendall(
        b"HTTP/1.1 101 Switching Protocols\r\n"
        b"Upgrade: websocket\r\n"
        b"Connection: Upgrade\r\n"
        b"Sec-WebSocket-Accept: "
        + accept
        + b"\r\n\r\n"
    )
    connection.settimeout(2)
    try:
        connection.recv(4096)
    except (TimeoutError, OSError):
        pass


def run_listener(listener: socket.socket, context: ssl.SSLContext, handler) -> None:
    while True:
        raw_connection, _ = listener.accept()
        try:
            with context.wrap_socket(raw_connection, server_side=True) as connection:
                handler(connection)
        except (ConnectionError, ssl.SSLError):
            raw_connection.close()


def make_listener() -> socket.socket:
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen()
    return listener


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--certificate", required=True)
    parser.add_argument("--key", required=True)
    parser.add_argument("--ready-file", required=True)
    arguments = parser.parse_args()

    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(arguments.certificate, arguments.key)

    https_listener = make_listener()
    wss_listener = make_listener()
    threads = [
        threading.Thread(
            target=run_listener,
            args=(https_listener, context, serve_http),
            daemon=True,
        ),
        threading.Thread(
            target=run_listener,
            args=(wss_listener, context, serve_websocket),
            daemon=True,
        ),
    ]
    for thread in threads:
        thread.start()

    ready_file = Path(arguments.ready_file)
    temporary = ready_file.with_suffix(".tmp")
    temporary.write_text(
        f"{https_listener.getsockname()[1]} {wss_listener.getsockname()[1]}\n",
        encoding="ascii",
    )
    os.replace(temporary, ready_file)

    signal.pause()


if __name__ == "__main__":
    main()
