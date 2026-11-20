"""The Python host: stdin, environment, arguments, HTTP, store and filesystem.

The HTTP server is the backend port for MI-written web applications: every
response carries permissive CORS headers so a Mi Browser opened from ``file://``
can call it with synchronous XMLHttpRequest.
"""
from __future__ import annotations

import json
import hashlib
import os
import sys
import threading
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any
from urllib.parse import urlparse

from ..jsonio import encode_json, from_json, parse_json, to_json
from ..values import (
    NOTHING,
    Assembly,
    MiValueError,
    StationValue,
    as_int,
    as_text,
    display,
    kind,
)

NET_TIMEOUT = 10.0
DEFAULT_STORE_FILE = ".mi-store.json"

_store_lock = threading.Lock()


# --------------------------------------------------------------------- stdio
def _ask(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    prompt = bound.get("prompt")
    if prompt is not None:
        sys.stdout.write(display(prompt))
        sys.stdout.flush()
    line = sys.stdin.readline()
    if line == "":
        return ""
    return line.rstrip("\n").rstrip("\r")


def _env(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    name = as_text(bound.get("name"), "`env`'s `name`")
    fallback = bound.get("fallback")
    value = os.environ.get(name)
    if value is None:
        return fallback if fallback is not None else ""
    return value


def _argv(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    return tuple(runtime.argv)


# ------------------------------------------------------------------- net
def _http(method: str, url: str, body: str | None, safe: bool = False) -> Any:
    data = body.encode("utf-8") if body is not None else None
    request = urllib.request.Request(url, data=data, method=method)
    if data is not None:
        request.add_header("Content-Type", "text/plain; charset=utf-8")
    try:
        with urllib.request.urlopen(request, timeout=NET_TIMEOUT) as response:
            payload = response.read().decode("utf-8", "replace")
            return Assembly.from_pairs(
                [
                    ("status", int(response.status)),
                    ("ok", True),
                    ("body", payload),
                ]
            )
    except urllib.error.HTTPError as exc:
        payload = exc.read().decode("utf-8", "replace")
        return Assembly.from_pairs(
            [("status", int(exc.code)), ("ok", False), ("body", payload)]
        )
    except (urllib.error.URLError, OSError, ValueError) as exc:
        if safe:
            return Assembly.from_pairs(
                [("status", 0), ("ok", False), ("body", f"网络错误：{exc}")]
            )
        raise MiValueError(f"network error for {url}: {exc}", code="E0309")


def _net_get(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    url = as_text(bound.get("url"), "`net.get`'s `url`")
    safe = bound.get("safe")
    return _http("GET", url, None, safe=bool(safe) if safe is not None else False)


def _net_post(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    url = as_text(bound.get("url"), "`net.post`'s `url`")
    body = as_text(bound.get("body"), "`net.post`'s `body`")
    safe = bound.get("safe")
    return _http("POST", url, body, safe=bool(safe) if safe is not None else False)


# ------------------------------------------------------------------ store
_MISSING = object()


def _store_path() -> str:
    return os.environ.get("MI_STORE_FILE") or DEFAULT_STORE_FILE


def _load_store() -> dict[str, Any]:
    path = _store_path()
    if not os.path.exists(path):
        return {}
    try:
        with open(path, "r", encoding="utf-8") as handle:
            data = json.load(handle)
    except (OSError, json.JSONDecodeError) as exc:
        raise MiValueError(f"cannot read the store {path}: {exc}", code="E0309")
    if not isinstance(data, dict):
        raise MiValueError(f"store {path} must contain a JSON object", code="E0309")
    return data


def _save_store(data: dict[str, Any]) -> None:
    path = _store_path()
    directory = os.path.dirname(os.path.abspath(path)) or "."
    os.makedirs(directory, exist_ok=True)
    temporary = f"{path}.{os.getpid()}.{threading.get_ident()}.tmp"
    try:
        with open(temporary, "w", encoding="utf-8") as handle:
            json.dump(data, handle, ensure_ascii=False, indent=2)
        os.replace(temporary, path)
    except OSError as exc:
        raise MiValueError(f"cannot write the store {path}: {exc}", code="E0309")
    finally:
        if os.path.exists(temporary):  # pragma: no cover - best effort
            try:
                os.remove(temporary)
            except OSError:
                pass


def _store_read(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    name = as_text(bound.get("name"), "`store.read`'s `name`")
    fallback = bound.get("fallback", _MISSING)
    with _store_lock:
        data = _load_store()
    if name not in data:
        return NOTHING if fallback is _MISSING else fallback
    return from_json(data[name])


def _store_write(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    name = as_text(bound.get("name"), "`store.write`'s `name`")
    encoded = to_json(bound.get("value"))
    with _store_lock:
        data = _load_store()
        data[name] = encoded
        _save_store(data)
    return NOTHING


def _store_clear(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    name = bound.get("name")
    with _store_lock:
        data = _load_store()
        if name is None:
            data = {}
        else:
            data.pop(as_text(name, "`store.clear`'s `name`"), None)
        _save_store(data)
    return NOTHING


# ------------------------------------------------------------------- files
def _fs_read(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    path = as_text(bound.get("path"), "`fs.read`'s `path`")
    try:
        with open(path, "r", encoding="utf-8") as handle:
            return handle.read()
    except OSError as exc:
        raise MiValueError(f"cannot read {path}: {exc}", code="E0309")


def _fs_write(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    path = as_text(bound.get("path"), "`fs.write`'s `path`")
    text = as_text(bound.get("text"), "`fs.write`'s `text`")
    try:
        directory = os.path.dirname(os.path.abspath(path))
        if directory:
            os.makedirs(directory, exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
    except OSError as exc:
        raise MiValueError(f"cannot write {path}: {exc}", code="E0309")
    return NOTHING


def _fs_exists(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    path = as_text(bound.get("path"), "`fs.exists`'s `path`")
    return os.path.exists(path)


def _crypto_sha256(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    text = as_text(bound.get("text"), "`crypto.sha256`'s `text`")
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


# -------------------------------------------------------------------- serve
def _serve(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    port = as_int(bound.get("port"), "`serve`'s `port`")
    handler = bound.get("handler")
    if not isinstance(handler, (StationValue,)):
        raise MiValueError(
            f"`serve`'s handler must be a station, not {kind(handler)}"
        )

    class Handler(BaseHTTPRequestHandler):
        server_version = "MI/1.0"

        def log_message(self, fmt: str, *args: Any) -> None:  # quiet by default
            pass

        def _cors(self) -> None:
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Headers", "*")
            self.send_header("Access-Control-Allow-Methods", "GET,POST,PUT,DELETE,OPTIONS")

        def _send(self, status: int, payload: bytes, content_type: str = "text/plain; charset=utf-8") -> None:
            self.send_response(status)
            self._cors()
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(payload)

        def do_OPTIONS(self) -> None:  # noqa: N802 - http.server API
            self._send(204, b"")

        def _respond(self) -> None:  # noqa: N802 - http.server API
            parsed = urlparse(self.path)
            length = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(length).decode("utf-8", "replace") if length else ""
            request = Assembly.from_pairs(
                [
                    ("method", self.command),
                    ("path", parsed.path),
                    ("query", parsed.query),
                    ("body", body),
                ]
            )
            try:
                # Every HTTP request gets its own step budget: `serve` is a
                # long-running host, so a shared counter would eventually
                # exhaust the budget of the whole process.
                runtime.steps = 0
                runtime.depth = 0
                runtime.stack.clear()
                result = runtime.call_value(handler, [(None, request)], span)
            except Exception as exc:  # noqa: BLE001 - report as a 500 body
                text = f"handler failed: {exc}"
                status = 500
                content_type = "text/plain; charset=utf-8"
                payload = text.encode("utf-8")
            else:
                text = ""
                status = 200
                content_type = "text/plain; charset=utf-8"
                payload_bytes: bytes | None = None
                if isinstance(result, str):
                    text = result
                elif isinstance(result, Assembly):
                    if result.has("status"):
                        status = as_int(result.get("status"), "the response status")
                    if result.has("content_type"):
                        content_type = display(result.get("content_type"))
                    if result.has("bytes"):
                        raw = result.get("bytes")
                        if isinstance(raw, str):
                            try:
                                payload_bytes = bytes.fromhex(raw)
                            except ValueError:
                                raise MiValueError("`bytes` must be hexadecimal text")
                    if result.has("body"):
                        text = display(result.get("body"))
                else:
                    text = display(result)
                payload = payload_bytes if payload_bytes is not None else text.encode("utf-8")
            self._send(status, payload, content_type)

        do_GET = _respond
        do_POST = _respond
        do_PUT = _respond
        do_DELETE = _respond
        do_HEAD = _respond

    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    server.daemon_threads = True
    actual_port = server.server_address[1]
    runtime.emit_line(f"listening on http://127.0.0.1:{actual_port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:  # pragma: no cover - interactive
        pass
    finally:
        server.server_close()
    return NOTHING


IMPLS: dict[str, Any] = {
    "ask": _ask,
    "env": _env,
    "argv": _argv,
    "serve": _serve,
    "net.get": _net_get,
    "net.post": _net_post,
    "store.read": _store_read,
    "store.write": _store_write,
    "store.clear": _store_clear,
    "fs.read": _fs_read,
    "fs.write": _fs_write,
    "fs.exists": _fs_exists,
    "crypto.sha256": _crypto_sha256,
}
