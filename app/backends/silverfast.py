"""Headless SilverFast SE Plus 9 backend (Wine + silverfast-linux + Xvfb)."""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import threading
import time
from pathlib import Path
from typing import Callable, Optional

from .base import ScannerBackend, ScanRequest, pick_free_display

BASE_DIR = Path(__file__).resolve().parents[2]
SFL_WRAPPER = BASE_DIR / "sfl.sh"
SFL_PREFIX = BASE_DIR / "silverfast-data" / "prefix"


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


def _find_usbd_pid() -> Optional[int]:
    """Locate the running silverfast-usbd helper PID via /proc."""
    proc = Path("/proc")
    for entry in proc.iterdir():
        if not entry.name.isdigit():
            continue
        try:
            comm = (entry / "comm").read_text(encoding="utf-8", errors="ignore").strip()
            if comm == "silverfast-usbd":
                return int(entry.name)
        except OSError:
            continue
    return None


def _read_proc_wchar(pid: int) -> Optional[int]:
    """Read cumulative bytes written (USB bulk-in -> Wine socket) from /proc/<pid>/io."""
    try:
        for line in Path(f"/proc/{pid}/io").read_text(encoding="utf-8", errors="ignore").splitlines():
            if line.startswith("wchar:"):
                return int(line.split(":", 1)[1].strip())
    except (OSError, ValueError):
        return None
    return None


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


def _parse_scan_geometry(log_tail: str, dpi: int) -> tuple[int, int, int, int]:
    """Extract (rgb_w, rgb_h, ir_w, ir_h) from SilverFast LogFile.txt, with DPI fallback."""
    nom_w = int(round(5.90 * dpi))
    nom_h = int(round(9.70 * dpi))
    rgb_w, rgb_h = nom_w, nom_h
    ir_w, ir_h = nom_w, nom_h

    m_norm = re.search(r"Wanted size of normal exposure-scan in pixels:\s*(\d+)/(\d+)", log_tail)
    if m_norm:
        rgb_w, rgb_h = int(m_norm.group(1)), int(m_norm.group(2))
    m_ir = re.search(r"Wanted size of infrared-scan in pixels:\s*(\d+)/(\d+)", log_tail)
    if m_ir:
        ir_w, ir_h = int(m_ir.group(1)), int(m_ir.group(2))
    return rgb_w, rgb_h, ir_w, ir_h


def _fmt_eta(sec: float) -> str:
    s = max(0, int(round(sec)))
    if s >= 3600:
        return f"{s // 3600}h{(s % 3600) // 60:02d}m"
    if s >= 60:
        return f"{s // 60}m{s % 60:02d}s"
    return f"{s}s"


def _replace_xml_tag(xml_text: str, tag: str, new_val: str) -> str:
    # Matches SilverFast's multi-line tag style: <Tag>\r\r\n  VALUE\r\r\n</Tag>
    pattern = re.compile(rf"(<{re.escape(tag)}>\s*)([^<]*?)(\s*</{re.escape(tag)}>)", re.DOTALL)
    return pattern.sub(lambda m: f"{m.group(1)}{new_val}{m.group(3)}", xml_text)


