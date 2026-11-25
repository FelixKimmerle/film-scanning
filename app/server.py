#!/usr/bin/env python3
"""Standalone Glass Cockpit web server for analog film scanning & half-frame cropping.

Zero external web framework dependencies (uses Python stdlib ThreadingHTTPServer).
Binds to 0.0.0.0:8765 by default so you can run the scanner on one machine and
control/crop from any browser on your LAN.
"""
from __future__ import annotations

import argparse
import io
import json
import mimetypes
import threading
import time
import urllib.parse
import uuid
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image

from app.backends.base import JobState, ScanRequest, detect_usb_scanner
from app.backends.silverfast import SilverFastBackend
from app.backends.vuescan import VueScanBackend
from app.core import detector, tiff_bridge

BASE_DIR = Path(__file__).resolve().parent.parent
SCANS_DIR = BASE_DIR / "scans"
EXPORTS_DIR = BASE_DIR / "exports"
WEB_DIR = Path(__file__).resolve().parent / "web"

INVERT_MODES = {
    "raw": 0,
    "color_neg": 1,
    "bw_neg": 2,
    "slide": 3,
}


class AppState:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.backends = {
            "silverfast": SilverFastBackend(),
            "vuescan": VueScanBackend(),
        }
        self.job = JobState(job_id="none", status="idle", stage="READY")
        self.cancel_event = threading.Event()
        self.preview_cache: dict[tuple, tuple[float, bytes]] = {}
        self.info_cache: dict[str, tuple[float, dict]] = {}

    def list_scans(self) -> list[dict]:
        SCANS_DIR.mkdir(parents=True, exist_ok=True)
        out = []
        for p in sorted(SCANS_DIR.iterdir(), key=lambda x: x.stat().st_mtime, reverse=True):
            if not p.is_file() or p.suffix.lower() not in (".tif", ".tiff"):
                continue
            mtime = p.stat().st_mtime
            key = str(p)
            cached = self.info_cache.get(key)
            if cached and cached[0] == mtime:
                meta = cached[1]
            else:
                try:
                    info = tiff_bridge.read_info(p)
                    meta = {
                        "name": p.name,
                        "path": str(p),
                        "size_bytes": p.stat().st_size,
                        "mtime": mtime,
                        "width": info.width,
                        "height": info.height,
                        "dpi": round(info.dpi_x, 1),
                        "bits": info.bits_per_sample,
                        "has_ir": info.has_ir,
                        "is_linear_hdr": info.is_linear_hdr,
                        "software": info.software,
                        "has_layout": (SCANS_DIR / f"{p.name}.layout.json").exists(),
                    }
                    self.info_cache[key] = (mtime, meta)
                except Exception as exc:
                    meta = {
                        "name": p.name,
                        "path": str(p),
                        "size_bytes": p.stat().st_size,
                        "mtime": mtime,
                        "error": str(exc),
                    }
            # Refresh has_layout flag
            meta = dict(meta)
            meta["has_layout"] = (SCANS_DIR / f"{p.name}.layout.json").exists()
            out.append(meta)
        return out

    def get_preview_bytes(self, scan_path: Path, kind: str, mode: str, ir_sens: float, max_dim: int) -> tuple[bytes, str]:
        mtime = scan_path.stat().st_mtime
        key = (str(scan_path), kind, mode, round(ir_sens, 2), max_dim)
        cached = self.preview_cache.get(key)
        if cached and cached[0] == mtime:
            ctype = "image/png" if kind == "ir_mask" else "image/jpeg"
            return cached[1], ctype

        inv = INVERT_MODES.get(mode, 0)
        sens = ir_sens if kind == "ir_mask" else 0.0
        rgb8, ir8, mask8 = tiff_bridge.build_preview(scan_path, max_dim=max_dim, invert_mode=inv, ir_sensitivity=sens)

        bio = io.BytesIO()
        if kind == "rgb":
            Image.fromarray(rgb8, mode="RGB").save(bio, format="JPEG", quality=88)
            ctype = "image/jpeg"
        elif kind == "ir_raw":
            Image.fromarray(ir8, mode="L").save(bio, format="JPEG", quality=88)
            ctype = "image/jpeg"
        elif kind == "ir_mask":
            h, w = mask8.shape
            rgba = np.zeros((h, w, 4), dtype=np.uint8)
            hits = mask8 > 0
            # High-contrast magenta/error dust mask overlay
            rgba[hits, 0] = 255
            rgba[hits, 1] = 77
            rgba[hits, 2] = 255
            rgba[hits, 3] = 215
            Image.fromarray(rgba, mode="RGBA").save(bio, format="PNG", compress_level=3)
            ctype = "image/png"
        else:
            raise ValueError(f"Unknown preview kind: {kind}")

        data = bio.getvalue()
        # Keep cache bounded (~24 entries)
        if len(self.preview_cache) > 24:
            self.preview_cache.pop(next(iter(self.preview_cache)))
        self.preview_cache[key] = (mtime, data)
        return data, ctype

    def get_region_bytes(
        self,
        scan_path: Path,
        kind: str,
        mode: str,
        rx: int,
        ry: int,
        rw: int,
        rh: int,
        out_w: int,
        out_h: int,
        ir_sens: float,
    ) -> tuple[bytes, str]:
        inv = INVERT_MODES.get(mode, 0)
        rgb8, ir8, mask8 = tiff_bridge.read_region(
            scan_path,
            rx=rx,
            ry=ry,
            rw=rw,
            rh=rh,
            out_w=out_w,
            out_h=out_h,
            invert_mode=inv,
            want_ir_raw=(kind == "ir_raw"),
            want_ir_mask=(kind == "ir_mask"),
            ir_sensitivity=ir_sens,
        )
        bio = io.BytesIO()
        if kind == "rgb":
            Image.fromarray(rgb8, mode="RGB").save(bio, format="JPEG", quality=90)
            return bio.getvalue(), "image/jpeg"
        if kind == "ir_raw" and ir8 is not None:
            Image.fromarray(ir8, mode="L").save(bio, format="JPEG", quality=90)
            return bio.getvalue(), "image/jpeg"
        if kind == "ir_mask" and mask8 is not None:
            h, w = mask8.shape
            rgba = np.zeros((h, w, 4), dtype=np.uint8)
            hits = mask8 > 0
            rgba[hits, 0] = 255
            rgba[hits, 1] = 77
            rgba[hits, 2] = 255
            rgba[hits, 3] = 215
            Image.fromarray(rgba, mode="RGBA").save(bio, format="PNG", compress_level=2)
            return bio.getvalue(), "image/png"
        raise ValueError(f"Unsupported region kind: {kind}")

    def start_scan(self, req: ScanRequest) -> dict:
        with self.lock:
            if self.job.status == "running":
                raise RuntimeError("A scan is already in progress")
            backend = self.backends.get(req.backend)
            if not backend:
                raise ValueError(f"Unknown backend: {req.backend}")
            ok, msg = backend.is_available()
            if not ok:
                raise RuntimeError(f"Backend {req.backend} unavailable: {msg}")

            self.cancel_event.clear()
            self.job = JobState(
                job_id=uuid.uuid4().hex[:8],
                status="running",
                backend=req.backend,
                stage="INITIALIZING",
                progress=2.0,
                started_at=time.time(),
                logs=[f"[{time.strftime('%H:%M:%S')}] Starting {req.backend} scan ({req.dpi} DPI, {req.film_format})"],
            )

        def _worker() -> None:
            last_prefix = ""

            def log_cb(msg: str, prog: float | None = None) -> None:
                nonlocal last_prefix
                with self.lock:
                    self.job.stage = msg
                    if prog is not None:
                        self.job.progress = prog
                    line = f"[{time.strftime('%H:%M:%S')}] {msg}"
                    # Live progress ticks (same pass/phase prefix) update last log line in-place
                    # so the log box stays clean while transitions append new lines.
                    prefix = msg.split("(", 1)[0].split("·", 2)
                    cur_prefix = "·".join(prefix[:2]).strip() if (
                        msg.startswith("PASS ") or msg.startswith("CALIBRATING") or msg.startswith("WRITING TIFF") or msg.startswith("VUESCAN ")
                    ) else ""
                    if cur_prefix and cur_prefix == last_prefix and self.job.logs:
                        self.job.logs[-1] = line
                    else:
                        self.job.logs.append(line)
                        if len(self.job.logs) > 200:
                            self.job.logs = self.job.logs[-200:]
                        last_prefix = cur_prefix

            try:
                out_file = backend.scan(req, SCANS_DIR, log_cb, self.cancel_event)
                with self.lock:
                    self.job.status = "done"
                    self.job.progress = 100.0
                    self.job.output_file = out_file.name
                    self.job.finished_at = time.time()
            except Exception as exc:
                with self.lock:
                    self.job.status = "cancelled" if self.cancel_event.is_set() else "error"
                    self.job.error = str(exc)
                    self.job.stage = "CANCELLED" if self.cancel_event.is_set() else "ERROR"
                    self.job.logs.append(f"[{time.strftime('%H:%M:%S')}] ERR: {exc}")
                    self.job.finished_at = time.time()

        t = threading.Thread(target=_worker, daemon=True)
        t.start()
        return self.job.to_dict()

    def cancel_scan(self) -> dict:
        with self.lock:
            if self.job.status == "running":
                self.cancel_event.set()
                self.job.stage = "CANCELLING (PARKING CARRIAGE)"
                self.job.logs.append(f"[{time.strftime('%H:%M:%S')}] Cancel requested by user")
            return self.job.to_dict()


