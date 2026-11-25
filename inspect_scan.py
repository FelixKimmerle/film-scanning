#!/usr/bin/env python3
"""Inspect a scanned 16-bit/64-bit TIFF and locate film-holder strip lanes."""
import sys
import numpy as np
from PIL import Image


def inspect(path: str) -> None:
    Image.MAX_IMAGE_PIXELS = None
    with Image.open(path) as im:
        print(f"File: {path}")
        print(f"Size: {im.size[0]} x {im.size[1]} px")
        print(f"Mode: {im.mode}, Format: {im.format}")
        dpi = im.info.get("dpi")
        print(f"DPI:  {dpi}")
        tags = {k: v for k, v in im.tag_v2.items() if k in (256, 257, 258, 259, 262, 277, 282, 283, 296, 338)}
        print(f"TIFF tags: {tags}")

        # Generate a fast 8-bit downscaled preview PNG next to the scan
        im_rgb = im.convert("RGB")
        im_rgb.thumbnail((1200, 1600))
        preview_path = path.rsplit(".", 1)[0] + "_preview.png"
        im_rgb.save(preview_path)
        print(f"Saved preview: {preview_path}")

        # Horizontal intensity profile to find the 4 film-strip holder lanes
        arr = np.asarray(im_rgb, dtype=np.float32).mean(axis=2)
        col_profile = arr.mean(axis=0)
        thresh = (col_profile.max() + col_profile.min()) * 0.4
        in_lane = col_profile > thresh
        lanes = []
        start = None
        for x, val in enumerate(in_lane):
            if val and start is None:
                start = x
            elif not val and start is not None:
                if x - start > 20:
                    lanes.append((start, x))
                start = None
        if start is not None and len(in_lane) - start > 20:
            lanes.append((start, len(in_lane)))

        scale = im.size[0] / arr.shape[1]
        print(f"Detected {len(lanes)} strip lane(s) (in full-res X px):")
        for idx, (x0, x1) in enumerate(lanes, 1):
            fx0, fx1 = int(x0 * scale), int(x1 * scale)
            w_mm = ((fx1 - fx0) / dpi[0] * 25.4) if dpi and dpi[0] else 0
            print(f"  Lane {idx}: x={fx0}..{fx1} (width={fx1 - fx0} px, ~{w_mm:.1f} mm)")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: inspect_scan.py <scan.tif>")
        sys.exit(1)
    inspect(sys.argv[1])
