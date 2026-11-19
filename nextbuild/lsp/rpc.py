from __future__ import annotations

import json
import sys
from dataclasses import dataclass
from typing import Any, BinaryIO, Callable


@dataclass
class Request:
    id: int | str | None
    method: str
    params: Any


class JsonRpcStdio:
    """Minimal LSP transport: Content-Length framed JSON-RPC over stdio."""

    def __init__(self, reader: BinaryIO | None = None, writer: BinaryIO | None = None):
        self.reader = reader or sys.stdin.buffer
        self.writer = writer or sys.stdout.buffer

    def read_message(self) -> dict[str, Any] | None:
        headers: dict[str, str] = {}
        while True:
            line = self.reader.readline()
            if not line:
                return None
            if line in (b"\r\n", b"\n"):
                break
            text = line.decode("utf-8", errors="replace").strip()
            if ":" in text:
                key, value = text.split(":", 1)
                headers[key.strip().lower()] = value.strip()

        length = int(headers.get("content-length", "0"))
        if length <= 0:
            return None
        body = self.reader.read(length)
        if not body:
            return None
        return json.loads(body.decode("utf-8"))

    def write_message(self, payload: dict[str, Any]) -> None:
        raw = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        header = f"Content-Length: {len(raw)}\r\n\r\n".encode("ascii")
        self.writer.write(header + raw)
        self.writer.flush()

    def respond(self, request_id: int | str | None, result: Any) -> None:
        self.write_message({"jsonrpc": "2.0", "id": request_id, "result": result})

    def respond_error(self, request_id: int | str | None, code: int, message: str) -> None:
        self.write_message(
            {
                "jsonrpc": "2.0",
                "id": request_id,
                "error": {"code": code, "message": message},
            }
        )

    def notify(self, method: str, params: Any) -> None:
        self.write_message({"jsonrpc": "2.0", "method": method, "params": params})


Handler = Callable[[Request], Any]
