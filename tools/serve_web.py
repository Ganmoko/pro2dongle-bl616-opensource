#!/usr/bin/env python3
"""Serve the Pro2 WebHID configurator on localhost or a LAN interface."""

from __future__ import annotations

import argparse
import functools
import http.server
import socket
import ssl
from pathlib import Path


class NoCacheHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self) -> None:
        self.send_header("Cache-Control", "no-store, max-age=0")
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        super().end_headers()


def lan_address() -> str:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect(("8.8.8.8", 80))
        return str(sock.getsockname()[0])
    except OSError:
        try:
            return socket.gethostbyname(socket.gethostname())
        except OSError:
            return "127.0.0.1"
    finally:
        sock.close()


def parse_args() -> argparse.Namespace:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="0.0.0.0", help="listen address")
    parser.add_argument("--port", type=int, default=8765, help="listen port")
    parser.add_argument("--directory", type=Path, default=root / "web")
    parser.add_argument("--certfile", type=Path, help="TLS certificate PEM")
    parser.add_argument("--keyfile", type=Path, help="TLS private-key PEM")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    directory = args.directory.resolve()
    if not (directory / "index.html").is_file():
        raise SystemExit(f"web root does not contain index.html: {directory}")
    if bool(args.certfile) != bool(args.keyfile):
        raise SystemExit("--certfile and --keyfile must be supplied together")

    handler = functools.partial(NoCacheHandler, directory=str(directory))
    server = http.server.ThreadingHTTPServer((args.bind, args.port), handler)
    scheme = "http"
    if args.certfile and args.keyfile:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(args.certfile, args.keyfile)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        scheme = "https"

    address = lan_address()
    origin = f"{scheme}://{address}:{args.port}"
    print(f"Serving {directory}", flush=True)
    print(f"LAN URL: {origin}/", flush=True)
    if scheme == "http" and address not in ("127.0.0.1", "::1"):
        print("WebHID requires a secure context. For temporary LAN testing,", flush=True)
        print("launch Chromium with:", flush=True)
        print(
            "  --user-data-dir=<temporary-profile> "
            f'--unsafely-treat-insecure-origin-as-secure="{origin}" "{origin}/"',
            flush=True,
        )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
