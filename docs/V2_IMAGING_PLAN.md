# Film Cockpit V2 — Imaging Core & Focused Develop UI Plan

**Design principle:** Do **not** reinvent Lightroom. Build a razor-focused, fast, standalone film lab tuned specifically for an **Epson V750 Pro + 35mm / Olympus Pen FV** workflow — while keeping 16-bit linear RAW export open anytime.

## 1. Native C++20 Imaging Upgrades (`app/core/film_core.cpp`)

### A. V2 Hardware IR Dust & Scratch Cleaner
- **Auto RGB ↔ IR alignment `(dx, dy)`:** Cross-correlates IR dust spots against RGB dips (`±8 px`) to remove residual `1–3 px` V750 pass offset.
- **Tighter dilation (`2–3 px` instead of `5 px`):** Halves the healed footprint around thin hairs and sharp edges.
- **RGB dip cross-check:** Rejects dense-highlight IR dye crosstalk where raw RGB has no physical dust shadow.
- **Fast Gauss-Seidel SOR + local 16-bit film-grain re-injection:** Converges across thick 3200 DPI fibers and re-injects matched surrounding grain variance so repairs disappear even at 1:1 zoom.

### B. Noise Separation (Digital Chroma Noise vs. Organic Film Grain)
- **Edge-preserving Chroma Bilateral Filter (YCbCr `Cb`/`Cr` only):** Removes purple/green shadow speckle with zero loss of sharpness or film grain.
- **Optional Shadow-Weighted Luma Denoise (`0–100%`):** Gentle edge-preserving filter weighted toward dark negative shadows where flatbed CCD noise dominates.
- **Flatbed Optical Sharpening (USM) + optional clean Lanczos-3 downscale** for sharing/archive JPEGs.

### C. Rebate-Sampled Film Lab Color Engine (C-41 / B&W / Slide)
- **True Film-Base (`D-min`) Sampling:** Samples the actual unexposed orange rebate right between frames on each strip instead of guessing from image percentiles.
- **3×3 Dye Unmixing Matrix + Look Profiles:** `NATURAL`, `FRONTIER`, `NORITSU`, `WARM`.
- **RA-4 Print Paper S-Curve:** Smooth highlight roll-off + rich toe contrast instead of flat linear stretch.
- **Per-Frame Auto White Balance (Shades-of-Gray) + Viewport WB Eyedropper.**

### D. Archive-Ready Exports
- **EXIF metadata:** Optional `Camera`, `Film Stock`, and `Scanner: Epson Perfection V750 Pro` tags.
- **Flexible outputs:** `16-BIT LINEAR RAW TIFF`, `16-BIT DEVELOPED TIFF`, and/or `DEVELOPED JPEG` (`FULL` / `2400px` / `2048px`).

## 2. Minimal 2-Tab Glass Cockpit UI (`[ CROP ]` | `[ DEVELOP ]`)
- Keeps the exact 3-column Glass Cockpit layout with draggable docks.
- `[ CROP ]` tab: existing fast full-holder border/strip/frame-stepping cockpit.
- `[ DEVELOP ]` tab: swaps only the Right Dock into a compact Film Lab panel (`PROFILE`, `AUTO WB` + `PICKER`, `EXPOSURE`, `CONTRAST`, `BLACKS`, `TEMP`, `TINT`, `CHROMA NR`, `LUMA NR`, `SHARPEN`, `APPLY TO STRIP / ALL`, `RESET`), with live preview and hold-to-compare Before/After (`\`).
- Persists per-frame develop settings in `scans/<scan>.tif.layout.json` via existing 0.9s auto-save and 100-step Undo/Redo.