STATE = AppState()


def _safe_scan_path(filename: str) -> Path:
    clean = Path(urllib.parse.unquote(filename)).name
    p = (SCANS_DIR / clean).resolve()
    if p.parent != SCANS_DIR.resolve() or not p.exists():
        raise FileNotFoundError(f"Scan not found: {clean}")
    return p


class CockpitHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt: str, *args: Any) -> None:
        # Compact console logging (skip noisy SSE/status polls)
        msg = fmt % args
        if "/api/status" in msg or "/api/events" in msg:
            return
        print(f"[http] {self.address_string()} {msg}", flush=True)

    def _send_json(self, obj: Any, status: int = 200) -> None:
        raw = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(raw)

    def _send_bytes(self, data: bytes, ctype: str, cache: str = "no-store") -> None:
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", cache)
        self.end_headers()
        self.wfile.write(data)

    def _read_json(self) -> dict:
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0:
            return {}
        raw = self.rfile.read(length)
        return json.loads(raw.decode("utf-8"))

    def do_GET(self) -> None:
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        qs = urllib.parse.parse_qs(parsed.query)

        try:
            if path == "/api/status":
                backends_info = {}
                for name, b in STATE.backends.items():
                    ok, note = b.is_available()
                    backends_info[name] = {"available": ok, "note": note}
                with STATE.lock:
                    job_dict = STATE.job.to_dict()
                self._send_json(
                    {
                        "scanner": detect_usb_scanner(),
                        "backends": backends_info,
                        "job": job_dict,
                        "default_export_dir": str(EXPORTS_DIR),
                        "scans_dir": str(SCANS_DIR),
                    }
                )
                return

            if path == "/api/scans":
                self._send_json({"scans": STATE.list_scans()})
                return

            if path == "/api/events":
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-cache")
                self.send_header("Connection", "keep-alive")
                self.end_headers()
                try:
                    while True:
                        with STATE.lock:
                            payload = json.dumps({"job": STATE.job.to_dict(), "scanner": detect_usb_scanner()})
                        chunk = f"data: {payload}\n\n".encode("utf-8")
                        self.wfile.write(chunk)
                        self.wfile.flush()
                        time.sleep(0.8)
                except (BrokenPipeError, ConnectionResetError):
                    return

            if path.startswith("/api/scan/"):
                parts = path.split("/")
                # /api/scan/<filename>/<action>
                if len(parts) == 5:
                    scan_path = _safe_scan_path(parts[3])
                    action = parts[4]
                    if action in ("preview", "ir_raw", "ir_mask"):
                        kind = "rgb" if action == "preview" else action
                        mode = qs.get("mode", ["raw"])[0]
                        ir_sens = float(qs.get("ir_sens", ["0.5"])[0])
                        max_dim = int(qs.get("max_dim", ["1800"])[0])
                        data, ctype = STATE.get_preview_bytes(scan_path, kind, mode, ir_sens, max_dim)
                        self._send_bytes(data, ctype, cache="private, max-age=30")
                        return
                    if action == "region":
                        kind = qs.get("kind", ["rgb"])[0]
                        mode = qs.get("mode", ["raw"])[0]
                        rx = int(float(qs.get("x", ["0"])[0]))
                        ry = int(float(qs.get("y", ["0"])[0]))
                        rw = max(1, int(float(qs.get("w", ["512"])[0])))
                        rh = max(1, int(float(qs.get("h", ["512"])[0])))
                        out_w = min(2000, max(16, int(float(qs.get("out_w", [str(rw)])[0]))))
                        out_h = min(2000, max(16, int(float(qs.get("out_h", [str(rh)])[0]))))
                        ir_sens = float(qs.get("ir_sens", ["0.5"])[0])
                        data, ctype = STATE.get_region_bytes(
                            scan_path, kind, mode, rx, ry, rw, rh, out_w, out_h, ir_sens
                        )
                        self._send_bytes(data, ctype, cache="no-store")
                        return
                    if action == "layout":
                        lp = SCANS_DIR / f"{scan_path.name}.layout.json"
                        if lp.exists():
                            self._send_json(json.loads(lp.read_text(encoding="utf-8")))
                        else:
                            self._send_json({"exists": False})
                        return

            if path == "/favicon.ico":
                self.send_response(204)
                self.end_headers()
                return

            # Static frontend files
            rel = "index.html" if path in ("/", "") else path.lstrip("/")
            file_path = (WEB_DIR / rel).resolve()
            if not str(file_path).startswith(str(WEB_DIR.resolve())) or not file_path.is_file():
                self._send_json({"error": "Not found"}, status=HTTPStatus.NOT_FOUND)
                return
            ctype, _ = mimetypes.guess_type(str(file_path))
            if file_path.suffix == ".woff2":
                ctype = "font/woff2"
            self._send_bytes(file_path.read_bytes(), ctype or "application/octet-stream", cache="no-cache")
        except (BrokenPipeError, ConnectionResetError):
            return
        except FileNotFoundError as exc:
            self._send_json({"error": str(exc)}, status=404)
        except Exception as exc:
            try:
                self._send_json({"error": str(exc)}, status=500)
            except (BrokenPipeError, ConnectionResetError):
                return

    def do_POST(self) -> None:
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path

        try:
            body = self._read_json()

            if path == "/api/scan/start":
                req = ScanRequest(
                    backend=str(body.get("backend", "silverfast")),
                    roll_name=str(body.get("roll_name", "roll")),
                    dpi=int(body.get("dpi", 2400)),
                    film_format=str(body.get("film_format", "half_frame")),
                    film_type=str(body.get("film_type", "color_neg")),
                    multi_exposure=bool(body.get("multi_exposure", False)),
                    ir_enabled=bool(body.get("ir_enabled", True)),
                    ir_level=str(body.get("ir_level", "light")),
                    dng_output=bool(body.get("dng_output", False)),
                )
                job = STATE.start_scan(req)
                self._send_json({"job": job})
                return

            if path == "/api/scan/cancel":
                self._send_json({"job": STATE.cancel_scan()})
                return

            if path.startswith("/api/scan/"):
                parts = path.split("/")
                if len(parts) == 5:
                    scan_path = _safe_scan_path(parts[3])
                    action = parts[4]
                    if action == "detect":
                        t0 = time.perf_counter()
                        res = detector.detect_frames(
                            scan_path,
                            film_format=body.get("film_format", "half_frame"),
                            film_type=body.get("film_type", "color_neg"),
                            inset_pct=float(body.get("inset_pct", 0.0)),
                            skip_empty=bool(body.get("skip_empty", True)),
                        )
                        res["elapsed_ms"] = round((time.perf_counter() - t0) * 1000.0, 1)
                        self._send_json(res)
                        return
                    if action == "layout":
                        lp = SCANS_DIR / f"{scan_path.name}.layout.json"
                        lp.write_text(json.dumps(body, indent=2), encoding="utf-8")
                        self._send_json({"saved": True, "path": str(lp)})
                        return
                    if action == "delete":
                        lp = SCANS_DIR / f"{scan_path.name}.layout.json"
                        scan_path.unlink(missing_ok=True)
                        lp.unlink(missing_ok=True)
                        # Drop cached entries for this scan
                        key_str = str(scan_path)
                        STATE.info_cache.pop(key_str, None)
                        for k in [k for k in STATE.preview_cache if k[0] == key_str]:
                            STATE.preview_cache.pop(k, None)
                        self._send_json({"deleted": True, "name": scan_path.name})
                        return

            if path == "/api/export":
                scan_name = str(body.get("scan_name", ""))
                scan_path = _safe_scan_path(scan_name)
                frames = body.get("frames", [])
                out_dir = Path(str(body.get("out_dir", str(EXPORTS_DIR)))).expanduser()
                roll_name = str(body.get("roll_name", scan_path.stem))
                apply_ir = bool(body.get("apply_ir_clean", True))
                ir_sens = float(body.get("ir_sensitivity", 0.5))
                gamma_22 = bool(body.get("gamma_22", False))
                export_proof = bool(body.get("export_positive_proof", True))
                film_type = str(body.get("film_type", "color_neg"))
                pos_mode = INVERT_MODES.get(film_type, 1)

                target_sub = out_dir / ("".join(c if (c.isalnum() or c in "-_") else "_" for c in roll_name).strip("_") or "roll")
                t0 = time.perf_counter()
                exported = tiff_bridge.export_frames(
                    src_path=scan_path,
                    frames=frames,
                    out_dir=target_sub,
                    roll_name=roll_name,
                    apply_ir_clean=apply_ir,
                    ir_sensitivity=ir_sens,
                    gamma_22=gamma_22,
                    export_positive_proof=export_proof,
                    positive_mode=pos_mode,
                )
                elapsed_ms = round((time.perf_counter() - t0) * 1000.0, 1)
                self._send_json(
                    {
                        "ok": True,
                        "out_dir": str(target_sub),
                        "count": len(exported),
                        "elapsed_ms": elapsed_ms,
                        "files": exported,
                    }
                )
                return

            self._send_json({"error": "Unknown endpoint"}, status=404)
        except Exception as exc:
            self._send_json({"error": str(exc)}, status=400)


def main() -> None:
    parser = argparse.ArgumentParser(description="Film Cockpit Web Orchestrator")
    parser.add_argument("--host", default="0.0.0.0", help="Bind host (default 0.0.0.0 for LAN access)")
    parser.add_argument("--port", type=int, default=8765, help="Port (default 8765)")
    args = parser.parse_args()

    SCANS_DIR.mkdir(parents=True, exist_ok=True)
    EXPORTS_DIR.mkdir(parents=True, exist_ok=True)

    srv = ThreadingHTTPServer((args.host, args.port), CockpitHandler)
    print(f"FILM COCKPIT listening on http://{args.host}:{args.port} (local: http://127.0.0.1:{args.port})", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down Film Cockpit...", flush=True)
        srv.server_close()


if __name__ == "__main__":
    main()
