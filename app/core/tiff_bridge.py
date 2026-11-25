"""Python ctypes bridge to libfilm_core.so (native C++20 16-bit TIFF + IR engine)."""
from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import ctypes
import json
import os
import shutil
import time
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


class _CDevelopParams(ctypes.Structure):
    _fields_ = [
        ("enabled", ctypes.c_int),
        ("profile", ctypes.c_int),
        ("base_r", ctypes.c_float),
        ("base_g", ctypes.c_float),
        ("base_b", ctypes.c_float),
        ("exposure", ctypes.c_float),
        ("contrast", ctypes.c_float),
        ("highlights", ctypes.c_float),
        ("blacks", ctypes.c_float),
        ("temp", ctypes.c_float),
        ("tint", ctypes.c_float),
        ("saturation", ctypes.c_float),
        ("chroma_nr", ctypes.c_float),
        ("luma_nr", ctypes.c_float),
        ("sharpen", ctypes.c_float),
        ("auto_wb", ctypes.c_int),
        ("ir_sensitivity", ctypes.c_float),
        ("ir_dilate", ctypes.c_int),
        ("ir_auto_shift", ctypes.c_int),
        ("ir_shift_x", ctypes.c_int),
        ("ir_shift_y", ctypes.c_int),
        ("ir_show_mask", ctypes.c_int),
    ]


class _CIrSpot(ctypes.Structure):
    _fields_ = [
        ("u", ctypes.c_float),
        ("v", ctypes.c_float),
        ("r", ctypes.c_float),
        ("erase", ctypes.c_int),
    ]


PROFILE_MAP = {
    "natural": 0,
    "frontier": 1,
    "noritsu": 2,
    "warm": 3,
    "bw_classic": 4,
}


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

_lib.fc_stitch_horizontal_bigtiff.argtypes = [
    ctypes.c_int,
    ctypes.POINTER(ctypes.c_char_p),
    ctypes.c_char_p,
]
_lib.fc_stitch_horizontal_bigtiff.restype = ctypes.c_int

_lib.fc_sample_film_base.argtypes = [
    ctypes.c_char_p,
    ctypes.c_int,
    ctypes.POINTER(ctypes.c_int),
    ctypes.POINTER(ctypes.c_float),
]
_lib.fc_sample_film_base.restype = ctypes.c_int

_lib.fc_develop_frame_preview.argtypes = [
    ctypes.c_char_p,
    ctypes.POINTER(_CFrameSpec),
    ctypes.POINTER(_CDevelopParams),
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_float,
    ctypes.c_int,
    ctypes.POINTER(ctypes.c_int),
    ctypes.POINTER(ctypes.c_int),
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.POINTER(ctypes.c_int),
    ctypes.POINTER(_CIrSpot),
    ctypes.c_int,
]
_lib.fc_develop_frame_preview.restype = ctypes.c_int

_lib.fc_export_frames_v2.argtypes = [
    ctypes.c_char_p,
    ctypes.c_int,
    ctypes.POINTER(_CFrameSpec),
    ctypes.POINTER(_CDevelopParams),
    ctypes.POINTER(ctypes.c_char_p),
    ctypes.POINTER(ctypes.c_char_p),
    ctypes.POINTER(ctypes.c_char_p),
    ctypes.c_int,
    ctypes.c_float,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.POINTER(_CIrSpot),
    ctypes.POINTER(ctypes.c_int),
    ctypes.POINTER(ctypes.c_int),
]
_lib.fc_export_frames_v2.restype = ctypes.c_int


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


def stitch_horizontal_bigtiff(tile_paths: list[str | Path], out_path: str | Path) -> Path:
    """Stitch horizontal RGB16(+IR16) TIFF tiles side-by-side into one 64-bit BigTIFF."""
    if not tile_paths:
        raise ValueError("tile_paths must not be empty")
    n = len(tile_paths)
    c_tiles = (ctypes.c_char_p * n)(*[str(p).encode("utf-8") for p in tile_paths])
    out_p = Path(out_path)
    out_p.parent.mkdir(parents=True, exist_ok=True)
    rc = _lib.fc_stitch_horizontal_bigtiff(ctypes.c_int(n), c_tiles, str(out_p).encode("utf-8"))
    if rc != 0:
        raise RuntimeError(f"fc_stitch_horizontal_bigtiff failed (rc={rc})")
    return out_p


