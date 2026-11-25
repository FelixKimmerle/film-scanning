#!/usr/bin/env python3
"""Minimal CLI trigger for either SilverFast 9 or VueScan backend."""
import argparse
import sys
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from app.backends.base import ScanRequest
from app.backends.silverfast import SilverFastBackend
from app.backends.vuescan import VueScanBackend


def main() -> None:
    ap = argparse.ArgumentParser(description="Headless V750 film scan CLI")
    ap.add_argument("--backend", choices=["silverfast", "vuescan"], default="silverfast")
    ap.add_argument("--roll", default="roll01")
    ap.add_argument("--dpi", type=int, default=600)
    ap.add_argument("--format", dest="film_format", choices=["half_frame", "full_frame"], default="half_frame")
    ap.add_argument("--film-type", choices=["color_neg", "bw_neg", "slide"], default="color_neg")
    ap.add_argument("--multi-exposure", action="store_true")
    ap.add_argument("--no-ir", action="store_true")
    args = ap.parse_args()

    req = ScanRequest(
        backend=args.backend,
        roll_name=args.roll,
        dpi=args.dpi,
        film_format=args.film_format,
        film_type=args.film_type,
        multi_exposure=args.multi_exposure,
        ir_enabled=not args.no_ir,
    )
    backend = SilverFastBackend() if args.backend == "silverfast" else VueScanBackend()
    dest = Path(__file__).resolve().parent / "scans"

    def log_cb(msg: str, prog: float | None) -> None:
        tag = f"{prog:5.1f}%" if prog is not None else "  --- "
        print(f"[{tag}] {msg}", flush=True)

    out = backend.scan(req, dest, log_cb, threading.Event())
    print("DONE:", out, flush=True)


if __name__ == "__main__":
    main()