def _to_win_encoded_path(linux_path: Path) -> str:
    # SilverFast XML encodes ':' as '%3a' and uses forward slashes after Z:
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

    def _configure_xml(self, req: ScanRequest, staging_dir: Path, stem: str) -> None:
        prefs_dir = _find_prefs_dir()
        if not prefs_dir:
            return
        spt = prefs_dir / "PrefsSPT(EPSON Perfection V700_V750).xml"
        if not spt.exists():
            return

        txt = spt.read_text(encoding="utf-8", errors="replace")
        # Full Epson V700/V750 35mm holder bed: 0.0 -> 9.70" tall, 0.0 -> 5.90" wide
        full_area = "0.0000000000000000e+00%200.0000000000000000e+00%209.6999999999999993e+00%205.9000000000000004e+00%200.0000000000000000e+00%20"
        dpi_str = f"{float(req.dpi):.16e}%20"
        me_str = "TRUE%20" if req.multi_exposure else "FALSE%20"
        win_dir = _to_win_encoded_path(staging_dir)

        txt = _replace_xml_tag(txt, "AreaInches", full_area)
        txt = _replace_xml_tag(txt, "Directory", win_dir)
        txt = _replace_xml_tag(txt, "OutResDPI", dpi_str)
        txt = _replace_xml_tag(txt, "PrintResDPI", dpi_str)
        txt = _replace_xml_tag(txt, "MultiExposure", me_str)
        txt = _replace_xml_tag(txt, "General_Bitdepth", "16%20")
        txt = _replace_xml_tag(txt, "General_HdrMode", "TRUE%20")

        # Update <ScanParameter>...<Name>Untitled</Name> without touching dialog names
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

    def scan(
        self,
        req: ScanRequest,
        dest_dir: Path,
        log_cb: Callable[[str, Optional[float]], None],
        cancel_event: threading.Event,
    ) -> Path:
        staging_dir = BASE_DIR / ".sf_staging"
        if staging_dir.exists():
            shutil.rmtree(staging_dir)
        staging_dir.mkdir(parents=True, exist_ok=True)

        safe_roll = "".join(c if (c.isalnum() or c in "-_") else "_" for c in (req.roll_name or "roll")).strip("_") or "roll"
        ts = time.strftime("%Y%m%d_%H%M%S")
        stem = f"{safe_roll}_{req.dpi}dpi_{ts}"
        out_path = dest_dir / f"{stem}.tif"

        log_cb(f"Configuring SilverFast XML ({req.dpi} DPI, ME={'ON' if req.multi_exposure else 'OFF'}, IR={'64-bit HDRi' if req.ir_enabled else '48-bit HDR'})", 1.0)
        self._configure_xml(req, staging_dir, stem)

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

            log_cb("Launching SilverFast 9 + USB helper in Xvfb...", 2.0)
            sf_log_path = BASE_DIR / "silverfast_xvfb.log"
            sf_log = open(sf_log_path, "w")
            sf_proc = subprocess.Popen([str(SFL_WRAPPER), "run"], env=env, stdout=sf_log, stderr=subprocess.STDOUT)

            # Snapshot LogFile before launcher so we can watch WebEngine readiness
            launch_log_offset = sf_app_log.stat().st_size if (sf_app_log and sf_app_log.exists()) else 0

            # Wait for SilverFast 9 launcher window
            launcher_ready = False
            for _ in range(50):
                if cancel_event.is_set():
                    raise RuntimeError("Cancelled before launch")
                if _run_xdotool(display, "search", "--onlyvisible", "--name", "SilverFast 9"):
                    launcher_ready = True
                    break
                time.sleep(0.5)
            if not launcher_ready:
                raise RuntimeError("SilverFast 9 launcher window did not appear")

            # Wait for SilverFast's QtWebEngine launcher UI to finish rendering START >
            web_t0 = time.time()
            while time.time() - web_t0 < 10.0:
                if cancel_event.is_set():
                    raise RuntimeError("Cancelled before launch")
                ltail = _read_log_tail(sf_app_log, launch_log_offset)
                if "Web engine loading finished 1" in ltail:
                    time.sleep(0.8)
                    break
                time.sleep(0.3)
            else:
                time.sleep(1.5)

            if _run_xdotool(display, "search", "--onlyvisible", "--name", "No Connection"):
                log_cb("Dismissing offline check notice...", 3.0)
                _run_xdotool(display, "mousemove", "975", "673", "click", "1")
                time.sleep(0.8)

            log_cb("Connecting Epson V750...", 4.0)
            _run_xdotool(display, "mousemove", "1115", "725", "click", "1")

            main_ready = False
            for attempt in range(80):
                if cancel_event.is_set():
                    raise RuntimeError("Cancelled during scanner init")
                if _run_xdotool(display, "search", "--onlyvisible", "--name", "Perfection V700"):
                    main_ready = True
                    break
                # Retry clicking START > (and dismissing offline dialog) every 5s in case UI was still painting
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
                raise RuntimeError("SilverFast main scanner window did not open (check MySilverFast login / V750 USB)")

            time.sleep(3.0)
            log_cb("Selecting Transparency: Holder + HDR bit depth...", 5.0)
            _run_xdotool(display, "mousemove", "80", "368", "click", "1")
            time.sleep(0.5)
            _run_xdotool(display, "mousemove", "100", "444", "click", "1")
            time.sleep(1.2)

            # 64 Bit HDRi RAW (542) or 48 Bit HDR RAW (495)
            _run_xdotool(display, "mousemove", "265", "368", "click", "1")
            time.sleep(0.5)
            y_choice = "542" if req.ir_enabled else "495"
            _run_xdotool(display, "mousemove", "270", y_choice, "click", "1")
            time.sleep(0.8)
            wins = [w for w in _run_xdotool(display, "search", "--onlyvisible", "--name", ".").splitlines() if w]
            if len(wins) > 1:
                _run_xdotool(display, "mousemove", "790", "676", "click", "1")
                time.sleep(0.5)

            # Snapshot LogFile & sti.log offsets right before triggering scan
            log_offset = sf_app_log.stat().st_size if (sf_app_log and sf_app_log.exists()) else 0
            sti_log_path = BASE_DIR / "silverfast-data" / "logs" / "sti.log"
            sti_offset = sti_log_path.stat().st_size if sti_log_path.exists() else 0

            # Click Scan button (956, 308)
            log_cb(f"Starting hardware scan at {req.dpi} DPI (warming lamp / calibrating)...", 5.0)
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

            # Build exact pass schedule & calibrated Epson V750 per-pass durations
            # (empirically measured on V750: 600 DPI RGB=101s/IR=65s; 3200 DPI RGB=989s/IR~610s)
            est_rgb_sec = max(45.0, 101.0 + max(0, req.dpi - 600) * 0.3415) if req.dpi >= 600 else max(35.0, req.dpi * 0.17)
            est_ir_sec = est_rgb_sec * 0.62
            passes: list[tuple[str, float]] = [("RGB NORMAL", est_rgb_sec)]
            if req.multi_exposure:
                passes.append(("MULTI-EXP RGB", est_rgb_sec))
            if req.ir_enabled:
                passes.append(("INFRARED (IR)", est_ir_sec))
            total_est_sec = sum(d for _, d in passes)

            # 4-hour hard safety cap (no false stall aborts while scanner is working)
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
                    log_cb("Cancelling scan cleanly (waiting for V750 carriage to park)...", None)
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

                # Fail fast if SilverFast logs a fatal scan error
                if "Scanthread finished with error" in log_tail:
                    err_lines = [ln.strip() for ln in log_tail.splitlines() if "Error:" in ln]
                    detail = err_lines[-1] if err_lines else "SilverFast Scanthread finished with error"
                    raise RuntimeError(detail)

                scan_thread_done = "Scanthread finished; doExit=0" in log_tail

                # Check output TIFF file in staging or Wine Pictures
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
                        # Wait until SilverFast confirms Scanthread finished OR file size stable for 6s
                        if scan_thread_done or (now - stable_since >= 6.0):
                            saved_file = cand
                            break

                    if now - last_report_t >= 2.0:
                        mb = sz / (1024 * 1024)
                        log_cb(f"WRITING TIFF TO DISK ({mb:.0f} MB)...", 97.0)
                        last_report_t = now
                    time.sleep(1.0)
                    continue

                # Exact hardware pass tracking via sti.log CreateDevice calls on scan start
                pass_starts = sti_tail.count("CreateDevice")
                if pass_starts > 0:
                    new_idx = min(len(passes) - 1, pass_starts - 1)
                    if new_idx != active_pass_idx:
                        active_pass_idx = new_idx
                        pass_start_t = now
                        last_report_t = 0.0  # Report immediately on pass transition

                if now - last_report_t >= 2.0:
                    if active_pass_idx < 0:
                        elapsed = int(now - t0)
                        log_cb(f"CALIBRATING / WARMING LAMP ({elapsed}s)...", 5.0)
                    else:
                        pass_name, pass_est = passes[active_pass_idx]
                        pass_elapsed = now - pass_start_t
                        pass_frac = min(0.99, pass_elapsed / max(1.0, pass_est))
                        pass_pct = int(pass_frac * 100)

                        done_est = sum(d for _, d in passes[:active_pass_idx]) + min(pass_est * 0.99, pass_elapsed)
                        overall_pct = min(95.0, 5.0 + 90.0 * min(1.0, done_est / max(1.0, total_est_sec)))
                        rem_sec = max(0.0, (pass_est - pass_elapsed)) + sum(d for _, d in passes[active_pass_idx + 1 :])
                        eta_str = f" · ETA ~{_fmt_eta(rem_sec)}" if rem_sec >= 5.0 else ""

                        stage_msg = f"PASS {active_pass_idx + 1}/{len(passes)} · {pass_name} ~{pass_pct}% ({_fmt_eta(pass_elapsed)}{eta_str})"
                        log_cb(stage_msg, round(overall_pct, 1))
                    last_report_t = now

                time.sleep(1.0)

            if not saved_file:
                _run_xdotool(display, "mousemove", "368", "780", "click", "1")
                time.sleep(4.0)
                raise TimeoutError("SilverFast scan exceeded 4-hour safety limit")

            log_cb("Closing SilverFast cleanly and moving scan to library...", 98.0)
            _run_xdotool(display, "key", "alt+F4")
            time.sleep(3.0)

            dest_dir.mkdir(parents=True, exist_ok=True)
            shutil.move(str(saved_file), str(out_path))
            mb = out_path.stat().st_size / (1024 * 1024)
            log_cb(f"Saved {out_path.name} ({mb:.1f} MB)", 100.0)
            return out_path
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
            xvfb.terminate()
            xvfb.wait()