def _frame_to_cspec(f: dict) -> _CFrameSpec:
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
    return _CFrameSpec(
        cx=float(f["cx"]),
        cy=float(f["cy"]),
        width=fw,
        height=fh,
        angle_deg=ang,
        inset_pct=float(f.get("inset_pct", 0.0)),
    )


def _dev_to_cparams(
    dev: dict | None,
    film_base: tuple[float, float, float] | None = None,
    show_ir_mask: bool = False,
) -> _CDevelopParams:
    d = dev or {}
    prof_raw = d.get("profile", "frontier")
    prof_id = PROFILE_MAP.get(str(prof_raw).lower(), int(prof_raw) if isinstance(prof_raw, int) else 1)
    br = float(d.get("base_r", film_base[0] if film_base else 0.0))
    bg = float(d.get("base_g", film_base[1] if film_base else 0.0))
    bb = float(d.get("base_b", film_base[2] if film_base else 0.0))
    ir_sens_raw = d.get("ir_sensitivity", -1.0)
    ir_sens = -1.0 if ir_sens_raw is None else float(ir_sens_raw)
    ir_dil_raw = d.get("ir_dilate", -1)
    ir_dil = -1 if ir_dil_raw is None else int(ir_dil_raw)
    return _CDevelopParams(
        enabled=1 if d.get("enabled", True) else 0,
        profile=prof_id,
        base_r=br,
        base_g=bg,
        base_b=bb,
        exposure=float(d.get("exposure", 0.0)),
        contrast=float(d.get("contrast", 1.0)),
        highlights=float(d.get("highlights", 0.0)),
        blacks=float(d.get("blacks", 0.0)),
        temp=float(d.get("temp", 0.0)),
        tint=float(d.get("tint", 0.0)),
        saturation=float(d.get("saturation", 1.0)),
        chroma_nr=float(d.get("chroma_nr", 0.55)),
        luma_nr=float(d.get("luma_nr", 0.15)),
        sharpen=float(d.get("sharpen", 0.25)),
        auto_wb=1 if d.get("auto_wb", True) else 0,
        ir_sensitivity=ir_sens,
        ir_dilate=ir_dil,
        ir_auto_shift=1 if d.get("ir_auto_shift", True) else 0,
        ir_shift_x=int(d.get("ir_shift_x", 0)),
        ir_shift_y=int(d.get("ir_shift_y", 0)),
        ir_show_mask=1 if (show_ir_mask or d.get("ir_show_mask", False)) else 0,
    )


def sample_film_base_from_frames(src_path: str | Path, frames: list[dict]) -> tuple[float, float, float] | None:
    """Sample true unexposed orange film base (D-min) directly from inter-frame gaps."""
    if len(frames) < 2:
        return None
    by_strip: dict[int, list[dict]] = {}
    for f in frames:
        by_strip.setdefault(int(f.get("strip_index", 0)), []).append(f)

    boxes: list[int] = []
    for s_frames in by_strip.values():
        s_frames.sort(key=lambda x: float(x["cy"]))
        for a, b in zip(s_frames, s_frames[1:]):
            y_top = float(a["cy"]) + float(a["height"]) * 0.5
            y_bot = float(b["cy"]) - float(b["height"]) * 0.5
            gap = y_bot - y_top
            if gap < 6.0 or gap > float(a["height"]) * 0.35:
                continue
            mid_y = 0.5 * (y_top + y_bot)
            gh = max(2, int(round(gap * 0.45)))
            gw = max(12, int(round(min(float(a["width"]), float(b["width"])) * 0.55)))
            gx = int(round(0.5 * (float(a["cx"]) + float(b["cx"])) - gw * 0.5))
            gy = int(round(mid_y - gh * 0.5))
            boxes.extend([gx, gy, gw, gh])

    if not boxes:
        return None
    n_boxes = len(boxes) // 4
    c_boxes = (ctypes.c_int * len(boxes))(*boxes)
    out_rgb = (ctypes.c_float * 3)()
    rc = _lib.fc_sample_film_base(str(src_path).encode("utf-8"), ctypes.c_int(n_boxes), c_boxes, out_rgb)
    if rc != 0:
        return None
    return (float(out_rgb[0]), float(out_rgb[1]), float(out_rgb[2]))


