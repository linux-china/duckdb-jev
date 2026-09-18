#!/usr/bin/env python3
"""Deterministic stand-in for the TypeSafe System One API, used by test/sql/*.test.

Rules (see docs/DESIGN.md):

* ``Authorization`` must be exactly ``Bearer test-key``, otherwise 401.
* A request whose condition or instructions mention ``trigger422`` returns 422.
* The first request mentioning ``trigger429once`` returns 429 with ``Retry-After: 1``;
  every later one is answered normally, so a test can prove the retry path works.
* noul   -> 0.9 when the last word of the condition occurs in the row JSON, else 0.1.
* score  -> score = len(row_json) % len(criteria), confidence 0.75.
* choice -> choice = criteria_keys[len(row_json) % len(criteria)], confidence 0.75.

Rows arrive as ``state.rows``; question ``r<i>`` asks about ``rows[i]``.

Usage: python3 test/mock_api.py [port]     (default 8765)
"""

from __future__ import annotations

import json
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

API_KEY = "test-key"
DEFAULT_PORT = 8765
CONFIDENCE = 0.75

#: How often each "trigger429once" marker has been seen, so the 429 happens exactly once.
_seen_markers: dict[str, int] = {}
_seen_lock = threading.Lock()


def first_time_seen(marker: str) -> bool:
    with _seen_lock:
        count = _seen_markers.get(marker, 0)
        _seen_markers[marker] = count + 1
        return count == 0


def row_text(row) -> str:
    """Compact JSON text of one row, matching DuckDB's ``to_json`` output."""
    if isinstance(row, str):
        return row
    return json.dumps(row, separators=(",", ":"), ensure_ascii=False)


def last_word(condition: str) -> str:
    words = condition.split()
    return words[-1] if words else ""


def answer_for(question: dict, condition: str, row: str) -> dict:
    kind = question.get("type", "noul")
    criteria = question.get("criteria")
    if kind == "noul":
        needle = last_word(condition or question.get("instructions", ""))
        return {"type": "noul", "noul": 0.9 if needle and needle in row else 0.1}
    if kind == "score":
        levels = list(criteria or [])
        if not levels:
            raise ValueError("score question needs a criteria list")
        idx = len(row) % len(levels)
        return {
            "type": "score",
            "score": float(idx),
            "confidence": CONFIDENCE,
            "legend": {str(i): level for i, level in enumerate(levels)},
            "probabilities": {str(i): (1.0 if i == idx else 0.0) for i in range(len(levels))},
        }
    if kind == "choice":
        options = list((criteria or {}).keys())
        if not options:
            raise ValueError("choice question needs a criteria object")
        idx = len(row) % len(options)
        return {
            "type": "choice",
            "choice": options[idx],
            "confidence": CONFIDENCE,
            "probabilities": {opt: (1.0 if i == idx else 0.0) for i, opt in enumerate(options)},
        }
    raise ValueError(f"unknown question type {kind!r}")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # quiet by default
        if "-v" in sys.argv:
            sys.stderr.write("mock_api: " + (fmt % args) + "\n")

    def send_json(self, status: int, payload: dict, extra_headers: dict | None = None) -> None:
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        for name, value in (extra_headers or {}).items():
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):  # health check for test/run.sh
        self.send_json(200, {"ok": True})

    def do_POST(self):
        if self.headers.get("Authorization") != f"Bearer {API_KEY}":
            self.send_json(401, {"error": "invalid api key"})
            return

        content_types = self.headers.get_all("Content-Type") or []
        if content_types != ["application/json"]:
            self.send_json(422, {"error": f"content-type must be exactly one application/json, got {content_types}"})
            return
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length).decode("utf-8")
        try:
            body = json.loads(raw)
        except json.JSONDecodeError as exc:
            self.send_json(400, {"error": f"bad json: {exc}"})
            return

        if "trigger422" in raw:
            self.send_json(422, {"error": "unprocessable entity: trigger422"})
            return

        if "trigger429once" in raw and first_time_seen("trigger429once"):
            self.send_json(
                429,
                {"error": "slow down: trigger429once"},
                extra_headers={"Retry-After": "1"},
            )
            return

        state = body.get("state") or {}
        rows = [row_text(row) for row in state.get("rows", [])]
        condition = state.get("condition", "")
        questions = body.get("questions") or {}

        answers = {}
        try:
            for key, question in questions.items():
                idx = int(key[1:]) if key[:1] == "r" and key[1:].isdigit() else None
                if idx is None or idx >= len(rows):
                    self.send_json(400, {"error": f"question {key} has no matching row"})
                    return
                answers[key] = answer_for(question, condition, rows[idx])
        except ValueError as exc:
            self.send_json(400, {"error": str(exc)})
            return

        self.send_json(
            200,
            {
                "model": body.get("model", "jev-latest"),
                "answers": answers,
                "usage": {"input_tokens": len(raw) // 4, "output_tokens": len(answers)},
            },
        )


def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else DEFAULT_PORT
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    sys.stderr.write(f"mock_api: listening on http://127.0.0.1:{port}/v1/systemone\n")
    sys.stderr.flush()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
