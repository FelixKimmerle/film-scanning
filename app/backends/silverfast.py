"""Headless SilverFast SE Plus 9 backend (Wine + silverfast-linux + Xvfb).

Automatically stays under SilverFast's classic 32-bit TIFF 4.0 GiB ceiling:
  - <= 3000 DPI (64-bit HDRi) or <= 3200 DPI (48-bit no-IR): single full-holder pass.
  - 3200 DPI + IR: auto-splits into 2 halves along the 17.6mm center holder spine
    (Strips 1-2 + Strips 3-4) and stitches into one seamless 64-bit HDRi BigTIFF.
  - 4800 DPI + IR: auto-splits into 4 individual strips along the plastic dividers
    and stitches into one seamless 64-bit HDRi BigTIFF.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import threading
import time
from pathlib import Path
from typing import Callable, Optional

from ..core import tiff_bridge
from .base import ScannerBackend, ScanRequest, pick_free_display

BASE_DIR = Path(__file__).resolve().parents[2]
SFL_WRAPPER = BASE_DIR / "sfl.sh"
SFL_PREFIX = BASE_DIR / "silverfast-data" / "prefix"

# Trimmed full V750 35mm 4-strip holder area (+0.15" margin outside all 4 strips)
HOLDER_Y0 = 0.50
HOLDER_Y1 = 9.70
HOLDER_X0 = 0.00
HOLDER_X1 = 5.85

# Deterministic classic 32-bit TIFF ceiling (4,294,967,295 B) minus generous 245 MB safety cushion
CLASSIC_TIFF_SAFE_BYTES = 4_050_000_000


def _estimate_tiff_bytes(w_in: float, h_in: float, dpi: int, ir_enabled: bool) -> int:
    # 16-bit RGB = 6 B/px, 16-bit IR = 2 B/px, plus ~32 MB thumbnail & TIFF directory overhead
    bpp = 8 if ir_enabled else 6
    return int(round(w_in * dpi)) * int(round(h_in * dpi)) * bpp + 32_000_000


def _plan_scan_tiles(dpi: int, ir_enabled: bool) -> list[tuple[str, float, float, float, float]]:
    """Return list of (label, y0, x0, y1, x1) tiles in inches guaranteed < 4.0 GiB each."""
    h_in = HOLDER_Y1 - HOLDER_Y0
    if _estimate_tiff_bytes(HOLDER_X1 - HOLDER_X0, h_in, dpi, ir_enabled) <= CLASSIC_TIFF_SAFE_BYTES:
        return [("FULL HOLDER", HOLDER_Y0, HOLDER_X0, HOLDER_Y1, HOLDER_X1)]

    # 2-way split dead-center on the 17.6mm plastic spine between Strip 2 and Strip 3 (X = 2.90")
    if _estimate_tiff_bytes(2.95, h_in, dpi, ir_enabled) <= CLASSIC_TIFF_SAFE_BYTES:
        return [
            ("LEFT (STRIPS 1-2)", HOLDER_Y0, HOLDER_X0, HOLDER_Y1, 2.90),
            ("RIGHT (STRIPS 3-4)", HOLDER_Y0, 2.90, HOLDER_Y1, HOLDER_X1),
        ]

    # 4-way split dead-center on all 3 plastic dividers (1.32", 2.90", 4.48")
    return [
        ("STRIP 1", HOLDER_Y0, HOLDER_X0, HOLDER_Y1, 1.32),
        ("STRIP 2", HOLDER_Y0, 1.32, HOLDER_Y1, 2.90),
        ("STRIP 3", HOLDER_Y0, 2.90, HOLDER_Y1, 4.48),
        ("STRIP 4", HOLDER_Y0, 4.48, HOLDER_Y1, HOLDER_X1),
    ]


def _format_area_inches(y0: float, x0: float, y1: float, x1: float) -> str:
    return f"{y0:.16e}%20{x0:.16e}%20{y1:.16e}%20{x1:.16e}%200.0000000000000000e+00%20"


def _find_sf_roaming_dir() -> Optional[Path]:
    users_dir = SFL_PREFIX / "drive_c" / "users"
    if not users_dir.exists():
        return None
    for user in users_dir.iterdir():
        p = user / "AppData" / "Roaming" / "LaserSoft Imaging" / "SilverFast 9"
        if p.is_dir():
            return p
    return None


def _find_prefs_dir() -> Optional[Path]:
    roam = _find_sf_roaming_dir()
    if not roam:
        return None
    p = roam / "Prefs"
    return p if p.is_dir() else None


def _read_log_tail(log_path: Optional[Path], offset: int) -> str:
    if not log_path or not log_path.exists():
        return ""
    try:
        sz = log_path.stat().st_size
        if sz <= offset:
            return ""
        with open(log_path, "rb") as fp:
            fp.seek(offset)
            return fp.read().decode("utf-8", errors="replace")
    except OSError:
        return ""


def _fmt_eta(sec: float) -> str:
    s = max(0, int(round(sec)))
    if s >= 3600:
        return f"{s // 3600}h{(s % 3600) // 60:02d}m"
    if s >= 60:
        return f"{s // 60}m{s % 60:02d}s"
    return f"{s}s"


def _replace_xml_tag(xml_text: str, tag: str, new_val: str) -> str:
    pattern = re.compile(rf"(<{re.escape(tag)}>\s*)([^<]*?)(\s*</{re.escape(tag)}>)", re.DOTALL)
    return pattern.sub(lambda m: f"{m.group(1)}{new_val}{m.group(3)}", xml_text)


def _to_win_encoded_path(linux_path: Path) -> str:
    return f"Z%3a{str(linux_path).replace(os.sep, '/')}"


def _run_xdotool(display: str, *args: str) -> str:
    env = os.environ.copy()
    env["DISPLAY"] = display
    res = subprocess.run(["xdotool", *args], env=env, capture_output=True, text=True)
    return res.stdout.strip()


class SilverFastBackend(ScannerBackend):
    name = "silverfast"

    def is_available(self) -> tuple[bool, str]:
        if not SFL_WRAPPER.exists():
            return False, "sfl.sh wrapper not found"
        sf_exe = SFL_PREFIX / "drive_c" / "Program Files" / "SilverFast Application" / "SilverFast 9" / "SilverFast 9.exe"
        if not sf_exe.exists():
            return False, "SilverFast 9 not installed in Wine prefix"
        if not shutil.which("Xvfb") or not shutil.which("xdotool"):
            return False, "Xvfb or xdotool missing"
        return True, "SilverFast SE Plus 9.2.10 (Wine + USB helper)"

    def _configure_xml(
        self,
        req: ScanRequest,
        staging_dir: Path,
        stem: str,
        area_box: tuple[float, float, float, float],
    ) -> None:
        prefs_dir = _find_prefs_dir()
        if not prefs_dir:
            return
        spt = prefs_dir / "PrefsSPT(EPSON Perfection V700_V750).xml"
        if not spt.exists():
            return

        txt = spt.read_text(encoding="utf-8", errors="replace")
        y0, x0, y1, x1 = area_box
        area_str = _format_area_inches(y0, x0, y1, x1)
        dpi_str = f"{float(req.dpi):.16e}%20"
        me_str = "TRUE%20" if req.multi_exposure else "FALSE%20"
        win_dir = _to_win_encoded_path(staging_dir)

        txt = _replace_xml_tag(txt, "AreaInches", area_str)
        txt = _replace_xml_tag(txt, "Directory", win_dir)
        txt = _replace_xml_tag(txt, "OutResDPI", dpi_str)
        txt = _replace_xml_tag(txt, "PrintResDPI", dpi_str)
        txt = _replace_xml_tag(txt, "MultiExposure", me_str)
        txt = _replace_xml_tag(txt, "General_Bitdepth", "16%20")
        txt = _replace_xml_tag(txt, "General_HdrMode", "TRUE%20")

        sp_idx = txt.find("<ScanParameter>")
        if sp_idx != -1:
            head, tail = txt[:sp_idx], txt[sp_idx:]
            tail = re.sub(
                r"(<Name>\s*)([^<]+?)(\s*</Name>)",
                lambda m: f"{m.group(1)}{stem}{m.group(3)}",
                tail,
                count=1,
            )
            txt = head + tail

        spt.write_text(txt, encoding="utf-8")

    def _scan_single_tile(
        self,
        req: ScanRequest,
        tile_idx: int,
        num_tiles: int,
        tile_label: str,
        area_box: tuple[float, float, float, float],
        staging_dir: Path,
        stem: str,
        log_cb: Callable[[str, Optional[float]], None],
        cancel_event: threading.Event,
    ) -> Path:
        if staging_dir.exists():
            shutil.rmtree(staging_dir)
        staging_dir.mkdir(parents=True, exist_ok=True)

        tile_prefix = f"TILE {tile_idx + 1}/{num_tiles} ({tile_label}) · " if num_tiles > 1 else ""
        # Map this tile's 0..100% into the overall progress bar slice (leaving final 3% for stitching/move)
        prog_lo = (tile_idx / num_tiles) * 96.0
        prog_span = (1.0 / num_tiles) * 96.0

        def tile_cb(msg: str, local_pct: Optional[float]) -> None:
            overall = round(prog_lo + prog_span * (local_pct / 100.0), 1) if local_pct is not None else None
            log_cb(f"{tile_prefix}{msg}", overall)

        y0, x0, y1, x1 = area_box
        est_gib = _estimate_tiff_bytes(x1 - x0, y1 - y0, req.dpi, req.ir_enabled) / (1024**3)
        tile_cb(
            f"Configuring SilverFast ({req.dpi} DPI, ~{est_gib:.2f} GiB, ME={'ON' if req.multi_exposure else 'OFF'}, IR={'64-bit HDRi' if req.ir_enabled else '48-bit HDR'})",
            2.0,
        )
        self._configure_xml(req, staging_dir, stem, area_box)

        roam_dir = _find_sf_roaming_dir()
        sf_app_log = (roam_dir / "LogFile.txt") if roam_dir else None

        display = pick_free_display(45)
        xvfb = subprocess.Popen(
            ["Xvfb", display, "-screen", "0", "1440x900x24", "-nolisten", "tcp"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        sf_proc = None
        try:
            time.sleep(0.8)
            env = os.environ.copy()
            env["DISPLAY"] = display
            xdg = env.get("XDG_RUNTIME_DIR", "")
            if not xdg or not os.access(xdg, os.W_OK):
                fallback_run = Path("/tmp/sfl_run")
                fallback_run.mkdir(parents=True, exist_ok=True)
                env["XDG_RUNTIME_DIR"] = str(fallback_run)

            tile_cb("Launching SilverFast 9 + USB helper in Xvfb...", 3.0)
            sf_log_path = BASE_DIR / "silverfast_xvfb.log"
            sf_log = open(sf_log_path, "w")
            sf_proc = subprocess.Popen([str(SFL_WRAPPER), "run"], env=env, stdout=sf_log, stderr=subprocess.STDOUT)

            launch_log_offset = sf_app_log.stat().st_size if (sf_app_log and sf_app_log.exists()) else 0

            launcher_ready = False
            for _ in range(50):
                if cancel_event.is_set():
                    raise RuntimeError("Cancelled before launch")
                if sf_proc.poll() is not None:
                    sf_log.flush()
                    detail = sf_log_path.read_text(encoding="utf-8", errors="replace").strip()
                    detail = detail.splitlines()[-1] if detail else f"exit code {sf_proc.returncode}"
                    raise RuntimeError(f"SilverFast failed to launch: {detail}")
                if _run_xdotool(display, "search", "--onlyvisible", "--name", "SilverFast 9"):
                    launcher_ready = True
                    break
                time.sleep(0.5)
            if not launcher_ready:
                sf_log.flush()
                detail = sf_log_path.read_text(encoding="utf-8", errors="replace").strip()
                extra = f" ({detail.splitlines()[-1]})" if detail else ""
                raise RuntimeError(f"SilverFast 9 launcher window did not appear{extra}")

            web_t0 = time.time()
            while time.time() - web_t0 < 10.0:
                if cancel_event.is_set():
                    raise RuntimeError("Cancelled before launch")
                if "Web engine loading finished 1" in _read_log_tail(sf_app_log, launch_log_offset):
                    time.sleep(0.8)
                    break
                time.sleep(0.3)
            else:
                time.sleep(1.5)

            if _run_xdotool(display, "search", "--onlyvisible", "--name", "No Connection"):
                tile_cb("Dismissing offline check notice...", 4.0)
                _run_xdotool(display, "mousemove", "975", "673", "click", "1")
                time.sleep(0.8)

            tile_cb("Connecting Epson V750...", 5.0)
            _run_xdotool(display, "mousemove", "1115", "725", "click", "1")

            main_ready = False
            for attempt in range(80):
                if cancel_event.is_set():
                    raise RuntimeError("Cancelled during scanner init")
                if _run_xdotool(display, "search", "--onlyvisible", "--name", "Perfection V700"):
                    main_ready = True
                    break
                if attempt > 0 and attempt % 10 == 0:
                    if _run_xdotool(display, "search", "--onlyvisible", "--name", "No Connection"):
                        _run_xdotool(display, "mousemove", "975", "673", "click", "1")
                        time.sleep(0.6)
                    _run_xdotool(display, "mousemove", "1115", "725", "click", "1")
                time.sleep(0.5)
            if not main_ready:
                if shutil.which("import"):
                    subprocess.run(
                        ["import", "-window", "root", str(BASE_DIR / "silverfast_last_err.png")],
                        env=env,
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                    )
                raise RuntimeError("SilverFast main scanner window did not open (check V750 power/USB or MySilverFast login)")

            time.sleep(3.0)
            tile_cb("Selecting Transparency: Holder + HDR bit depth...", 6.0)
            _run_xdotool(display, "mousemove", "80", "368", "click", "1")
            time.sleep(0.5)
            _run_xdotool(display, "mousemove", "100", "444", "click", "1")
            time.sleep(1.2)

            _run_xdotool(display, "mousemove", "265", "368", "click", "1")
            time.sleep(0.5)
            y_choice = "542" if req.ir_enabled else "495"
            _run_xdotool(display, "mousemove", "270", y_choice, "click", "1")
            time.sleep(0.8)
            wins = [w for w in _run_xdotool(display, "search", "--onlyvisible", "--name", ".").splitlines() if w]
            if len(wins) > 1:
                _run_xdotool(display, "mousemove", "790", "676", "click", "1")
                time.sleep(0.5)

            log_offset = sf_app_log.stat().st_size if (sf_app_log and sf_app_log.exists()) else 0
            sti_log_path = BASE_DIR / "silverfast-data" / "logs" / "sti.log"
            sti_offset = sti_log_path.stat().st_size if sti_log_path.exists() else 0

            tile_cb(f"Starting hardware scan at {req.dpi} DPI (warming lamp / calibrating)...", 7.0)
            _run_xdotool(display, "mousemove", "956", "308", "click", "1")

            wine_pics = None
            users_dir = SFL_PREFIX / "drive_c" / "users"
            if users_dir.exists():
                for u in users_dir.iterdir():
                    cand = u / "Pictures"
                    if cand.is_dir():
                        wine_pics = cand
                        break
            before_pics = set(wine_pics.glob("*.tif")) if wine_pics else set()

            # Empirically calibrated Epson V750 per-pass durations (scaled mildly for narrower tiles)
            width_factor = 0.72 + 0.28 * ((x1 - x0) / max(1.0, HOLDER_X1 - HOLDER_X0))
            height_factor = (y1 - y0) / 9.70
            base_rgb = max(45.0, 101.0 + max(0, req.dpi - 600) * 0.3415) if req.dpi >= 600 else max(35.0, req.dpi * 0.17)
            est_rgb_sec = base_rgb * width_factor * height_factor
            est_ir_sec = est_rgb_sec * 0.62
            passes: list[tuple[str, float]] = [("RGB NORMAL", est_rgb_sec)]
            if req.multi_exposure:
                passes.append(("MULTI-EXP RGB", est_rgb_sec))
            if req.ir_enabled:
                passes.append(("INFRARED (IR)", est_ir_sec))
            total_est_sec = sum(d for _, d in passes)

            hard_timeout_sec = 14400
            t0 = time.time()
            last_file_sz = 0
            stable_since: Optional[float] = None
            saved_file: Optional[Path] = None
            last_report_t = 0.0
            active_pass_idx = -1
            pass_start_t = t0

            while time.time() - t0 < hard_timeout_sec:
                now = time.time()
                if cancel_event.is_set():
                    tile_cb("Cancelling scan cleanly (waiting for V750 carriage to park)...", None)
                    _run_xdotool(display, "mousemove", "368", "780", "click", "1")
                    for _ in range(15):
                        if "Scanthread finished" in _read_log_tail(sf_app_log, log_offset):
                            break
                        time.sleep(1.0)
                    time.sleep(1.5)
                    raise RuntimeError("Scan cancelled by user")

                if sf_proc.poll() is not None:
                    raise RuntimeError("SilverFast exited unexpectedly during scan")

                log_tail = _read_log_tail(sf_app_log, log_offset)
                sti_tail = _read_log_tail(sti_log_path, sti_offset)

                if "Scanthread finished with error" in log_tail or "Maximum TIFF file size exceeded" in log_tail:
                    err_lines = [ln.strip() for ln in log_tail.splitlines() if ("Error:" in ln or "Maximum TIFF" in ln)]
                    detail = err_lines[-1] if err_lines else "SilverFast Scanthread finished with error"
                    raise RuntimeError(detail)

                scan_thread_done = "Scanthread finished; doExit=0" in log_tail

                candidates = list(staging_dir.glob("*.tif"))
                if not candidates and wine_pics:
                    new_pics = [p for p in wine_pics.glob("*.tif") if p not in before_pics]
                    candidates.extend(new_pics)

                if candidates:
                    cand = max(candidates, key=lambda p: p.stat().st_mtime)
                    sz = cand.stat().st_size
                    if sz != last_file_sz:
                        last_file_sz = sz
                        stable_since = None
                    elif sz > 10000:
                        if stable_since is None:
                            stable_since = now
                        if scan_thread_done or (now - stable_since >= 6.0):
                            saved_file = cand
                            break

                    if now - last_report_t >= 2.0:
                        mb = sz / (1024 * 1024)
                        tile_cb(f"WRITING TIFF TO DISK ({mb:.0f} MB)...", 97.0)
                        last_report_t = now
                    time.sleep(1.0)
                    continue

                pass_starts = sti_tail.count("CreateDevice")
                if pass_starts > 0:
                    new_idx = min(len(passes) - 1, pass_starts - 1)
                    if new_idx != active_pass_idx:
                        active_pass_idx = new_idx
                        pass_start_t = now
                        last_report_t = 0.0

                if now - last_report_t >= 2.0:
                    if active_pass_idx < 0:
                        elapsed = int(now - t0)
                        tile_cb(f"CALIBRATING / WARMING LAMP ({elapsed}s)...", 7.0)
                    else:
                        pass_name, pass_est = passes[active_pass_idx]
                        pass_elapsed = now - pass_start_t
                        pass_frac = min(0.99, pass_elapsed / max(1.0, pass_est))
                        pass_pct = int(pass_frac * 100)

                        done_est = sum(d for _, d in passes[:active_pass_idx]) + min(pass_est * 0.99, pass_elapsed)
                        local_pct = min(95.0, 7.0 + 88.0 * min(1.0, done_est / max(1.0, total_est_sec)))
                        rem_sec = max(0.0, (pass_est - pass_elapsed)) + sum(d for _, d in passes[active_pass_idx + 1 :])
                        eta_str = f" · ETA ~{_fmt_eta(rem_sec)}" if rem_sec >= 5.0 else ""

                        stage_msg = f"PASS {active_pass_idx + 1}/{len(passes)} · {pass_name} ~{pass_pct}% ({_fmt_eta(pass_elapsed)}{eta_str})"
                        tile_cb(stage_msg, local_pct)
                    last_report_t = now

                time.sleep(1.0)

            if not saved_file:
                _run_xdotool(display, "mousemove", "368", "780", "click", "1")
                time.sleep(4.0)
                raise TimeoutError("SilverFast scan exceeded 4-hour safety limit")

            tile_cb("Closing SilverFast cleanly...", 99.0)
            _run_xdotool(display, "key", "alt+F4")
            time.sleep(3.0)
            return saved_file
        finally:
            if sf_proc and sf_proc.poll() is None:
                sf_proc.terminate()
                try:
                    sf_proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    sf_proc.kill()
            subprocess.run(
                ["wineserver", "-k"],
                env={**os.environ, "WINEPREFIX": str(SFL_PREFIX)},
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            # Wait briefly for silverfast-usbd to release its socket (prevents Tile 1 -> Tile 2 races)
            usbd_sock = Path(env.get("XDG_RUNTIME_DIR", "")) / "silverfast-linux" / "usbd.sock"
            for _ in range(10):
                if not usbd_sock.exists():
                    break
                time.sleep(0.3)
            xvfb.terminate()
            xvfb.wait()

    def scan(
        self,
        req: ScanRequest,
        dest_dir: Path,
        log_cb: Callable[[str, Optional[float]], None],
        cancel_event: threading.Event,
    ) -> Path:
        safe_roll = "".join(c if (c.isalnum() or c in "-_") else "_" for c in (req.roll_name or "roll")).strip("_") or "roll"
        ts = time.strftime("%Y%m%d_%H%M%S")
        stem = f"{safe_roll}_{req.dpi}dpi_{ts}"
        out_path = dest_dir / f"{stem}.tif"
        dest_dir.mkdir(parents=True, exist_ok=True)

        tiles = _plan_scan_tiles(req.dpi, req.ir_enabled)
        if len(tiles) == 1:
            label, y0, x0, y1, x1 = tiles[0]
            saved = self._scan_single_tile(
                req=req,
                tile_idx=0,
                num_tiles=1,
                tile_label=label,
                area_box=(y0, x0, y1, x1),
                staging_dir=BASE_DIR / ".sf_staging",
                stem=stem,
                log_cb=log_cb,
                cancel_event=cancel_event,
            )
            shutil.move(str(saved), str(out_path))
            mb = out_path.stat().st_size / (1024 * 1024)
            log_cb(f"Saved {out_path.name} ({mb:.1f} MB)", 100.0)
            return out_path

        # Multi-tile auto-split: keep every SilverFast output < 4.0 GiB, then stitch into one BigTIFF
        log_cb(
            f"Auto-splitting {req.dpi} DPI into {len(tiles)} tiles along holder dividers (avoids SilverFast 4 GiB TIFF limit)...",
            1.0,
        )
        tile_archive_dir = BASE_DIR / ".sf_tiles"
        if tile_archive_dir.exists():
            shutil.rmtree(tile_archive_dir)
        tile_archive_dir.mkdir(parents=True, exist_ok=True)

        tile_files: list[Path] = []
        try:
            for idx, (label, y0, x0, y1, x1) in enumerate(tiles):
                saved = self._scan_single_tile(
                    req=req,
                    tile_idx=idx,
                    num_tiles=len(tiles),
                    tile_label=label,
                    area_box=(y0, x0, y1, x1),
                    staging_dir=BASE_DIR / ".sf_staging",
                    stem=f"{stem}_tile{idx + 1}",
                    log_cb=log_cb,
                    cancel_event=cancel_event,
                )
                archived = tile_archive_dir / f"tile_{idx + 1}.tif"
                shutil.move(str(saved), str(archived))
                tile_files.append(archived)
                # Brief pause between halves so USB helper releases cleanly
                if idx + 1 < len(tiles):
                    time.sleep(2.0)

            log_cb(f"Stitching {len(tile_files)} tiles into seamless 64-bit HDRi BigTIFF...", 97.5)
            tiff_bridge.stitch_horizontal_bigtiff(tile_files, out_path)
            gb = out_path.stat().st_size / (1024**3)
            log_cb(f"Saved {out_path.name} ({gb:.2f} GiB BigTIFF)", 100.0)
            return out_path
        finally:
            if tile_archive_dir.exists():
                shutil.rmtree(tile_archive_dir, ignore_errors=True)
