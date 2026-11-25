#!/usr/bin/env python3
"""Headless CLI driver for VueScan + Epson Perfection V700/V750 on Linux.

Runs VueScan inside an isolated Xvfb display (no visible GUI), generates a
clean vuescan.ini for full-holder Transparency scanning (with optional
Multi-Exposure and Infrared Clean), triggers Scan, monitors vuescan.log for
progress, and shuts VueScan down cleanly without ever stranding USB state.
"""
import argparse
import os
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent
VUESCAN_HOME = BASE_DIR / "vuescan_home"
SCANS_DIR = BASE_DIR / "scans"

# Epson V700/V750 Transparency mode resolution indices in VueScan
# (verified against VueScan 9.8.60; 0=Auto)
DPI_TO_INDEX = {
    4800: 1,
    2400: 2,
    1600: 3,
    1200: 4,
    800: 5,
    600: 6,
    400: 7,   # Note: verify exact index mapping for 200/300/400/2400
    300: 8,
    200: 9,
}

IR_CLEAN_MAP = {
    "none": 0,
    "light": 1,
    "medium": 2,
    "heavy": 3,
}

MEDIA_MAP = {
    "image": 0,
    "slide": 1,
    "color_neg": 2,
    "bw_neg": 3,
}


def write_vuescan_ini(
    output_dir: Path,
    dpi_index: int,
    media: str,
    multi_exposure: bool,
    ir_clean: str,
    dng: bool,
) -> Path:
    ini_dir = VUESCAN_HOME / ".vuescan"
    ini_dir.mkdir(parents=True, exist_ok=True)
    output_dir.mkdir(parents=True, exist_ok=True)

    ir_val = IR_CLEAN_MAP[ir_clean]
    # Use 64-bit RGBI whenever IR clean is active, else 48-bit RGB
    bpp_val = 6 if ir_val > 0 else 5
    media_val = MEDIA_MAP[media]
    me_val = 1 if multi_exposure else 0
    dng_val = 1 if dng else 0
    raw_type = 4 if ir_val == 0 else 4  # 48-bit RGB output (with IR baked in when RawSaveFilm=1)

    ini = f"""[VueScan]
[Input]
Options=2
Source=PerfectionV700
Mode=1
DefaultFolder={output_dir}
[Input-PerfectionV700]
Mode=1
[Input-PerfectionV700-Transparency]
Media={media_val}
BitsPerPixel={bpp_val}
MultiExposure={me_val}
ScanResolution={dpi_index}
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
DefaultFolder={output_dir}
[Output-PerfectionV700-Transparency]
DefaultFolder={output_dir}
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
    ini_path = ini_dir / "vuescan.ini"
    ini_path.write_text(ini)
    return ini_path


def run_xdotool(display: str, *args: str) -> str:
    env = os.environ.copy()
    env["DISPLAY"] = display
    res = subprocess.run(
        ["xdotool", *args],
        env=env,
        capture_output=True,
        text=True,
    )
    return res.stdout.strip()


def pick_free_display() -> str:
    for num in range(40, 120):
        if not Path(f"/tmp/.X{num}-lock").exists():
            return f":{num}"
    raise RuntimeError("No free Xvfb display found")


def scan_holder(
    out_path: Path,
    dpi_index: int = 7,
    media: str = "image",
    multi_exposure: bool = False,
    ir_clean: str = "light",
    dng: bool = False,
    timeout_sec: int = 900,
) -> Path:
    staging_dir = BASE_DIR / ".vuescan_staging"
    if staging_dir.exists():
        shutil.rmtree(staging_dir)
    staging_dir.mkdir(parents=True, exist_ok=True)

    write_vuescan_ini(
        output_dir=staging_dir,
        dpi_index=dpi_index,
        media=media,
        multi_exposure=multi_exposure,
        ir_clean=ir_clean,
        dng=dng,
    )

    display = pick_free_display()
    xvfb = subprocess.Popen(
        ["Xvfb", display, "-screen", "0", "1280x900x24", "-nolisten", "tcp"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    vuescan_proc = None
    try:
        time.sleep(0.8)
        env = os.environ.copy()
        env["HOME"] = str(VUESCAN_HOME)
        env["DISPLAY"] = display
        env["GDK_BACKEND"] = "x11"

        log_out = open(VUESCAN_HOME / "stdout.log", "w")
        vuescan_proc = subprocess.Popen(
            ["vuescan"],
            env=env,
            stdout=log_out,
            stderr=subprocess.STDOUT,
        )

        # Wait for VueScan main window
        main_wid = ""
        for _ in range(40):
            main_wid = run_xdotool(display, "search", "--onlyvisible", "--name", "VueScan 9")
            if main_wid:
                main_wid = main_wid.splitlines()[0]
                break
            time.sleep(0.5)
        if not main_wid:
            raise RuntimeError("VueScan window did not appear (is the scanner connected/powered on?)")

        time.sleep(2.0)

        # Verify VueScan actually claimed the real Epson scanner (not fallback 'disk' mode)
        vs_log = VUESCAN_HOME / ".vuescan" / "vuescan.log"
        if vs_log.exists():
            log_text = vs_log.read_text(errors="replace")
            if "could not claim interface" in log_text:
                raise RuntimeError("Scanner USB interface is busy (likely in use by SilverFast or another process)")
            if "Source driver                        : disk" in log_text:
                raise RuntimeError("VueScan did not detect the physical Epson scanner (check power/USB)")

        # Dismiss startup dialogs if present (Tip of the Day / Unregistered About)
        tip_wid = run_xdotool(display, "search", "--onlyvisible", "--name", "Tip of the Day")
        if tip_wid:
            run_xdotool(display, "mousemove", "784", "562", "click", "1")
            time.sleep(0.4)

        about_wid = run_xdotool(display, "search", "--onlyvisible", "--name", "About")
        if about_wid:
            run_xdotool(display, "mousemove", "804", "645", "click", "1")
            time.sleep(0.5)

        # Trigger Scan button (155, 855)
        print("Triggering headless VueScan scan...", flush=True)
        run_xdotool(display, "mousemove", "155", "855", "click", "1")

        # Poll staging directory for finished scan file; auto-dismiss trial nag dialog
        ext = "*.dng" if dng else "*.tif"
        start_ts = time.time()
        saved_file = None
        last_status = 0.0

        while time.time() - start_ts < timeout_sec:
            if vuescan_proc.poll() is not None:
                raise RuntimeError("VueScan exited unexpectedly during scan")

            # Dismiss trial watermark nag dialog ("No" button at 485, 635)
            wins = [w for w in run_xdotool(display, "search", "--onlyvisible", "--name", ".").splitlines() if w]
            if len(wins) > 1:
                run_xdotool(display, "mousemove", "485", "635", "click", "1")
                time.sleep(0.5)

            matches = list(staging_dir.glob(ext))
            if matches:
                cand = matches[0]
                sz1 = cand.stat().st_size
                if sz1 > 10000:
                    time.sleep(1.5)
                    sz2 = cand.stat().st_size
                    if sz1 == sz2:
                        saved_file = cand
                        break

            now = time.time()
            if now - last_status >= 10:
                print(f"  scanning... ({int(now - start_ts)}s elapsed)", flush=True)
                last_status = now
            time.sleep(1.0)

        if not saved_file:
            # Click Cancel cleanly so scanner parks carriage before exiting!
            print("Timeout reached — clicking Cancel cleanly...", flush=True)
            run_xdotool(display, "mousemove", "50", "855", "click", "1")
            time.sleep(5.0)
            raise TimeoutError(f"Scan did not finish within {timeout_sec}s")

        # Clean quit
        run_xdotool(display, "key", "ctrl+q")
        try:
            vuescan_proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            vuescan_proc.terminate()
            vuescan_proc.wait(timeout=5)

        out_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(saved_file), str(out_path))
        print(f"Saved scan to {out_path} ({out_path.stat().st_size / (1024*1024):.1f} MB)", flush=True)
        return out_path
    finally:
        if vuescan_proc and vuescan_proc.poll() is None:
            vuescan_proc.terminate()
            try:
                vuescan_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                vuescan_proc.kill()
        xvfb.terminate()
        xvfb.wait()


def main():
    parser = argparse.ArgumentParser(description="Headless VueScan CLI for Epson V750")
    parser.add_argument("--out", required=True, help="Output TIFF/DNG file path")
    parser.add_argument("--dpi-index", type=int, default=7, help="VueScan ScanResolution index (default 7 = 200dpi)")
    parser.add_argument("--media", choices=list(MEDIA_MAP.keys()), default="image")
    parser.add_argument("--multi-exposure", action="store_true")
    parser.add_argument("--ir-clean", choices=list(IR_CLEAN_MAP.keys()), default="light")
    parser.add_argument("--dng", action="store_true")
    parser.add_argument("--timeout", type=int, default=900)
    args = parser.parse_args()

    scan_holder(
        out_path=Path(args.out).resolve(),
        dpi_index=args.dpi_index,
        media=args.media,
        multi_exposure=args.multi_exposure,
        ir_clean=args.ir_clean,
        dng=args.dng,
        timeout_sec=args.timeout,
    )


if __name__ == "__main__":
    main()
