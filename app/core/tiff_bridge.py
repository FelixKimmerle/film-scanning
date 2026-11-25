"""Python ctypes bridge to libfilm_core.so (native C++20 16-bit TIFF + IR engine)."""
from __future__ import annotations

import ctypes
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

import numpy as np
from PIL import Image

PROJECT_ROOT = Path(__file__).resolve().parents[2]


def _resolve_lib_path() -> Path:
    candidates = [
        PROJECT_ROOT / "build" / "release" / "libfilm_core.so",
        PROJECT_ROOT / "build" / "debug" / "libfilm_core.so",
        Path(__file__).resolve().parent / "libfilm_core.so",
    ]
    for p in candidates:
        if p.exists():
            return p
    raise FileNotFoundError(
        "libfilm_core.so not found — build it with: cd ~/Documents/dev/film_scanning && ./nobcpp release"
    )


LIB_PATH = _resolve_lib_path()


class _CTiffInfo(ctypes.Structure):
    _fields_ = [
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("dpi_x", ctypes.c_float),
        ("dpi_y", ctypes.c_float),
        ("bits_per_sample", ctypes.c_uint16),
        ("samples_per_pixel", ctypes.c_uint16),
        ("has_ir", ctypes.c_uint8),
        ("is_linear_hdr", ctypes.c_uint8),
        ("software", ctypes.c_char * 128),
    ]


class _CFrameSpec(ctypes.Structure):
    _fields_ = [
        ("cx", ctypes.c_double),
        ("cy", ctypes.c_double),
        ("width", ctypes.c_double),
        ("height", ctypes.c_double),
        ("angle_deg", ctypes.c_double),
        ("inset_pct", ctypes.c_double),
    ]


@dataclass
class ScanInfo:
    path: str
    width: int
    height: int
    dpi_x: float
    dpi_y: float
    bits_per_sample: int
    samples_per_pixel: int
    has_ir: bool
    is_linear_hdr: bool
    software: str


_lib = ctypes.CDLL(str(LIB_PATH))

_lib.fc_read_info.argtypes = [ctypes.c_char_p, ctypes.POINTER(_CTiffInfo)]
_lib.fc_read_info.restype = ctypes.c_int

_lib.fc_build_preview.argtypes = [
    ctypes.c_char_p,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_float,
    ctypes.POINTER(ctypes.c_int),
    ctypes.POINTER(ctypes.c_int),
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.POINTER(ctypes.c_float),
]
_lib.fc_build_preview.restype = ctypes.c_int

_lib.fc_read_region.argtypes = [
    ctypes.c_char_p,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.POINTER(ctypes.c_float),
    ctypes.c_float,
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.POINTER(ctypes.c_uint8),
]
_lib.fc_read_region.restype = ctypes.c_int

_lib.fc_export_frames.argtypes = [
    ctypes.c_char_p,
    ctypes.c_int,
    ctypes.POINTER(_CFrameSpec),
    ctypes.POINTER(ctypes.c_char_p),
    ctypes.POINTER(ctypes.c_char_p),
    ctypes.c_int,
    ctypes.c_float,
    ctypes.c_int,
    ctypes.c_int,
]
_lib.fc_export_frames.restype = ctypes.c_int


def read_info(path: str | Path) -> ScanInfo:
    cinfo = _CTiffInfo()
    rc = _lib.fc_read_info(str(path).encode("utf-8"), ctypes.byref(cinfo))
    if rc != 0:
        raise RuntimeError(f"Failed to read TIFF info for {path} (rc={rc})")
    return ScanInfo(
        path=str(path),
        width=int(cinfo.width),
        height=int(cinfo.height),
        dpi_x=float(cinfo.dpi_x),
        dpi_y=float(cinfo.dpi_y),
        bits_per_sample=int(cinfo.bits_per_sample),
        samples_per_pixel=int(cinfo.samples_per_pixel),
        has_ir=bool(cinfo.has_ir),
        is_linear_hdr=bool(cinfo.is_linear_hdr),
        software=cinfo.software.decode("utf-8", errors="replace").strip(),
    )