def develop_frame_preview(
    src_path: str | Path,
    frame: dict,
    dev: dict | None = None,
    film_base: tuple[float, float, float] | None = None,
    max_dim: int = 1100,
    apply_ir_clean: bool = True,
    ir_sensitivity: float = 0.5,
    film_type: int = 1,
    show_ir_mask: bool = False,
    return_stats: bool = False,
):
    """Fast random-access live preview of one developed frame (returns RGB8 ndarray, or (rgb8, stats))."""
    cspec = _frame_to_cspec(frame)
    cdev = _dev_to_cparams(dev, film_base, show_ir_mask=show_ir_mask)
    alloc_w = max_dim if max_dim > 0 else int(max(cspec.width, cspec.height) + 16)
    buf = np.empty((alloc_w * alloc_w * 3,), dtype=np.uint8)
    ow = ctypes.c_int(0)
    oh = ctypes.c_int(0)
    ir_stats = (ctypes.c_int * 4)()
    raw_spots = frame.get("ir_spots") or []
    n_spots = len(raw_spots)
    if n_spots > 0:
        c_spots = (_CIrSpot * n_spots)(
            *[
                _CIrSpot(
                    u=float(sp.get("u", 0.0)),
                    v=float(sp.get("v", 0.0)),
                    r=float(sp.get("r", 0.005)),
                    erase=1 if sp.get("erase") else 0,
                )
                for sp in raw_spots
            ]
        )
        spots_ptr = c_spots
    else:
        spots_ptr = ctypes.POINTER(_CIrSpot)()

    rc = _lib.fc_develop_frame_preview(
        str(src_path).encode("utf-8"),
        ctypes.byref(cspec),
        ctypes.byref(cdev),
        ctypes.c_int(max_dim),
        ctypes.c_int(1 if apply_ir_clean else 0),
        ctypes.c_float(ir_sensitivity),
        ctypes.c_int(film_type),
        ctypes.byref(ow),
        ctypes.byref(oh),
        buf.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
        ir_stats,
        spots_ptr,
        ctypes.c_int(n_spots),
    )
    if rc != 0:
        raise RuntimeError(f"fc_develop_frame_preview failed (rc={rc})")
    w, h = int(ow.value), int(oh.value)
    rgb = buf[: w * h * 3].reshape((h, w, 3)).copy()
    if return_stats:
        return rgb, {
            "shift_x": int(ir_stats[0]),
            "shift_y": int(ir_stats[1]),
            "dust_px": int(ir_stats[2]),
            "auto_dilate": int(ir_stats[3]),
        }
    return rgb


def _sanitize_token(raw: str, default: str = "") -> str:
    cleaned = "".join(c if (c.isalnum() or c in "-_") else "_" for c in (raw or "").strip())
    while "__" in cleaned:
        cleaned = cleaned.replace("__", "_")
    cleaned = cleaned.strip("_")
    return cleaned or default


def parse_event_slugs(raw: str | None) -> list[str]:
    """Parse comma-separated event/carousel slugs, preserving safe optional nested subpaths."""
    if not raw:
        return []
    out: list[str] = []
    seen: set[str] = set()
    for part in str(raw).split(","):
        segs = []
        for seg in part.replace("\\", "/").split("/"):
            tok = _sanitize_token(seg, "")
            if tok and tok not in (".", ".."):
                segs.append(tok)
        if not segs:
            continue
        slug = "/".join(segs)
        if slug not in seen:
            seen.add(slug)
            out.append(slug)
    return out


def _build_exif_bytes(
    camera: str,
    film_stock: str,
    roll_name: str = "",
    frame_num: int = 0,
    events: Sequence[str] = (),
) -> bytes | None:
    exif = Image.Exif()
    if camera:
        exif[0x0110] = camera  # Model
    exif[0x0131] = "Film Cockpit V2 (Epson Perfection V750 Pro)"  # Software

    desc_parts = []
    if roll_name:
        desc_parts.append(f"Roll: {roll_name}")
    if frame_num > 0:
        desc_parts.append(f"Frame: {frame_num:03d}")
    if events:
        desc_parts.append(f"Event: {', '.join(events)}")
    if film_stock:
        desc_parts.append(f"Film: {film_stock}")
    desc_parts.append("Scanner: Epson Perfection V750 Pro")
    exif[0x010E] = " | ".join(desc_parts)  # ImageDescription

    kw_parts = []
    if roll_name:
        kw_parts.append(f"roll:{roll_name}")
    for ev in events:
        kw_parts.append(f"event:{ev}")
    if film_stock:
        kw_parts.append(f"film:{film_stock}")
    if camera:
        kw_parts.append(f"camera:{camera}")
    if kw_parts:
        exif[0x9C9E] = (";".join(kw_parts) + "\x00").encode("utf-16le")  # XPKeywords (Lightroom/Darktable/Windows)

    return exif.tobytes()


