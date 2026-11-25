"""Headless VueScan backend (vuescan.ini + Xvfb + xdotool)."""
from __future__ import annotations

import os
import shutil
import subprocess
import threading
import time
from pathlib import Path
from typing import Callable, Optional

from .base import ScannerBackend, ScanRequest, pick_free_display

BASE_DIR = Path(__file__).resolve().parents[2]
VUESCAN_HOME = BASE_DIR / "vuescan_home"

# Epson V700/V750 Transparency mode ScanResolution indices in VueScan 9.8
# (verified against VueScan 9.8.60)
VUESCAN_DPI_INDEX = {
    4800: 1,
    3200: 2,
    2400: 2,
    1600: 3,
    1200: 4,
    800: 5,
    600: 6,
    400: 6,
    300: 7,
    200: 7,
}

IR_CLEAN_MAP = {"none": 0, "light": 1, "medium": 2, "heavy": 3}
MEDIA_MAP = {"image": 0, "slide": 1, "color_neg": 0, "bw_neg": 0}


def _run_xdotool(display: str, *args: str) -> str:
    env = os.environ.copy()
    env["DISPLAY"] = display
    res = subprocess.run(["xdotool", *args], env=env, capture_output=True, text=True)
    return res.stdout.strip()


class VueScanBackend(ScannerBackend):
    name = "vuescan"

    def is_available(self) -> tuple[bool, str]:
        if not shutil.which("vuescan"):
            return False, "vuescan binary not installed"
        if not shutil.which("Xvfb") or not shutil.which("xdotool"):
            return False, "Xvfb or xdotool missing"
        rc_path = VUESCAN_HOME / ".vuescanrc"
        licensed = False
        if rc_path.exists():
            txt = rc_path.read_text(errors="replace")
            if "SerialNumber=0" not in txt and "SerialNumber=" in txt:
                licensed = True
        note = "VueScan 9 Pro (Licensed)" if licensed else "VueScan 9 (Trial — watermarked)"
        return True, note

    def _write_ini(self, req: ScanRequest, staging_dir: Path) -> None:
        ini_dir = VUESCAN_HOME / ".vuescan"
        ini_dir.mkdir(parents=True, exist_ok=True)

        dpi_idx = VUESCAN_DPI_INDEX.get(req.dpi, 2)
        ir_val = IR_CLEAN_MAP.get(req.ir_level, 1) if req.ir_enabled else 0
        bpp_val = 6 if req.ir_enabled else 5  # 64-bit RGBI vs 48-bit RGB
        me_val = 1 if req.multi_exposure else 0
        dng_val = 1 if req.dng_output else 0
        # RawFileType=4 saves 48-bit RGB (with IR Clean baked in when RawSaveFilm=1),
        # or 5 saves 64-bit RGBI if IR channel preserved.
        raw_type = 4

        ini = f"""[VueScan]
[Input]
Options=2
Source=PerfectionV700
Mode=1
DefaultFolder={staging_dir}
[Input-PerfectionV700]
Mode=1
[Input-PerfectionV700-Transparency]
Media=0
BitsPerPixel={bpp_val}
MultiExposure={me_val}
ScanResolution={dpi_idx}
BatchScan=0
AutoSkew=0
[Crop-PerfectionV700-Transparency]
CropSize=0
AutoOffset=0
[Filter]
InfraredClean={ir_val}
[Filter-PerfectionV700-Transparency]
InfraredClean={ir_val}
[Output]
DefaultFolder={staging_dir}
[Output-PerfectionV700-Transparency]
DefaultFolder={staging_dir}
AutoFileName=0
PDFFile=0
JPEGFile=0
TIFFFile=0
RawFile=1
RawFileType={raw_type}
RawOutputWith=2
RawSaveFilm=1
RawDNGFormat={dng_val}
[Prefs]
StartupTip=0
EnableSound=0
ExternalViewer=0
WindowXSize=1280
WindowYSize=900
"""
        (ini_dir / "vuescan.ini").write_text(ini)

    def scan(
        self,
        req: ScanRequest,
        dest_dir: Path,
        log_cb: Callable[[str, Optional[float]], None],
        cancel_event: threading.Event,
    ) -> Path:
        staging_dir = BASE_DIR / ".vuescan_staging"
        if staging_dir.exists():
            shutil.rmtree(staging_dir)
        staging_dir.mkdir(parents=True, exist_ok=True)

        safe_roll = "".join(c if (c.isalnum() or c in "-_") else "_" for c in (req.roll_name or "roll")).strip("_") or "roll"
        ts = time.strftime("%Y%m%d_%H%M%S")
        ext = "dng" if req.dng_output else "tif"
        out_path = dest_dir / f"{safe_roll}_vs_{req.dpi}dpi_{ts}.{ext}"

        log_cb(f"Writing vuescan.ini ({req.dpi} DPI, ME={'ON' if req.multi_exposure else 'OFF'}, IR={req.ir_level if req.ir_enabled else 'OFF'})", 5.0)
        self._write_ini(req, staging_dir)

        display = pick_free_display(60)
        xvfb = subprocess.Popen(
            ["Xvfb", display, "-screen", "0", "1280x900x24", "-nolisten", "tcp"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        vs_proc = None
        try:
            time.sleep(0.8)
            env = os.environ.copy()
            env["HOME"] = str(VUESCAN_HOME)
            env["DISPLAY"] = display
            env["GDK_BACKEND"] = "x11"

            log_cb("Launching VueScan headlessly in Xvfb...", 10.0)
            log_out = open(VUESCAN_HOME / "stdout.log", "w")
            vs_proc = subprocess.Popen(["vuescan"], env=env, stdout=log_out, stderr=subprocess.STDOUT)

            main_wid = ""
            for _ in range(40):
                if cancel_event.is_set():
                    raise RuntimeError("Cancelled before launch")
                main_wid = _run_xdotool(display, "search", "--onlyvisible", "--name", "VueScan 9")
                if main_wid:
                    break
                time.sleep(0.5)
            if not main_wid:
                raise RuntimeError("VueScan window did not appear")

            time.sleep(2.0)

            vs_log = VUESCAN_HOME / ".vuescan" / "vuescan.log"
            if vs_log.exists():
                ltxt = vs_log.read_text(errors="replace")
                if "could not claim interface" in ltxt:
                    raise RuntimeError("Scanner USB interface is busy (in use by another process)")
                if "Source driver                        : disk" in ltxt:
                    raise RuntimeError("VueScan did not detect the physical V750 (check power/USB)")

            # Dismiss Tip / About dialogs
            if _run_xdotool(display, "search", "--onlyvisible", "--name", "Tip of the Day"):
                _run_xdotool(display, "mousemove", "784", "562", "click", "1")
                time.sleep(0.4)
            if _run_xdotool(display, "search", "--onlyvisible", "--name", "About"):
                _run_xdotool(display, "mousemove", "804", "645", "click", "1")
                time.sleep(0.5)

            log_cb(f"Scanning full holder via VueScan ({req.dpi} DPI)...", 20.0)
            _run_xdotool(display, "mousemove", "155", "855", "click", "1")

            timeout_sec = 14400
            t0 = time.time()
            saved_file: Optional[Path] = None
            last_log = 0.0

            while time.time() - t0 < timeout_sec:
                if cancel_event.is_set():
                    log_cb("Cancelling VueScan cleanly...", None)
                    _run_xdotool(display, "mousemove", "50", "855", "click", "1")
                    time.sleep(5.0)
                    raise RuntimeError("Scan cancelled by user")

                if vs_proc.poll() is not None:
                    raise RuntimeError("VueScan exited unexpectedly during scan")

                # Dismiss trial watermark nag dialog ("No" at 485, 635)
                wins = [w for w in _run_xdotool(display, "search", "--onlyvisible", "--name", ".").splitlines() if w]
                if len(wins) > 1:
                    _run_xdotool(display, "mousemove", "485", "635", "click", "1")
                    time.sleep(0.5)

                matches = list(staging_dir.glob(f"*.{ext}"))
                if matches:
                    cand = matches[0]
                    sz1 = cand.stat().st_size
                    if sz1 > 10000:
                        time.sleep(3.0)
                        sz2 = cand.stat().st_size
                        if sz1 == sz2:
                            saved_file = cand
                            break

                elapsed = time.time() - t0
                if elapsed - last_log >= 5.0:
                    est_total = max(60.0, (req.dpi / 200.0) * 60.0 * (1.8 if req.multi_exposure else 1.0) * (1.3 if req.ir_enabled else 1.0))
                    pct = min(94.0, 10.0 + 84.0 * (elapsed / est_total))
                    passes = "RGB" + ("+ME" if req.multi_exposure else "") + ("+IR" if req.ir_enabled else "")
                    log_cb(f"VUESCAN {passes} · {int(elapsed)}s (EST ~{int(pct)}%)", round(pct, 1))
                    last_log = elapsed
                time.sleep(1.0)

            if not saved_file:
                _run_xdotool(display, "mousemove", "50", "855", "click", "1")
                time.sleep(5.0)
                raise TimeoutError("VueScan scan timed out")

            log_cb("Closing VueScan cleanly...", 95.0)
            _run_xdotool(display, "key", "ctrl+q")
            try:
                vs_proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                vs_proc.terminate()
                vs_proc.wait(timeout=5)

            dest_dir.mkdir(parents=True, exist_ok=True)
            shutil.move(str(saved_file), str(out_path))
            mb = out_path.stat().st_size / (1024 * 1024)
            log_cb(f"Saved {out_path.name} ({mb:.1f} MB)", 100.0)
            return out_path
        finally:
            if vs_proc and vs_proc.poll() is None:
                vs_proc.terminate()
                try:
                    vs_proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    vs_proc.kill()
            xvfb.terminate()
            xvfb.wait()