_INV_PARAMS_CACHE: dict[tuple[str, float], np.ndarray] = {}


def build_preview(
    path: str | Path,
    max_dim: int = 2000,
    invert_mode: int = 0,
    ir_sensitivity: float = 0.5,
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Returns (rgb8 [H,W,3], ir8 [H,W], dust_mask8 [H,W])."""
    p = Path(path)
    info = read_info(p)
    longest = max(info.width, info.height)
    scale = (max_dim / longest) if (max_dim > 0 and longest > max_dim) else 1.0
    pw = max(1, int(round(info.width * scale)))
    ph = max(1, int(round(info.height * scale)))

    rgb8 = np.empty((ph, pw, 3), dtype=np.uint8)
    ir8 = np.empty((ph, pw), dtype=np.uint8)
    mask8 = np.empty((ph, pw), dtype=np.uint8)
    inv_params = np.zeros(6, dtype=np.float32)

    out_w = ctypes.c_int(0)
    out_h = ctypes.c_int(0)
    rc = _lib.fc_build_preview(
        str(p).encode("utf-8"),
        ctypes.c_int(max_dim),
        ctypes.c_int(invert_mode),
        ctypes.c_float(ir_sensitivity),
        ctypes.byref(out_w),
        ctypes.byref(out_h),
        rgb8.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
        ir8.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
        mask8.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
        inv_params.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
    )
    if rc != 0:
        raise RuntimeError(f"fc_build_preview failed for {p} (rc={rc})")
    if inv_params[0] > 0.0 and inv_params[3] > 0.0:
        _INV_PARAMS_CACHE[(str(p), p.stat().st_mtime)] = inv_params
    return rgb8[: out_h.value, : out_w.value], ir8[: out_h.value, : out_w.value], mask8[: out_h.value, : out_w.value]


def read_region(
    path: str | Path,
    rx: int,
    ry: int,
    rw: int,
    rh: int,
    out_w: int,
    out_h: int,
    invert_mode: int = 0,
    want_ir_raw: bool = False,
    want_ir_mask: bool = False,
    ir_sensitivity: float = 0.5,
) -> tuple[np.ndarray, np.ndarray | None, np.ndarray | None]:
    """Fast random-access scanline crop for crisp 1:1 native zoom on huge TIFFs."""
    p = Path(path)
    mtime = p.stat().st_mtime
    inv_params = _INV_PARAMS_CACHE.get((str(p), mtime))
    if inv_params is None and invert_mode in (1, 2):
        # Compute roll-wide inversion params once via a quick small overview
        build_preview(p, max_dim=800, invert_mode=invert_mode, ir_sensitivity=0.0)
        inv_params = _INV_PARAMS_CACHE.get((str(p), mtime))

    rgb8 = np.empty((out_h, out_w, 3), dtype=np.uint8)
    ir8 = np.empty((out_h, out_w), dtype=np.uint8) if want_ir_raw else None
    mask8 = np.empty((out_h, out_w), dtype=np.uint8) if want_ir_mask else None

    params_ptr = (
        inv_params.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
        if inv_params is not None
        else ctypes.POINTER(ctypes.c_float)()
    )
    ir_ptr = ir8.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)) if ir8 is not None else ctypes.POINTER(ctypes.c_uint8)()
    mask_ptr = (
        mask8.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)) if mask8 is not None else ctypes.POINTER(ctypes.c_uint8)()
    )

    rc = _lib.fc_read_region(
        str(p).encode("utf-8"),
        ctypes.c_int(rx),
        ctypes.c_int(ry),
        ctypes.c_int(rw),
        ctypes.c_int(rh),
        ctypes.c_int(out_w),
        ctypes.c_int(out_h),
        ctypes.c_int(invert_mode),
        params_ptr,
        ctypes.c_float(ir_sensitivity if want_ir_mask else 0.0),
        rgb8.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
        ir_ptr,
        mask_ptr,
    )
    if rc != 0:
        raise RuntimeError(f"fc_read_region failed for {p} (rc={rc})")
    return rgb8, ir8, mask8


def export_frames(
    src_path: str | Path,
    frames: Sequence[dict],
    out_dir: str | Path,
    roll_name: str,
    apply_ir_clean: bool = True,
    ir_sensitivity: float = 0.5,
    gamma_22: bool = False,
    export_positive_proof: bool = False,
    positive_mode: int = 1,
) -> list[dict]:
    """Exports cropped 16-bit TIFF frames (and optional JPEG positive proofs)."""
    if not frames:
        return []
    out_path = Path(out_dir).expanduser().resolve()
    out_path.mkdir(parents=True, exist_ok=True)
    proof_dir = out_path / "proofs_jpg"
    if export_positive_proof:
        proof_dir.mkdir(parents=True, exist_ok=True)

    safe_roll = "".join(c if (c.isalnum() or c in "-_") else "_" for c in (roll_name or "roll")).strip("_") or "roll"

    n = len(frames)
    c_frames = (_CFrameSpec * n)()
    c_tiffs = (ctypes.c_char_p * n)()
    c_ppms = (ctypes.c_char_p * n)()

    tiff_paths: list[Path] = []
    ppm_paths: list[Path | None] = []

    for i, f in enumerate(frames):
        idx = i + 1
        tpath = out_path / f"{safe_roll}_{idx:03d}.tif"
        tiff_paths.append(tpath)
        c_tiffs[i] = str(tpath).encode("utf-8")

        if export_positive_proof:
            ppath = proof_dir / f".tmp_{safe_roll}_{idx:03d}.ppm"
            ppm_paths.append(ppath)
            c_ppms[i] = str(ppath).encode("utf-8")
        else:
            ppm_paths.append(None)
            c_ppms[i] = b""

        rot90 = int(f.get("rot90", 0)) % 360
        fw = float(f["width"])
        fh = float(f["height"])
        ang = float(f.get("angle_deg", 0.0))
        if rot90 == 90:
            fw, fh, ang = fh, fw, ang - 90.0
        elif rot90 == 180:
            ang = ang + 180.0
        elif rot90 == 270:
            fw, fh, ang = fh, fw, ang + 90.0

        c_frames[i] = _CFrameSpec(
            cx=float(f["cx"]),
            cy=float(f["cy"]),
            width=fw,
            height=fh,
            angle_deg=ang,
            inset_pct=float(f.get("inset_pct", 0.0)),
        )

    rc = _lib.fc_export_frames(
        str(src_path).encode("utf-8"),
        ctypes.c_int(n),
        c_frames,
        c_tiffs,
        c_ppms,
        ctypes.c_int(1 if apply_ir_clean else 0),
        ctypes.c_float(ir_sensitivity),
        ctypes.c_int(1 if gamma_22 else 0),
        ctypes.c_int(positive_mode if export_positive_proof else 0),
    )
    if rc != 0:
        raise RuntimeError(f"fc_export_frames failed (rc={rc})")

    results = []
    for i, tpath in enumerate(tiff_paths):
        proof_jpg = None
        ppath = ppm_paths[i]
        if ppath and ppath.exists():
            jpg_path = proof_dir / f"{safe_roll}_{i + 1:03d}.jpg"
            with Image.open(ppath) as pim:
                pim.save(jpg_path, format="JPEG", quality=92)
            os.remove(ppath)
            proof_jpg = str(jpg_path)
        results.append(
            {
                "index": i + 1,
                "tiff_path": str(tpath),
                "size_bytes": tpath.stat().st_size if tpath.exists() else 0,
                "proof_jpg": proof_jpg,
            }
        )
    return results
