#!/usr/bin/env python3
"""Local static server and WAV sink for the audio calibration page."""

from __future__ import annotations

import argparse
import json
import re
from datetime import datetime
from http import HTTPStatus
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse


ROOT = Path(__file__).resolve().parent
WEB_ROOT = ROOT / "web"
RECORDINGS_ROOT = ROOT / "recordings"
MAX_UPLOAD_BYTES = 16 * 1024 * 1024


def safe_component(value: str, fallback: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9_.-]+", "-", value.strip()).strip("-.")
    return (cleaned[:48] or fallback).lower()


class CalibrationHandler(SimpleHTTPRequestHandler):
    server_version = "SisuAudioCalibration/1.0"

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(WEB_ROOT), **kwargs)

    def end_headers(self) -> None:
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        super().end_headers()

    def do_GET(self) -> None:
        if urlparse(self.path).path == "/api/health":
            self._send_json(HTTPStatus.OK, {"ok": True})
            return
        super().do_GET()

    def do_POST(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path != "/api/save":
            self.send_error(HTTPStatus.NOT_FOUND)
            return

        try:
            content_length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self.send_error(HTTPStatus.BAD_REQUEST, "invalid content length")
            return

        if content_length < 44 or content_length > MAX_UPLOAD_BYTES:
            self.send_error(HTTPStatus.BAD_REQUEST, "invalid WAV size")
            return

        wav_data = self.rfile.read(content_length)
        if len(wav_data) != content_length:
            self.send_error(HTTPStatus.BAD_REQUEST, "incomplete upload")
            return
        if wav_data[:4] != b"RIFF" or wav_data[8:12] != b"WAVE":
            self.send_error(HTTPStatus.BAD_REQUEST, "body is not a WAV file")
            return

        query = parse_qs(parsed.query)
        slot = query.get("slot", [""])[0]
        if slot not in {"original", "dut"}:
            self.send_error(HTTPStatus.BAD_REQUEST, "invalid capture slot")
            return

        label = safe_component(query.get("label", ["take"])[0], "take")
        stamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")[:-3]
        stem = f"{stamp}_{slot}_{label}"

        RECORDINGS_ROOT.mkdir(parents=True, exist_ok=True)
        wav_path = RECORDINGS_ROOT / f"{stem}.wav"
        metadata_path = RECORDINGS_ROOT / f"{stem}.json"

        metadata = {
            "captured_at": datetime.now().astimezone().isoformat(),
            "slot": slot,
            "label": query.get("label", ["take"])[0],
            "sample_rate_hz": query.get("sample_rate", [""])[0],
            "duration_seconds": query.get("duration", [""])[0],
            "trim_start_seconds": query.get("trim_start", [""])[0],
            "trim_end_seconds": query.get("trim_end", [""])[0],
            "continuous_end_seconds": query.get("continuous_end", [""])[0],
            "continuous_rms_dbfs": query.get("continuous_dbfs", [""])[0],
            "active_rms_dbfs": query.get("active_dbfs", [""])[0],
            "peak_dbfs": query.get("peak_dbfs", [""])[0],
            "active_percent": query.get("active_percent", [""])[0],
            "clipped_samples": query.get("clipped_samples", [""])[0],
            "input_label": query.get("input_label", [""])[0],
            "wav_file": wav_path.name,
        }

        wav_path.write_bytes(wav_data)
        metadata_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
        self._send_json(
            HTTPStatus.CREATED,
            {"ok": True, "wav": wav_path.name, "metadata": metadata_path.name},
        )

    def _send_json(self, status: HTTPStatus, body: dict[str, object]) -> None:
        encoded = json.dumps(body).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def log_message(self, format_string: str, *args: object) -> None:
        print(f"[audio-cal] {self.address_string()} {format_string % args}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()

    server = ThreadingHTTPServer((args.host, args.port), CalibrationHandler)
    print(f"Audio calibration tool: http://{args.host}:{args.port}")
    print(f"Recordings directory: {RECORDINGS_ROOT}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
