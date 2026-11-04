#!/usr/bin/env python3
"""Relay HTTP(S) requests from the Android script runtime through the host."""

import argparse
import base64
import http.server
import json
import hashlib
import logging
import urllib.error
import urllib.parse
import urllib.request


MAX_BODY = 8 * 1024 * 1024
MAX_ENVELOPE = 6 * MAX_BODY + 64 * 1024
ALLOWED_METHODS = {"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD"}


def fetch(options):
    url = options.get("Url") or options.get("URL")
    if not isinstance(url, str) or urllib.parse.urlsplit(url).scheme not in ("http", "https"):
        raise ValueError("Url must be an http(s) URL")
    method = str(options.get("Method", "GET")).upper()
    if method not in ALLOWED_METHODS:
        raise ValueError("unsupported HTTP method")
    body = options.get("Body")
    if body is not None and not isinstance(body, str):
        raise ValueError("Body must be a string")
    headers = options.get("Headers", {})
    if not isinstance(headers, dict) or len(headers) > 80:
        raise ValueError("Headers must be a small table")
    if any(not isinstance(k, str) or not isinstance(v, str) or "\r" in k + v or "\n" in k + v
           for k, v in headers.items()):
        raise ValueError("Headers must contain valid strings")
    encoded = options.get("BodyBase64")
    if encoded is not None:
        if body is not None or not isinstance(encoded, str):
            raise ValueError("BodyBase64 must be a string without Body")
        payload = base64.b64decode(encoded, validate=True)
    else:
        payload = None if body is None else body.encode("utf-8")
    if payload is not None and len(payload) > MAX_BODY:
        raise ValueError("Body exceeds 8 MiB")
    req = urllib.request.Request(url, data=payload, headers=headers, method=method)
    if not req.has_header("User-agent"):
        req.add_header("User-Agent", "Roblox/WinInet")
    try:
        response = urllib.request.urlopen(req, timeout=15)
    except urllib.error.HTTPError as exc:
        response = exc
    with response:
        content = response.read(MAX_BODY + 1)
        if len(content) > MAX_BODY:
            raise ValueError("response exceeds 8 MiB")
        logging.getLogger("basalt.http").info("%s %s status=%s bytes=%s sha256=%s", method, url, response.status, len(content), hashlib.sha256(content).hexdigest())
        status = response.status
        result = {
            "Success": 200 <= status < 300,
            "StatusCode": status,
            "StatusMessage": response.reason,
            "Headers": dict(response.headers.items()),
        }
        try:
            result["Body"] = content.decode("utf-8")
        except UnicodeDecodeError:
            result["BodyBase64"] = base64.b64encode(content).decode("ascii")
        return result


class Handler(http.server.BaseHTTPRequestHandler):
    def reply(self, status, body):
        payload = json.dumps(body, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        if self.path == "/health":
            self.reply(200, {"status": "ready"})
        else:
            self.reply(404, {"error": "not found"})

    def do_POST(self):
        if self.path != "/http":
            return self.reply(404, {"error": "not found"})
        try:
            size = int(self.headers.get("Content-Length", "0"))
            if not 0 < size <= MAX_ENVELOPE:
                return self.reply(413, {"error": "request envelope exceeds limit"})
            options = json.loads(self.rfile.read(size))
            if not isinstance(options, dict):
                raise ValueError("request must be a table")
            if options.get("BridgeOperation") == "websocket":
                from websocket_bridge import operate
                try:
                    return self.reply(200, operate(options))
                except Exception as exc:
                    return self.reply(400, {"error": str(exc)})
            result = fetch(options)
            self.reply(200, result)
        except (ValueError, urllib.error.URLError, TimeoutError, OSError) as exc:
            self.reply(400, {"error": str(exc)})

    def log_message(self, fmt, *args):
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=17981)
    parser.add_argument("--trace", help="optional HTTP metadata log (no headers or response bodies)")
    args = parser.parse_args()
    if args.trace:
        logging.basicConfig(filename=args.trace, level=logging.INFO, format="%(asctime)s %(message)s")
    http.server.ThreadingHTTPServer(("0.0.0.0", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
