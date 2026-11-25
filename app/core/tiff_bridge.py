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


class _CDevelopParams(ctypes.Structure):
    _fields_ = [
        ("enabled", ctypes.c_int),
        ("profile", ctypes.c_int),
        ("base_r", ctypes.c_float),
        ("base_g", ctypes.c_float),
        ("base_b", ctypes.c_float),
        ("exposure", ctypes.c_float),
        ("contrast", ctypes.c_float),
        ("blacks", ctypes.c_float),
        ("temp", ctypes.c_float),
        ("tint", ctypes.c_float),
        ("saturation", ctypes.c_float),
        ("chroma_nr", ctypes.c_float),
        ("luma_nr", ctypes.c_float),
        ("sharpen", ctypes.c_float),
        ("auto_wb", ctypes.c_int),
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


def _dev_to_cparams(dev: dict | None, film_base: tuple[float, float, float] | None = None) -> _CDevelopParams:
    d = dev or {}
    prof_raw = d.get("profile", "frontier")
    prof_id = PROFILE_MAP.get(str(prof_raw).lower(), int(prof_raw) if isinstance(prof_raw, int) else 1)
    br = float(d.get("base_r", film_base[0] if film_base else 0.0))
    bg = float(d.get("base_g", film_base[1] if film_base else 0.0))
    bb = float(d.get("base_b", film_base[2] if film_base else 0.0))
    return _CDevelopParams(
        enabled=1 if d.get("enabled", True) else 0,
        profile=prof_id,
        base_r=br,
        base_g=bg,
        base_b=bb,
        exposure=float(d.get("exposure", 0.0)),
        contrast=float(d.get("contrast", 1.0)),
        blacks=float(d.get("blacks", 0.0)),
        temp=float(d.get("temp", 0.0)),
        tint=float(d.get("tint", 0.0)),
        saturation=float(d.get("saturation", 1.0)),
        chroma_nr=float(d.get("chroma_nr", 0.55)),
        luma_nr=float(d.get("luma_nr", 0.15)),
        sharpen=float(d.get("sharpen", 0.25)),
        auto_wb=1 if d.get("auto_wb", True) else 0,
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
) -> np.ndarray:
    """Fast random-access live preview of one developed frame (returns RGB8 ndarray)."""
    cspec = _frame_to_cspec(frame)
    cdev = _dev_to_cparams(dev, film_base)
    alloc_w = max_dim if max_dim > 0 else int(max(cspec.width, cspec.height) + 16)
    buf = np.empty((alloc_w * alloc_w * 3,), dtype=np.uint8)
    ow = ctypes.c_int(0)
    oh = ctypes.c_int(0)
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
    )
    if rc != 0:
        raise RuntimeError(f"fc_develop_frame_preview failed (rc={rc})")
    w, h = int(ow.value), int(oh.value)
    return buf[: w * h * 3].reshape((h, w, 3)).copy()


def _build_exif_bytes(camera: str, film_stock: str) -> bytes | None:
    if not camera and not film_stock:
        return None
    exif = Image.Exif()
    if camera:
        exif[0x0110] = camera  # Model
    exif[0x0131] = "Film Cockpit V2 (Epson Perfection V750 Pro)"  # Software
    if film_stock:
        exif[0x010E] = f"Film: {film_stock} | Scanner: Epson Perfection V750 Pro"  # ImageDescription
    return exif.tobytes()


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
) -> list[dict]:
    """V2 batch exporter: supports 16-bit RAW TIFF, 16-bit Developed TIFF, and Developed JPEG (+EXIF)."""
    if not frames:
        return []
    out_path = Path(out_dir).expanduser().resolve()
    out_path.mkdir(parents=True, exist_ok=True)

    raw_dir = out_path / "raw_16bit" if (export_raw_tiff and (export_dev_tiff or export_dev_jpg)) else out_path
    dev_tiff_dir = out_path / "developed_16bit" if export_dev_tiff else out_path
    jpg_dir = out_path / "developed_jpg" if export_dev_jpg else out_path
    if export_raw_tiff:
        raw_dir.mkdir(parents=True, exist_ok=True)
    if export_dev_tiff:
        dev_tiff_dir.mkdir(parents=True, exist_ok=True)
    if export_dev_jpg:
        jpg_dir.mkdir(parents=True, exist_ok=True)

    safe_roll = "".join(c if (c.isalnum() or c in "-_") else "_" for c in (roll_name or "roll")).strip("_") or "roll"
    if film_base is None and film_type != 3:
        film_base = sample_film_base_from_frames(src_path, frames)

    n = len(frames)
    c_frames = (_CFrameSpec * n)()
    c_devs = (_CDevelopParams * n)()
    c_raw = (ctypes.c_char_p * n)()
    c_dev = (ctypes.c_char_p * n)()
    c_ppm = (ctypes.c_char_p * n)()

    raw_paths: list[Path | None] = []
    dev_paths: list[Path | None] = []
    ppm_paths: list[Path | None] = []

    for i, f in enumerate(frames):
        idx = i + 1
        c_frames[i] = _frame_to_cspec(f)
        c_devs[i] = _dev_to_cparams(f.get("develop"), film_base)

        if export_raw_tiff:
            rp = raw_dir / f"{safe_roll}_{idx:03d}_raw.tif" if raw_dir != out_path else out_path / f"{safe_roll}_{idx:03d}.tif"
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
    )
    if rc != 0:
        raise RuntimeError(f"fc_export_frames_v2 failed (rc={rc})")

    exif_bytes = _build_exif_bytes(camera.strip(), film_stock.strip())
    results = []
    for i in range(n):
        jpg_out = None
        pp = ppm_paths[i]
        if pp and pp.exists():
            jp = jpg_dir / f"{safe_roll}_{i + 1:03d}.jpg"
            with Image.open(pp) as pim:
                if jpg_max_dim > 0 and max(pim.size) > jpg_max_dim:
                    pim.thumbnail((jpg_max_dim, jpg_max_dim), Image.Resampling.LANCZOS)
                save_kw = {"format": "JPEG", "quality": 93, "subsampling": 0}
                if exif_bytes:
                    save_kw["exif"] = exif_bytes
                pim.save(jp, **save_kw)
            os.remove(pp)
            jpg_out = str(jp)

        rp = raw_paths[i]
        dp = dev_paths[i]
        results.append(
            {
                "index": i + 1,
                "raw_tiff": str(rp) if rp else None,
                "dev_tiff": str(dp) if dp else None,
                "dev_jpg": jpg_out,
            }
        )
    return results
