"""Base types and hardware checks for scanner backends."""
from __future__ import annotations

import os
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional


@dataclass
class ScanRequest:
    backend: str = "silverfast"       # "silverfast" | "vuescan"
    roll_name: str = "roll"
    dpi: int = 2400
    film_format: str = "half_frame"   # "half_frame" | "full_frame"
    film_type: str = "color_neg"      # "color_neg" | "bw_neg" | "slide"
    multi_exposure: bool = False
    ir_enabled: bool = True
    ir_level: str = "light"           # VueScan: none|light|medium|heavy
    dng_output: bool = False          # VueScan optional DNG


@dataclass
class JobState:
    job_id: str
    status: str = "idle"              # idle | running | done | error | cancelled
    backend: str = ""
    stage: str = ""
    progress: float = 0.0             # 0..100 (or indeterminate)
    started_at: float = 0.0
    finished_at: float = 0.0
    output_file: Optional[str] = None
    error: Optional[str] = None
    logs: list[str] = field(default_factory=list)

    def to_dict(self) -> dict:
        elapsed = 0.0
        if self.started_at > 0:
            end = self.finished_at if self.finished_at > 0 else time.time()
            elapsed = round(end - self.started_at, 1)
        return {
            "job_id": self.job_id,
            "status": self.status,
            "backend": self.backend,
            "stage": self.stage,
            "progress": round(self.progress, 1),
            "elapsed_sec": elapsed,
            "output_file": self.output_file,
            "error": self.error,
            "logs": self.logs[-40:],
        }


def detect_usb_scanner() -> dict:
    """Check sysfs for connected Epson Perfection V700/V750 (04b8:012c)."""
    sys_usb = Path("/sys/bus/usb/devices")
    if sys_usb.exists():
        for dev in sys_usb.iterdir():
            vid_f = dev / "idVendor"
            pid_f = dev / "idProduct"
            if vid_f.exists() and pid_f.exists():
                try:
                    vid = vid_f.read_text().strip().lower()
                    pid = pid_f.read_text().strip().lower()
                except OSError:
                    continue
                if vid == "04b8" and pid == "012c":
                    bus = (dev / "busnum").read_text().strip() if (dev / "busnum").exists() else "?"
                    addr = (dev / "devnum").read_text().strip() if (dev / "devnum").exists() else "?"
                    node = f"/dev/bus/usb/{int(bus):03d}/{int(addr):03d}" if bus.isdigit() and addr.isdigit() else ""
                    accessible = bool(node and os.access(node, os.R_OK | os.W_OK))
                    return {
                        "connected": True,
                        "accessible": accessible,
                        "model": "EPSON Perfection V700/V750",
                        "usb_id": "04b8:012c",
                        "node": node,
                    }
    return {
        "connected": False,
        "accessible": False,
        "model": "EPSON Perfection V700/V750",
        "usb_id": "04b8:012c",
        "node": "",
    }


def pick_free_display(start: int = 45) -> str:
    for num in range(start, 140):
        if not Path(f"/tmp/.X{num}-lock").exists():
            return f":{num}"
    raise RuntimeError("No free Xvfb display available")


class ScannerBackend:
    name: str = "base"

    def is_available(self) -> tuple[bool, str]:
        raise NotImplementedError

    def scan(
        self,
        req: ScanRequest,
        dest_dir: Path,
        log_cb: Callable[[str, Optional[float]], None],
        cancel_event: threading.Event,
    ) -> Path:
        raise NotImplementedError