def _unlink_if_exists(p: Path) -> None:
    if p.is_symlink() or p.exists():
        p.unlink()


def _prune_empty_parents(directory: Path, stop_at: Path) -> None:
    cur = directory
    stop_resolved = stop_at.resolve()
    while cur.resolve() != stop_resolved:
        try:
            cur.rmdir()
        except OSError:
            break
        cur = cur.parent


def _populate_event_entry(src_file: Path, events_root: Path, slug: str, link_mode: str) -> Path:
    ev_dir = events_root / slug
    ev_dir.mkdir(parents=True, exist_ok=True)
    dst = ev_dir / src_file.name
    _unlink_if_exists(dst)
    if link_mode == "copy":
        shutil.copy2(src_file, dst)
    else:
        rel_target = os.path.relpath(src_file, ev_dir)
        os.symlink(rel_target, dst)
    return dst


def export_frames_v2(
    src_path: str | Path,
    frames: list[dict],
    out_dir: str | Path,
    roll_name: str,
    *,
    film_base: tuple[float, float, float] | None = None,
    apply_ir_clean: bool = True,
    ir_sensitivity: float = 0.5,
    gamma_22: bool = False,
    film_type: int = 1,
    export_raw_tiff: bool = True,
    export_dev_tiff: bool = False,
    export_dev_jpg: bool = True,
    jpg_max_dim: int = 0,  # 0 = full native res, e.g. 2048 or 2400 for clean Lanczos-3 web/share
    camera: str = "",
    film_stock: str = "",
    start_index: int = 1,
    reverse_order: bool = False,
    archive_layout: bool = True,
    event_link_mode: str = "symlink",  # "symlink" | "copy" | "off"
) -> dict:
    """V2 batch exporter with Roll + Many-to-Many Event/Carousel archive layout.

    When archive_layout=True (default):
      <out_dir>/rolls/<roll_name>/{raw_16bit, developed_16bit, jpg, roll.json}
      <out_dir>/events/<event_slug>/<roll_name>_NNN.jpg (relative symlink or copy)
    """
    if not frames:
        return {"roll_dir": str(Path(out_dir).expanduser().resolve()), "events_updated": [], "files": []}

    ordered_frames = list(reversed(frames)) if reverse_order else list(frames)
    start_num = max(1, int(start_index))
    safe_roll = _sanitize_token(roll_name or "roll", "roll")

    archive_root = Path(out_dir).expanduser().resolve()
    archive_root.mkdir(parents=True, exist_ok=True)

    if archive_layout:
        roll_dir = archive_root / "rolls" / safe_roll
        events_root = archive_root / "events"
        raw_dir = roll_dir / "raw_16bit"
        dev_tiff_dir = roll_dir / "developed_16bit"
        jpg_dir = roll_dir / "jpg"
    else:
        roll_dir = archive_root
        events_root = archive_root.parent / "events"
        raw_dir = roll_dir / "raw_16bit" if (export_raw_tiff and (export_dev_tiff or export_dev_jpg)) else roll_dir
        dev_tiff_dir = roll_dir / "developed_16bit" if export_dev_tiff else roll_dir
        jpg_dir = roll_dir / "developed_jpg" if export_dev_jpg else roll_dir

    roll_dir.mkdir(parents=True, exist_ok=True)
    if export_raw_tiff:
        raw_dir.mkdir(parents=True, exist_ok=True)
    if export_dev_tiff:
        dev_tiff_dir.mkdir(parents=True, exist_ok=True)
    if export_dev_jpg:
        jpg_dir.mkdir(parents=True, exist_ok=True)

    if film_base is None and film_type != 3:
        film_base = sample_film_base_from_frames(src_path, ordered_frames)

    n = len(ordered_frames)
    c_frames = (_CFrameSpec * n)()
    c_devs = (_CDevelopParams * n)()
    c_raw = (ctypes.c_char_p * n)()
    c_dev = (ctypes.c_char_p * n)()
    c_ppm = (ctypes.c_char_p * n)()

    raw_paths: list[Path | None] = []
    dev_paths: list[Path | None] = []
    ppm_paths: list[Path | None] = []
    frame_nums: list[int] = []

    for i, f in enumerate(ordered_frames):
        explicit_idx = f.get("export_index")
        idx = int(explicit_idx) if (explicit_idx is not None and int(explicit_idx) > 0) else (start_num + i)
        frame_nums.append(idx)
        c_frames[i] = _frame_to_cspec(f)
        c_devs[i] = _dev_to_cparams(f.get("develop"), film_base)

        if export_raw_tiff:
            rp = raw_dir / f"{safe_roll}_{idx:03d}_raw.tif" if raw_dir != roll_dir else roll_dir / f"{safe_roll}_{idx:03d}.tif"
            raw_paths.append(rp)
            c_raw[i] = str(rp).encode("utf-8")
        else:
            raw_paths.append(None)
            c_raw[i] = b""

        if export_dev_tiff:
            dp = dev_tiff_dir / f"{safe_roll}_{idx:03d}.tif"
            dev_paths.append(dp)
            c_dev[i] = str(dp).encode("utf-8")
        else:
            dev_paths.append(None)
            c_dev[i] = b""

        if export_dev_jpg:
            pp = jpg_dir / f".tmp_{safe_roll}_{idx:03d}.ppm"
            ppm_paths.append(pp)
            c_ppm[i] = str(pp).encode("utf-8")
        else:
            ppm_paths.append(None)
            c_ppm[i] = b""

    flat_spots: list[_CIrSpot] = []
    c_spot_offsets = (ctypes.c_int * n)()
    c_spot_counts = (ctypes.c_int * n)()
    for i, f in enumerate(ordered_frames):
        sp_list = f.get("ir_spots") or []
        c_spot_offsets[i] = len(flat_spots)
        c_spot_counts[i] = len(sp_list)
        for sp in sp_list:
            flat_spots.append(
                _CIrSpot(
                    u=float(sp.get("u", 0.0)),
                    v=float(sp.get("v", 0.0)),
                    r=float(sp.get("r", 0.005)),
                    erase=1 if sp.get("erase") else 0,
                )
            )
    if flat_spots:
        c_all_spots = (_CIrSpot * len(flat_spots))(*flat_spots)
        all_spots_ptr = c_all_spots
    else:
        all_spots_ptr = ctypes.POINTER(_CIrSpot)()

    rc = _lib.fc_export_frames_v2(
        str(src_path).encode("utf-8"),
        ctypes.c_int(n),
        c_frames,
        c_devs,
        c_raw,
        c_dev,
        c_ppm,
        ctypes.c_int(1 if apply_ir_clean else 0),
        ctypes.c_float(ir_sensitivity),
        ctypes.c_int(1 if gamma_22 else 0),
        ctypes.c_int(film_type),
        all_spots_ptr,
        c_spot_offsets,
        c_spot_counts,
    )
    if rc != 0:
        raise RuntimeError(f"fc_export_frames_v2 failed (rc={rc})")

    # Load existing roll.json manifest so Holder B (e.g. frames 49..72) merges cleanly with Holder A (1..48)
    manifest_path = roll_dir / "roll.json"
    manifest: dict = {"roll_name": safe_roll, "frames": {}}
    if manifest_path.exists():
        try:
            loaded = json.loads(manifest_path.read_text(encoding="utf-8"))
            if isinstance(loaded, dict):
                manifest = loaded
                if not isinstance(manifest.get("frames"), dict):
                    manifest["frames"] = {}
        except Exception:
            manifest = {"roll_name": safe_roll, "frames": {}}

    link_mode = str(event_link_mode or "symlink").lower()
    if link_mode not in ("symlink", "copy", "off"):
        link_mode = "symlink"

    cam_clean = camera.strip()
    stock_clean = film_stock.strip()

    # Parallelize PPM -> Lanczos-3 resize -> JPEG + EXIF encoding across CPU cores (Pillow releases GIL!)
    ev_slugs_list = [parse_event_slugs(f.get("event")) for f in ordered_frames]
    jpg_outs: list[Path | None] = [None] * n

    def _encode_jpg(i: int) -> tuple[int, Path | None]:
        pp = ppm_paths[i]
        if not (pp and pp.exists()):
            return (i, None)
        idx = frame_nums[i]
        jp = jpg_dir / f"{safe_roll}_{idx:03d}.jpg"
        exif_bytes = _build_exif_bytes(cam_clean, stock_clean, safe_roll, idx, ev_slugs_list[i])
        with Image.open(pp) as pim:
            if jpg_max_dim > 0 and max(pim.size) > jpg_max_dim:
                pim.thumbnail((jpg_max_dim, jpg_max_dim), Image.Resampling.LANCZOS)
            save_kw: dict = {"format": "JPEG", "quality": 93, "subsampling": 0}
            if exif_bytes:
                save_kw["exif"] = exif_bytes
            pim.save(jp, **save_kw)
        os.remove(pp)
        return (i, jp)

    if export_dev_jpg:
        jpg_workers = min(16, max(1, os.cpu_count() or 4), n)
        if jpg_workers > 1:
            with ThreadPoolExecutor(max_workers=jpg_workers) as pool:
                for i, jp in pool.map(_encode_jpg, range(n)):
                    jpg_outs[i] = jp
        else:
            for i in range(n):
                _, jpg_outs[i] = _encode_jpg(i)

    events_touched: set[str] = set()
    results = []

    for i, f in enumerate(ordered_frames):
        idx = frame_nums[i]
        key = f"{idx:03d}"
        ev_slugs = ev_slugs_list[i]
        jpg_out = jpg_outs[i]
        rp = raw_paths[i]
        dp = dev_paths[i]

        # Clean up stale event entries if this frame was re-tagged and re-exported
        prev_entry = manifest["frames"].get(key)
        if isinstance(prev_entry, dict) and events_root.exists():
            prev_events = prev_entry.get("events") or []
            for old_slug in prev_events:
                if old_slug not in ev_slugs or link_mode == "off":
                    old_dir = events_root / old_slug
                    for cand_name in (
                        f"{safe_roll}_{idx:03d}.jpg",
                        f"{safe_roll}_{idx:03d}.tif",
                        f"{safe_roll}_{idx:03d}_raw.tif",
                    ):
                        _unlink_if_exists(old_dir / cand_name)
                    if old_dir.exists():
                        _prune_empty_parents(old_dir, events_root)

        # Populate events/<event_slug>/ (prefers developed JPEG, falls back to dev TIFF / raw TIFF)
        event_links: list[str] = []
        primary_asset = jpg_out or dp or rp
        if link_mode != "off" and ev_slugs and primary_asset and primary_asset.exists():
            events_root.mkdir(parents=True, exist_ok=True)
            for slug in ev_slugs:
                dst = _populate_event_entry(primary_asset, events_root, slug, link_mode)
                event_links.append(str(dst))
                events_touched.add(slug)

        frame_rec = {
            "index": idx,
            "frame_id": f.get("id", ""),
            "events": ev_slugs,
            "raw_tiff": str(rp.relative_to(roll_dir)) if (rp and rp.exists()) else None,
            "dev_tiff": str(dp.relative_to(roll_dir)) if (dp and dp.exists()) else None,
            "dev_jpg": str(jpg_out.relative_to(roll_dir)) if (jpg_out and jpg_out.exists()) else None,
            "source_scan": Path(src_path).name,
        }
        manifest["frames"][key] = frame_rec

        results.append(
            {
                "index": idx,
                "events": ev_slugs,
                "raw_tiff": str(rp) if rp else None,
                "dev_tiff": str(dp) if dp else None,
                "dev_jpg": str(jpg_out) if jpg_out else None,
                "event_links": event_links,
            }
        )

    manifest["roll_name"] = safe_roll
    if cam_clean:
        manifest["camera"] = cam_clean
    if stock_clean:
        manifest["film_stock"] = stock_clean
    if film_base:
        manifest["film_base"] = [round(float(v), 5) for v in film_base]
    manifest["updated_at"] = time.strftime("%Y-%m-%dT%H:%M:%S")
    manifest["frame_count"] = len(manifest["frames"])
    # Keep frames sorted by frame number
    manifest["frames"] = dict(sorted(manifest["frames"].items(), key=lambda kv: int(kv[0])))
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")

    return {
        "roll_dir": str(roll_dir),
        "events_root": str(events_root) if events_touched else None,
        "events_updated": sorted(events_touched),
        "manifest": str(manifest_path),
        "files": results,
    }
