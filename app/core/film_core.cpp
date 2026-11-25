// film_core.cpp — Native C++20 16-bit TIFF engine for analog film scanning.
// Features:
//  - Lossless 16-bit RGB + 16-bit IR TIFF reader (supports both 2-page SilverFast
//    64-bit HDRi and 4-channel VueScan 64-bit RGBI, plus standard 48-bit/24-bit TIFF)
//  - Fast anti-aliased preview + live IR dust-mask + optical-density negative inversion
//  - Full-res hardware IR dust/scratch detection (O(1) integral-image local background)
//    and 16-bit harmonic diffusion inpainting
//  - Sub-pixel bilinear rotated crop with inset margin & atomic 16-bit TIFF export

#include <tiffio.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct TiffInfo {
    uint32_t width;
    uint32_t height;
    float dpi_x;
    float dpi_y;
    uint16_t bits_per_sample;
    uint16_t samples_per_pixel;
    uint8_t has_ir;
    uint8_t is_linear_hdr;
    char software[128];
};

struct FrameSpec {
    double cx;
    double cy;
    double width;
    double height;
    double angle_deg;
    double inset_pct;
};

struct DevelopParams {
    int enabled;       // 0 = raw / legacy proof, 1 = full Film Lab develop pipeline
    int profile;       // 0=NATURAL, 1=FRONTIER, 2=NORITSU, 3=WARM, 4=BW_CLASSIC
    float base_r;      // true unexposed film base R (0..1 linear; <=0 means auto)
    float base_g;
    float base_b;
    float exposure;    // -2.0 .. +2.0 EV (default 0.0)
    float contrast;    // 0.60 .. 1.60 RA-4 paper grade (default 1.0)
    float blacks;      // -0.5 .. +0.5 toe lift/crush (default 0.0)
    float temp;        // -1.0 .. +1.0 (Blue <-> Amber)
    float tint;        // -1.0 .. +1.0 (Green <-> Magenta)
    float saturation;  // 0.0 .. 2.0 (default 1.0)
    float chroma_nr;   // 0.0 .. 1.0 YCbCr edge-preserving chroma bilateral NR
    float luma_nr;     // 0.0 .. 1.0 shadow-weighted edge-preserving luma NR
    float sharpen;     // 0.0 .. 1.0 flatbed optical USM
    int auto_wb;       // 0 = off, 1 = Shades-of-Gray illuminant balance in density space
};

struct LoadedScan {
    uint32_t w = 0;
    uint32_t h = 0;
    float dpi_x = 300.0f;
    float dpi_y = 300.0f;
    uint16_t bps = 16;
    bool has_ir = false;
    bool is_linear_hdr = false;
    std::string software;
    std::vector<uint16_t> rgb; // w * h * 3
    std::vector<uint16_t> ir;  // w * h (if has_ir)
};

inline float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

inline uint8_t to_u8(float v) {
    int iv = static_cast<int>(std::lround(clamp01(v) * 255.0f));
    return static_cast<uint8_t>(iv < 0 ? 0 : (iv > 255 ? 255 : iv));
}

inline uint16_t to_u16(float v) {
    int iv = static_cast<int>(std::lround(clamp01(v) * 65535.0f));
    return static_cast<uint16_t>(iv < 0 ? 0 : (iv > 65535 ? 65535 : iv));
}

static bool load_tiff(const char* path, LoadedScan& out) {
    TIFFSetWarningHandler(nullptr);
    TIFF* tif = TIFFOpen(path, "r");
    if (!tif) return false;

    uint32_t w = 0, h = 0;
    uint16_t bps = 8, spp = 3, planar = PLANARCONFIG_CONTIG;
    float dx = 0.0f, dy = 0.0f;
    uint16_t res_unit = RESUNIT_INCH;
    char* sw = nullptr;

    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bps);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
    TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &planar);
    TIFFGetField(tif, TIFFTAG_XRESOLUTION, &dx);
    TIFFGetField(tif, TIFFTAG_YRESOLUTION, &dy);
    TIFFGetFieldDefaulted(tif, TIFFTAG_RESOLUTIONUNIT, &res_unit);
    if (TIFFGetField(tif, TIFFTAG_SOFTWARE, &sw) && sw) {
        out.software = sw;
    }

    if (res_unit == RESUNIT_CENTIMETER) {
        dx *= 2.54f;
        dy *= 2.54f;
    }
    if (dx <= 1.0f) dx = 300.0f;
    if (dy <= 1.0f) dy = dx;

    if (w == 0 || h == 0 || (bps != 8 && bps != 16) || spp < 1) {
        TIFFClose(tif);
        return false;
    }

    out.w = w;
    out.h = h;
    out.dpi_x = dx;
    out.dpi_y = dy;
    out.bps = bps;
    out.has_ir = false;
    out.is_linear_hdr = (bps == 16) && (
        out.software.find("SilverFast") != std::string::npos ||
        out.software.find("VueScan") != std::string::npos
    );

    const size_t npix = static_cast<size_t>(w) * h;
    out.rgb.assign(npix * 3, 0);

    // Handle 4-channel RGBI directly if present on page 0
    if (spp >= 4) {
        out.has_ir = true;
        out.ir.assign(npix, 0);
    }

    const tmsize_t scanline_sz = TIFFScanlineSize(tif);
    std::vector<uint8_t> buf(static_cast<size_t>(scanline_sz));

    if (planar == PLANARCONFIG_CONTIG) {
        for (uint32_t y = 0; y < h; ++y) {
            if (TIFFReadScanline(tif, buf.data(), y, 0) < 0) {
                TIFFClose(tif);
                return false;
            }
            uint16_t* dst_rgb = &out.rgb[static_cast<size_t>(y) * w * 3];
            uint16_t* dst_ir = out.has_ir ? &out.ir[static_cast<size_t>(y) * w] : nullptr;

            if (bps == 16) {
                const uint16_t* src = reinterpret_cast<const uint16_t*>(buf.data());
                if (spp == 1) {
                    for (uint32_t x = 0; x < w; ++x) {
                        uint16_t v = src[x];
                        dst_rgb[x * 3 + 0] = v;
                        dst_rgb[x * 3 + 1] = v;
                        dst_rgb[x * 3 + 2] = v;
                    }
                } else {
                    for (uint32_t x = 0; x < w; ++x) {
                        dst_rgb[x * 3 + 0] = src[x * spp + 0];
                        dst_rgb[x * 3 + 1] = src[x * spp + 1];
                        dst_rgb[x * 3 + 2] = src[x * spp + 2];
                        if (dst_ir) dst_ir[x] = src[x * spp + 3];
                    }
                }
            } else {
                const uint8_t* src = buf.data();
                if (spp == 1) {
                    for (uint32_t x = 0; x < w; ++x) {
                        uint16_t v = static_cast<uint16_t>(src[x]) * 257u;
                        dst_rgb[x * 3 + 0] = v;
                        dst_rgb[x * 3 + 1] = v;
                        dst_rgb[x * 3 + 2] = v;
                    }
                } else {
                    for (uint32_t x = 0; x < w; ++x) {
                        dst_rgb[x * 3 + 0] = static_cast<uint16_t>(src[x * spp + 0]) * 257u;
                        dst_rgb[x * 3 + 1] = static_cast<uint16_t>(src[x * spp + 1]) * 257u;
                        dst_rgb[x * 3 + 2] = static_cast<uint16_t>(src[x * spp + 2]) * 257u;
                        if (dst_ir) dst_ir[x] = static_cast<uint16_t>(src[x * spp + 3]) * 257u;
                    }
                }
            }
        }
    } else {
        // Planar separate
        for (uint16_t s = 0; s < std::min<uint16_t>(spp, 4); ++s) {
            for (uint32_t y = 0; y < h; ++y) {
                if (TIFFReadScanline(tif, buf.data(), y, s) < 0) {
                    TIFFClose(tif);
                    return false;
                }
                for (uint32_t x = 0; x < w; ++x) {
                    uint16_t v = (bps == 16)
                        ? reinterpret_cast<const uint16_t*>(buf.data())[x]
                        : static_cast<uint16_t>(buf[x]) * 257u;
                    if (s < 3) out.rgb[(static_cast<size_t>(y) * w + x) * 3 + s] = v;
                    else if (out.has_ir) out.ir[static_cast<size_t>(y) * w + x] = v;
                }
            }
        }
    }

    // Scan subsequent TIFF directories for SilverFast 64-bit HDRi infrared page (I;16).
    // At >=600 DPI SilverFast inserts a reduced-res thumbnail at directory 1 and puts IR at directory 2.
    while (!out.has_ir && TIFFReadDirectory(tif)) {
        uint32_t w2 = 0, h2 = 0;
        uint16_t bps2 = 8, spp2 = 1;
        TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w2);
        TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h2);
        TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bps2);
        TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &spp2);
        if (w2 == w && h2 == h && spp2 == 1 && (bps2 == 16 || bps2 == 8)) {
            out.has_ir = true;
            out.ir.assign(npix, 0);
            std::vector<uint8_t> ibuf(static_cast<size_t>(TIFFScanlineSize(tif)));
            for (uint32_t y = 0; y < h; ++y) {
                if (TIFFReadScanline(tif, ibuf.data(), y, 0) < 0) {
                    out.has_ir = false;
                    out.ir.clear();
                    break;
                }
                uint16_t* dst = &out.ir[static_cast<size_t>(y) * w];
                if (bps2 == 16) {
                    std::memcpy(dst, ibuf.data(), w * sizeof(uint16_t));
                } else {
                    for (uint32_t x = 0; x < w; ++x) dst[x] = static_cast<uint16_t>(ibuf[x]) * 257u;
                }
            }
        }
    }

    TIFFClose(tif);
    return true;
}

// Fast O(1)-per-pixel local background via 2D integral image for IR dust detection.
// Dust/hairs/scratches attenuate IR light sharply relative to surrounding film base.
// V2 Hardware IR dust mask:
//  1. Integral-image local IR drop detection (ignores black plastic holder bars)
//  2. Isolated 1-pixel IR sensor spike rejection
//  3. Optional automatic RGB<->IR shift alignment (+/-6 px) + RGB dip cross-check
//     (rejects faint dense-highlight dye crosstalk while keeping 100% of real dust/hairs)
//  4. Tight morphological dilation to cover dust halos without over-blurring
static void compute_ir_dust_mask(
    const uint16_t* ir,
    int w,
    int h,
    int radius,
    float sensitivity, // 0.0 .. 1.0 (default ~0.5)
    int dilate_px,
    std::vector<uint8_t>& mask_out,
    const uint16_t* rgb = nullptr // optional: enables auto RGB<->IR shift alignment + dye-bleed guard
) {
    const size_t npix = static_cast<size_t>(w) * h;
    mask_out.assign(npix, 0);
    if (!ir || w < 8 || h < 8 || sensitivity <= 0.001f) return;

    const int iw = w + 1;
    const uint16_t holder_floor = 3500;
    std::vector<double> integ(static_cast<size_t>(h + 1) * iw, 0.0);
    std::vector<int32_t> valid_integ(static_cast<size_t>(h + 1) * iw, 0);
    for (int y = 0; y < h; ++y) {
        double row_sum = 0.0;
        int32_t row_valid = 0;
        const uint16_t* rptr = ir + static_cast<size_t>(y) * w;
        double* cur = &integ[static_cast<size_t>(y + 1) * iw];
        const double* prev = &integ[static_cast<size_t>(y) * iw];
        int32_t* vcur = &valid_integ[static_cast<size_t>(y + 1) * iw];
        const int32_t* vprev = &valid_integ[static_cast<size_t>(y) * iw];
        for (int x = 0; x < w; ++x) {
            uint16_t v = rptr[x];
            if (v > holder_floor) {
                row_sum += static_cast<double>(v);
                row_valid += 1;
            }
            cur[x + 1] = prev[x + 1] + row_sum;
            vcur[x + 1] = vprev[x + 1] + row_valid;
        }
    }

    // Threshold: sensitivity 0.0 -> 0.16 drop, 0.5 -> 0.055 drop, 1.0 -> 0.015 drop
    const double rel_drop_thresh = std::max(0.012, 0.14 * std::pow(0.12, static_cast<double>(sensitivity)));

    // Store strength: 0 = clean, 1 = borderline IR dip, 2 = strong opaque dust/hair
    std::vector<uint8_t> raw_mask(npix, 0);
    for (int y = 0; y < h; ++y) {
        int y0 = std::max(0, y - radius);
        int y1 = std::min(h, y + radius + 1);
        for (int x = 0; x < w; ++x) {
            int x0 = std::max(0, x - radius);
            int x1 = std::min(w, x + radius + 1);
            int32_t area = (y1 - y0) * (x1 - x0);
            int32_t vcnt = valid_integ[static_cast<size_t>(y1) * iw + x1]
                         - valid_integ[static_cast<size_t>(y0) * iw + x1]
                         - valid_integ[static_cast<size_t>(y1) * iw + x0]
                         + valid_integ[static_cast<size_t>(y0) * iw + x0];
            if (vcnt < area * 0.85) continue;

            double sum = integ[static_cast<size_t>(y1) * iw + x1]
                       - integ[static_cast<size_t>(y0) * iw + x1]
                       - integ[static_cast<size_t>(y1) * iw + x0]
                       + integ[static_cast<size_t>(y0) * iw + x0];
            double local_mean = sum / static_cast<double>(vcnt);
            double val = static_cast<double>(ir[static_cast<size_t>(y) * w + x]);
            double rel_drop = (local_mean - val) / std::max(1.0, local_mean);
            if (rel_drop > rel_drop_thresh) {
                raw_mask[static_cast<size_t>(y) * w + x] = (rel_drop > rel_drop_thresh * 2.3) ? 2 : 1;
            }
        }
    }

    // Remove isolated 1-pixel IR sensor noise spikes (require >= 1 neighbor in 3x3)
    std::vector<uint8_t> denoised(npix, 0);
    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            size_t idx = static_cast<size_t>(y) * w + x;
            if (!raw_mask[idx]) continue;
            int nb = (raw_mask[idx - 1] > 0) + (raw_mask[idx + 1] > 0)
                   + (raw_mask[idx - w] > 0) + (raw_mask[idx + w] > 0)
                   + (raw_mask[idx - w - 1] > 0) + (raw_mask[idx - w + 1] > 0)
                   + (raw_mask[idx + w - 1] > 0) + (raw_mask[idx + w + 1] > 0);
            if (nb >= 1) denoised[idx] = raw_mask[idx];
        }
    }
    raw_mask.swap(denoised);

    // Optional V2 auto RGB<->IR shift alignment + RGB dip cross-check
    if (rgb) {
        // Compute grayscale RGB luminance & its integral image for O(1) local mean
        std::vector<float> rgb_lum(npix);
        std::vector<double> rgb_integ(static_cast<size_t>(h + 1) * iw, 0.0);
        for (int y = 0; y < h; ++y) {
            double rsum = 0.0;
            double* cur = &rgb_integ[static_cast<size_t>(y + 1) * iw];
            const double* prev = &rgb_integ[static_cast<size_t>(y) * iw];
            for (int x = 0; x < w; ++x) {
                size_t idx = static_cast<size_t>(y) * w + x;
                float lum = 0.299f * rgb[idx * 3 + 0] + 0.587f * rgb[idx * 3 + 1] + 0.114f * rgb[idx * 3 + 2];
                rgb_lum[idx] = lum;
                rsum += static_cast<double>(lum);
                cur[x + 1] = prev[x + 1] + rsum;
            }
        }
        auto local_rgb_mean = [&](int x, int y, int r) -> double {
            int x0 = std::max(0, x - r), x1 = std::min(w, x + r + 1);
            int y0 = std::max(0, y - r), y1 = std::min(h, y + r + 1);
            double s = rgb_integ[static_cast<size_t>(y1) * iw + x1]
                     - rgb_integ[static_cast<size_t>(y0) * iw + x1]
                     - rgb_integ[static_cast<size_t>(y1) * iw + x0]
                     + rgb_integ[static_cast<size_t>(y0) * iw + x0];
            return s / std::max(1, (x1 - x0) * (y1 - y0));
        };

        // Collect strong dust sample points (up to ~6000) to estimate sub-pixel RGB<->IR offset (+/-6 px)
        std::vector<std::pair<int, int>> pts;
        pts.reserve(4096);
        const int margin = 10;
        for (int y = margin; y < h - margin; ++y) {
            for (int x = margin; x < w - margin; ++x) {
                if (raw_mask[static_cast<size_t>(y) * w + x] >= 2) {
                    pts.emplace_back(x, y);
                }
            }
        }
        if (pts.size() < 40) {
            for (int y = margin; y < h - margin; ++y) {
                for (int x = margin; x < w - margin; ++x) {
                    if (raw_mask[static_cast<size_t>(y) * w + x] >= 1) pts.emplace_back(x, y);
                }
            }
        }

        int best_dx = 0, best_dy = 0;
        if (pts.size() >= 25) {
            const size_t stride = std::max<size_t>(1, pts.size() / 4000);
            const int r_loc = std::max(8, radius / 2);
            auto eval_shift = [&](int dx, int dy) -> double {
                double sc = 0.0;
                for (size_t i = 0; i < pts.size(); i += stride) {
                    int sx = pts[i].first + dx;
                    int sy = pts[i].second + dy;
                    double lm = local_rgb_mean(sx, sy, r_loc);
                    double v = static_cast<double>(rgb_lum[static_cast<size_t>(sy) * w + sx]);
                    double dip = (lm - v) / std::max(200.0, lm);
                    if (dip > 0.0) sc += dip;
                }
                return sc;
            };
            double sc0 = eval_shift(0, 0);
            double best_sc = sc0;
            for (int dy = -6; dy <= 6; ++dy) {
                for (int dx = -6; dx <= 6; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    double sc = eval_shift(dx, dy);
                    if (sc > best_sc) {
                        best_sc = sc;
                        best_dx = dx;
                        best_dy = dy;
                    }
                }
            }
            // Only apply shift if clearly better than zero-shift (>4% correlation gain)
            if (best_sc < sc0 * 1.04) {
                best_dx = 0;
                best_dy = 0;
            }
        }

        // Apply shift + cross-check borderline IR hits against RGB local dip
        // (prevents dense-highlight dye crosstalk from blurring clean textured areas)
        std::vector<uint8_t> aligned(npix, 0);
        const int r_chk = std::max(8, radius / 2);
        for (int y = 0; y < h; ++y) {
            int dy_y = y + best_dy;
            if (dy_y < 0 || dy_y >= h) continue;
            for (int x = 0; x < w; ++x) {
                uint8_t st = raw_mask[static_cast<size_t>(y) * w + x];
                if (!st) continue;
                int dx_x = x + best_dx;
                if (dx_x < 0 || dx_x >= w) continue;
                if (st >= 2) {
                    // Strong opaque dust/hair in IR -> always keep
                    aligned[static_cast<size_t>(dy_y) * w + dx_x] = 255;
                } else {
                    // Borderline IR dip -> verify physical light blocking in raw RGB
                    double lm = local_rgb_mean(dx_x, dy_y, r_chk);
                    double v = static_cast<double>(rgb_lum[static_cast<size_t>(dy_y) * w + dx_x]);
                    if ((lm - v) > lm * 0.015) {
                        aligned[static_cast<size_t>(dy_y) * w + dx_x] = 255;
                    }
                }
            }
        }
        raw_mask.swap(aligned);
    } else {
        for (uint8_t& v : raw_mask) v = v ? 255 : 0;
    }

    // Morphological dilation (box radius = dilate_px) to cover dust halos
    if (dilate_px <= 0) {
        mask_out.swap(raw_mask);
        return;
    }
    std::vector<uint8_t> tmp(npix, 0);
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = &raw_mask[static_cast<size_t>(y) * w];
        uint8_t* dst = &tmp[static_cast<size_t>(y) * w];
        for (int x = 0; x < w; ++x) {
            if (src[x]) {
                int x0 = std::max(0, x - dilate_px);
                int x1 = std::min(w - 1, x + dilate_px);
                for (int xx = x0; xx <= x1; ++xx) dst[xx] = 255;
            }
        }
    }
    for (int y = 0; y < h; ++y) {
        int y0 = std::max(0, y - dilate_px);
        int y1 = std::min(h - 1, y + dilate_px);
        uint8_t* dst = &mask_out[static_cast<size_t>(y) * w];
        for (int yy = y0; yy <= y1; ++yy) {
            const uint8_t* src = &tmp[static_cast<size_t>(yy) * w];
            for (int x = 0; x < w; ++x) {
                if (src[x]) dst[x] = 255;
            }
        }
    }
}

// Fast deterministic integer hash -> approximate unit-variance Gaussian noise [-2.2 .. +2.2]
static inline float grain_hash(uint32_t x, uint32_t y, uint32_t seed) {
    auto h = [](uint32_t v) -> float {
        v ^= v >> 16;
        v *= 0x7feb352du;
        v ^= v >> 15;
        v *= 0x846ca68bu;
        v ^= v >> 16;
        return static_cast<float>(v & 0xFFFFu) / 65535.0f - 0.5f;
    };
    // Sum of 3 uniform samples -> Irwin-Hall approx Gaussian, std = 0.5 -> multiply by 2.0
    uint32_t k = x * 374761393u + y * 668265263u + seed * 1442695041u;
    return 2.0f * (h(k) + h(k + 1013904223u) + h(k + 2246822519u));
}

// V2 16-bit SOR harmonic diffusion inpainting + local film-grain re-injection ONLY on masked pixels.
// Untouched pixels remain 100% bit-exact.
static void inpaint_rgb16(
    uint16_t* rgb,
    const uint8_t* mask,
    int w,
    int h,
    int iterations = 64
) {
    std::vector<int> masked_indices;
    masked_indices.reserve(4096);
    const size_t npix = static_cast<size_t>(w) * h;
    for (size_t i = 0; i < npix; ++i) {
        if (mask[i]) masked_indices.push_back(static_cast<int>(i));
    }
    if (masked_indices.empty()) return;

    std::vector<float> buf(npix * 3);
    for (size_t i = 0; i < npix * 3; ++i) buf[i] = static_cast<float>(rgb[i]);

    // Fast wavefront seed from nearest clean pixels (up to 40 layers)
    std::vector<uint8_t> filled(npix);
    for (size_t i = 0; i < npix; ++i) filled[i] = mask[i] ? 0 : 1;

    std::vector<int> frontier;
    frontier.reserve(masked_indices.size());
    for (int pass = 0; pass < 40; ++pass) {
        frontier.clear();
        for (int idx : masked_indices) {
            if (filled[idx]) continue;
            int y = idx / w, x = idx % w;
            float sr = 0, sg = 0, sb = 0;
            int cnt = 0;
            const int dx[4] = {-1, 1, 0, 0};
            const int dy[4] = {0, 0, -1, 1};
            for (int d = 0; d < 4; ++d) {
                int nx = x + dx[d], ny = y + dy[d];
                if (nx >= 0 && nx < w && ny >= 0 && ny < h) {
                    size_t ni = static_cast<size_t>(ny) * w + nx;
                    if (filled[ni]) {
                        sr += buf[ni * 3 + 0];
                        sg += buf[ni * 3 + 1];
                        sb += buf[ni * 3 + 2];
                        cnt++;
                    }
                }
            }
            if (cnt > 0) {
                buf[static_cast<size_t>(idx) * 3 + 0] = sr / cnt;
                buf[static_cast<size_t>(idx) * 3 + 1] = sg / cnt;
                buf[static_cast<size_t>(idx) * 3 + 2] = sb / cnt;
                frontier.push_back(idx);
            }
        }
        if (frontier.empty()) break;
        for (int idx : frontier) filled[idx] = 1;
    }

    // Successive Over-Relaxation (SOR, omega=1.65) converges ~5x faster than plain Gauss-Seidel
    const float omega = 1.65f;
    for (int it = 0; it < iterations; ++it) {
        for (int idx : masked_indices) {
            int y = idx / w, x = idx % w;
            float sr = 0, sg = 0, sb = 0;
            int cnt = 0;
            if (x > 0) { size_t n = idx - 1; sr += buf[n*3]; sg += buf[n*3+1]; sb += buf[n*3+2]; cnt++; }
            if (x + 1 < w) { size_t n = idx + 1; sr += buf[n*3]; sg += buf[n*3+1]; sb += buf[n*3+2]; cnt++; }
            if (y > 0) { size_t n = idx - w; sr += buf[n*3]; sg += buf[n*3+1]; sb += buf[n*3+2]; cnt++; }
            if (y + 1 < h) { size_t n = idx + w; sr += buf[n*3]; sg += buf[n*3+1]; sb += buf[n*3+2]; cnt++; }
            if (cnt > 0) {
                size_t o = static_cast<size_t>(idx) * 3;
                float avg_r = sr / cnt, avg_g = sg / cnt, avg_b = sb / cnt;
                buf[o + 0] = std::clamp(buf[o + 0] + omega * (avg_r - buf[o + 0]), 0.0f, 65535.0f);
                buf[o + 1] = std::clamp(buf[o + 1] + omega * (avg_g - buf[o + 1]), 0.0f, 65535.0f);
                buf[o + 2] = std::clamp(buf[o + 2] + omega * (avg_b - buf[o + 2]), 0.0f, 65535.0f);
            }
        }
    }

    // Local 16-bit film-grain re-injection:
    // Estimate local high-frequency grain noise (difference between adjacent clean pixels / sqrt(2))
    // in a small ring around each healed pixel so repaired fibers blend seamlessly even at 1:1 3200 DPI!
    for (int idx : masked_indices) {
        int y = idx / w, x = idx % w;
        float var_r = 0, var_g = 0, var_b = 0;
        int n_pairs = 0;
        const int r_g = 10;
        for (int yy = std::max(1, y - r_g); yy <= std::min(h - 2, y + r_g); yy += 2) {
            for (int xx = std::max(1, x - r_g); xx <= std::min(w - 2, x + r_g); xx += 2) {
                size_t ci = static_cast<size_t>(yy) * w + xx;
                if (mask[ci] || mask[ci + 1]) continue;
                float dr = static_cast<float>(rgb[ci * 3 + 0]) - static_cast<float>(rgb[(ci + 1) * 3 + 0]);
                float dg = static_cast<float>(rgb[ci * 3 + 1]) - static_cast<float>(rgb[(ci + 1) * 3 + 1]);
                float db = static_cast<float>(rgb[ci * 3 + 2]) - static_cast<float>(rgb[(ci + 1) * 3 + 2]);
                // Robust clamp so sharp object edges don't inflate grain estimate
                float lim = std::max(400.0f, 0.06f * (buf[idx * 3 + 1] + 500.0f));
                if (std::fabs(dg) < lim) {
                    var_r += dr * dr;
                    var_g += dg * dg;
                    var_b += db * db;
                    n_pairs++;
                }
            }
        }
        size_t o = static_cast<size_t>(idx) * 3;
        float nr = buf[o + 0], ng = buf[o + 1], nb = buf[o + 2];
        if (n_pairs >= 6) {
            float sig_r = std::sqrt(var_r / (2.0f * n_pairs));
            float sig_g = std::sqrt(var_g / (2.0f * n_pairs));
            float sig_b = std::sqrt(var_b / (2.0f * n_pairs));
            // 2x2 slightly correlated organic grain clump (luminance-coupled + slight chroma)
            float g_lum = 0.55f * grain_hash(x, y, 11)
                        + 0.15f * grain_hash(x + 1, y, 11)
                        + 0.15f * grain_hash(x, y + 1, 11)
                        + 0.15f * grain_hash(x - 1, y, 11);
            float g_r = 0.75f * g_lum + 0.25f * grain_hash(x, y, 23);
            float g_g = 0.75f * g_lum + 0.25f * grain_hash(x, y, 37);
            float g_b = 0.75f * g_lum + 0.25f * grain_hash(x, y, 53);
            nr += g_r * sig_r * 0.90f;
            ng += g_g * sig_g * 0.90f;
            nb += g_b * sig_b * 0.90f;
        }
        rgb[o + 0] = static_cast<uint16_t>(std::clamp(std::lround(nr), 0l, 65535l));
        rgb[o + 1] = static_cast<uint16_t>(std::clamp(std::lround(ng), 0l, 65535l));
        rgb[o + 2] = static_cast<uint16_t>(std::clamp(std::lround(nb), 0l, 65535l));
    }
}

// Estimate roll-wide optical-density inversion parameters [base_r, base_g, base_b, hi_r, hi_g, hi_b]
// so both overview previews and zoomed 1:1 native tiles share identical brightness/color balance.
static bool estimate_inversion_params_from_rgb16(
    const uint16_t* rgb16,
    int w,
    int h,
    bool is_linear,
    float out_params[6]
) {
    const size_t npix = static_cast<size_t>(w) * h;
    const int step = std::max(1, static_cast<int>(npix / 200000));
    std::vector<float> sr, sg, sb;
    sr.reserve(npix / step + 1);
    sg.reserve(npix / step + 1);
    sb.reserve(npix / step + 1);

    for (size_t i = 0; i < npix; i += step) {
        float r = rgb16[i * 3 + 0] / 65535.0f;
        float g = rgb16[i * 3 + 1] / 65535.0f;
        float b = rgb16[i * 3 + 2] / 65535.0f;
        if (!is_linear) {
            r = std::pow(r, 2.2f);
            g = std::pow(g, 2.2f);
            b = std::pow(b, 2.2f);
        }
        float lum = 0.299f * r + 0.587f * g + 0.114f * b;
        if (lum > 0.004f && lum < 0.96f) {
            sr.push_back(std::max(r, 1e-4f));
            sg.push_back(std::max(g, 1e-4f));
            sb.push_back(std::max(b, 1e-4f));
        }
    }

    if (sr.size() < 200) return false;

    auto pct = [](std::vector<float>& v, float p) -> float {
        size_t idx = std::min(v.size() - 1, static_cast<size_t>(p * v.size()));
        std::nth_element(v.begin(), v.begin() + idx, v.end());
        return v[idx];
    };

    float base[3] = { pct(sr, 0.985f), pct(sg, 0.985f), pct(sb, 0.985f) };
    float dmax_i[3] = { pct(sr, 0.015f), pct(sg, 0.015f), pct(sb, 0.015f) };
    for (int c = 0; c < 3; ++c) {
        out_params[c] = std::max(base[c], 1e-4f);
        float d_hi = -std::log10(std::max(dmax_i[c], 1e-4f) / out_params[c]);
        out_params[3 + c] = std::max(0.25f, d_hi);
    }
    return true;
}

// Cheap optical-density negative-to-positive inversion on a uint16 RGB buffer -> uint8 RGB.
// invert_mode: 0=raw display gamma, 1=color negative, 2=b&w negative, 3=slide positive
// fixed_params (optional): [base_r, base_g, base_b, hi_r, hi_g, hi_b] for consistent tile rendering.
static void render_rgb8_preview(
    const uint16_t* rgb16,
    int w,
    int h,
    int invert_mode,
    bool is_linear,
    uint8_t* out_rgb8,
    const float* fixed_params = nullptr
) {
    const size_t npix = static_cast<size_t>(w) * h;

    if (invert_mode == 0 || invert_mode == 3) {
        const float inv_gamma = is_linear ? (1.0f / 2.2f) : 1.0f;
        uint8_t lut[65536];
        for (int i = 0; i < 65536; ++i) {
            float v = static_cast<float>(i) / 65535.0f;
            if (is_linear) v = std::pow(v, inv_gamma);
            if (invert_mode == 3) {
                v = clamp01((v - 0.01f) / 0.98f);
            }
            lut[i] = to_u8(v);
        }
        for (size_t i = 0; i < npix * 3; ++i) {
            out_rgb8[i] = lut[rgb16[i]];
        }
        return;
    }

    float params[6];
    if (fixed_params && fixed_params[0] > 0.0f && fixed_params[3] > 0.0f) {
        std::memcpy(params, fixed_params, sizeof(params));
    } else if (!estimate_inversion_params_from_rgb16(rgb16, w, h, is_linear, params)) {
        for (size_t i = 0; i < npix * 3; ++i) {
            float v = 1.0f - std::pow(rgb16[i] / 65535.0f, is_linear ? (1.0f / 2.2f) : 1.0f);
            out_rgb8[i] = to_u8(v);
        }
        return;
    }

    for (size_t i = 0; i < npix; ++i) {
        float outc[3];
        for (int c = 0; c < 3; ++c) {
            float v = rgb16[i * 3 + c] / 65535.0f;
            if (!is_linear) v = std::pow(v, 2.2f);
            v = std::max(v, 1e-4f);
            float d = -std::log10(v / params[c]);
            float t = clamp01(d / params[3 + c]);
            outc[c] = clamp01(std::pow(t, 0.85f));
        }
        if (invert_mode == 2) {
            float g = 0.299f * outc[0] + 0.587f * outc[1] + 0.114f * outc[2];
            out_rgb8[i * 3 + 0] = to_u8(g);
            out_rgb8[i * 3 + 1] = to_u8(g);
            out_rgb8[i * 3 + 2] = to_u8(g);
        } else {
            out_rgb8[i * 3 + 0] = to_u8(outc[0]);
            out_rgb8[i * 3 + 1] = to_u8(outc[1]);
            out_rgb8[i * 3 + 2] = to_u8(outc[2]);
        }
    }
}

static bool write_tiff_rgb16(
    const char* out_path,
    const uint16_t* rgb,
    uint32_t w,
    uint32_t h,
    float dpi_x,
    float dpi_y
) {
    std::string tmp_path = std::string(out_path) + ".tmp";
    TIFF* tif = TIFFOpen(tmp_path.c_str(), "w");
    if (!tif) return false;

    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, w);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, h);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 3);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 16);
    TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
    TIFFSetField(tif, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
    TIFFSetField(tif, TIFFTAG_XRESOLUTION, dpi_x);
    TIFFSetField(tif, TIFFTAG_YRESOLUTION, dpi_y);
    TIFFSetField(tif, TIFFTAG_SOFTWARE, "FilmCockpit 1.0");
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 16u);

    for (uint32_t y = 0; y < h; ++y) {
        if (TIFFWriteScanline(tif, const_cast<uint16_t*>(&rgb[static_cast<size_t>(y) * w * 3]), y, 0) < 0) {
            TIFFClose(tif);
            std::remove(tmp_path.c_str());
            return false;
        }
    }
    TIFFClose(tif);
    return std::rename(tmp_path.c_str(), out_path) == 0;
}

static void bilinear_crop_rotated(
    const uint16_t* src_rgb,
    const uint16_t* src_ir,
    int sw,
    int sh,
    const FrameSpec& f,
    int& ow,
    int& oh,
    std::vector<uint16_t>& dst_rgb,
    std::vector<uint16_t>& dst_ir
) {
    const double scale = std::max(0.1, 1.0 - 2.0 * (f.inset_pct / 100.0));
    const double fw = std::max(4.0, f.width * scale);
    const double fh = std::max(4.0, f.height * scale);
    ow = static_cast<int>(std::lround(fw));
    oh = static_cast<int>(std::lround(fh));

    dst_rgb.assign(static_cast<size_t>(ow) * oh * 3, 0);
    if (src_ir) dst_ir.assign(static_cast<size_t>(ow) * oh, 0);
    else dst_ir.clear();

    const double rad = f.angle_deg * (3.14159265358979323846 / 180.0);
    const double ca = std::cos(rad);
    const double sa = std::sin(rad);
    const double hx = (ow - 1) * 0.5;
    const double hy = (oh - 1) * 0.5;

    for (int y = 0; y < oh; ++y) {
        const double dy = y - hy;
        for (int x = 0; x < ow; ++x) {
            const double dx = x - hx;
            const double sx = f.cx + dx * ca - dy * sa;
            const double sy = f.cy + dx * sa + dy * ca;

            int x0 = static_cast<int>(std::floor(sx));
            int y0 = static_cast<int>(std::floor(sy));
            double fx = sx - x0;
            double fy = sy - y0;

            x0 = std::clamp(x0, 0, sw - 1);
            y0 = std::clamp(y0, 0, sh - 1);
            int x1 = std::min(sw - 1, x0 + 1);
            int y1 = std::min(sh - 1, y0 + 1);

            const double w00 = (1.0 - fx) * (1.0 - fy);
            const double w10 = fx * (1.0 - fy);
            const double w01 = (1.0 - fx) * fy;
            const double w11 = fx * fy;

            const size_t i00 = (static_cast<size_t>(y0) * sw + x0);
            const size_t i10 = (static_cast<size_t>(y0) * sw + x1);
            const size_t i01 = (static_cast<size_t>(y1) * sw + x0);
            const size_t i11 = (static_cast<size_t>(y1) * sw + x1);

            const size_t oidx = static_cast<size_t>(y) * ow + x;
            for (int c = 0; c < 3; ++c) {
                double val = w00 * src_rgb[i00 * 3 + c]
                           + w10 * src_rgb[i10 * 3 + c]
                           + w01 * src_rgb[i01 * 3 + c]
                           + w11 * src_rgb[i11 * 3 + c];
                dst_rgb[oidx * 3 + c] = static_cast<uint16_t>(std::clamp(std::lround(val), 0l, 65535l));
            }
            if (src_ir) {
                double iv = w00 * src_ir[i00] + w10 * src_ir[i10] + w01 * src_ir[i01] + w11 * src_ir[i11];
                dst_ir[oidx] = static_cast<uint16_t>(std::clamp(std::lround(iv), 0l, 65535l));
            }
        }
    }
}

} // namespace

extern "C" {

int fc_read_info(const char* path, TiffInfo* out_info) {
    if (!path || !out_info) return -1;
    std::memset(out_info, 0, sizeof(TiffInfo));
    TIFFSetWarningHandler(nullptr);
    TIFF* tif = TIFFOpen(path, "r");
    if (!tif) return -2;

    uint32_t w = 0, h = 0;
    uint16_t bps = 8, spp = 3, res_unit = RESUNIT_INCH;
    float dx = 0.0f, dy = 0.0f;
    char* sw = nullptr;

    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bps);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
    TIFFGetField(tif, TIFFTAG_XRESOLUTION, &dx);
    TIFFGetField(tif, TIFFTAG_YRESOLUTION, &dy);
    TIFFGetFieldDefaulted(tif, TIFFTAG_RESOLUTIONUNIT, &res_unit);
    if (TIFFGetField(tif, TIFFTAG_SOFTWARE, &sw) && sw) {
        std::snprintf(out_info->software, sizeof(out_info->software), "%s", sw);
    }

    if (res_unit == RESUNIT_CENTIMETER) { dx *= 2.54f; dy *= 2.54f; }
    if (dx <= 1.0f) dx = 300.0f;
    if (dy <= 1.0f) dy = dx;

    uint8_t has_ir = (spp >= 4) ? 1 : 0;
    while (!has_ir && TIFFReadDirectory(tif)) {
        uint32_t w2 = 0, h2 = 0;
        uint16_t spp2 = 1;
        TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w2);
        TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h2);
        TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &spp2);
        if (w2 == w && h2 == h && spp2 == 1) has_ir = 1;
    }

    TIFFClose(tif);

    out_info->width = w;
    out_info->height = h;
    out_info->dpi_x = dx;
    out_info->dpi_y = dy;
    out_info->bits_per_sample = bps;
    out_info->samples_per_pixel = spp;
    out_info->has_ir = has_ir;
    out_info->is_linear_hdr = (bps == 16) ? 1 : 0;
    return 0;
}

// Builds downscaled RGB8 preview (raw or inverted), plus optional IR8, IR dust mask8,
// and roll-wide inversion parameters out_inv_params[6] for consistent zoomed tile rendering.
int fc_build_preview(
    const char* path,
    int max_dim,
    int invert_mode,
    float ir_sensitivity,
    int* out_w,
    int* out_h,
    uint8_t* out_rgb8,
    uint8_t* out_ir8,
    uint8_t* out_mask8,
    float* out_inv_params
) {
    LoadedScan scan;
    if (!load_tiff(path, scan)) return -1;

    double scale = 1.0;
    int longest = std::max(scan.w, scan.h);
    if (max_dim > 0 && longest > max_dim) {
        scale = static_cast<double>(max_dim) / longest;
    }
    int pw = std::max(1, static_cast<int>(std::lround(scan.w * scale)));
    int ph = std::max(1, static_cast<int>(std::lround(scan.h * scale)));
    if (out_w) *out_w = pw;
    if (out_h) *out_h = ph;
    if (!out_rgb8) return 0;

    // Box-averaged 16-bit downscale for crisp, alias-free preview
    std::vector<uint16_t> small_rgb(static_cast<size_t>(pw) * ph * 3, 0);
    std::vector<uint16_t> small_ir;
    if (scan.has_ir) small_ir.assign(static_cast<size_t>(pw) * ph, 0);

    const double sx_step = static_cast<double>(scan.w) / pw;
    const double sy_step = static_cast<double>(scan.h) / ph;

    for (int py = 0; py < ph; ++py) {
        int y0 = static_cast<int>(py * sy_step);
        int y1 = std::min<int>(scan.h, std::max(y0 + 1, static_cast<int>((py + 1) * sy_step)));
        for (int px = 0; px < pw; ++px) {
            int x0 = static_cast<int>(px * sx_step);
            int x1 = std::min<int>(scan.w, std::max(x0 + 1, static_cast<int>((px + 1) * sx_step)));
            uint64_t sr = 0, sg = 0, sb = 0, si = 0;
            uint64_t cnt = 0;
            for (int y = y0; y < y1; ++y) {
                const size_t row = static_cast<size_t>(y) * scan.w;
                for (int x = x0; x < x1; ++x) {
                    const size_t idx = row + x;
                    sr += scan.rgb[idx * 3 + 0];
                    sg += scan.rgb[idx * 3 + 1];
                    sb += scan.rgb[idx * 3 + 2];
                    if (scan.has_ir) si += scan.ir[idx];
                    cnt++;
                }
            }
            const size_t pidx = static_cast<size_t>(py) * pw + px;
            small_rgb[pidx * 3 + 0] = static_cast<uint16_t>(sr / cnt);
            small_rgb[pidx * 3 + 1] = static_cast<uint16_t>(sg / cnt);
            small_rgb[pidx * 3 + 2] = static_cast<uint16_t>(sb / cnt);
            if (scan.has_ir) small_ir[pidx] = static_cast<uint16_t>(si / cnt);
        }
    }

    float inv_params[6] = {0};
    bool have_params = estimate_inversion_params_from_rgb16(small_rgb.data(), pw, ph, scan.is_linear_hdr, inv_params);
    if (out_inv_params) {
        if (have_params) std::memcpy(out_inv_params, inv_params, sizeof(inv_params));
        else std::memset(out_inv_params, 0, sizeof(inv_params));
    }

    render_rgb8_preview(small_rgb.data(), pw, ph, invert_mode, scan.is_linear_hdr, out_rgb8, have_params ? inv_params : nullptr);

    if (out_ir8) {
        if (scan.has_ir) {
            for (size_t i = 0; i < static_cast<size_t>(pw) * ph; ++i) {
                out_ir8[i] = static_cast<uint8_t>(small_ir[i] >> 8);
            }
        } else {
            std::memset(out_ir8, 0, static_cast<size_t>(pw) * ph);
        }
    }

    if (out_mask8) {
        if (scan.has_ir && ir_sensitivity > 0.001f) {
            std::vector<uint8_t> m;
            compute_ir_dust_mask(small_ir.data(), pw, ph, 12, ir_sensitivity, 1, m);
            std::memcpy(out_mask8, m.data(), m.size());
        } else {
            std::memset(out_mask8, 0, static_cast<size_t>(pw) * ph);
        }
    }

    return 0;
}

// Fast random-access scanline region reader for crisp 1:1 native zoom on multi-GB TIFFs.
// Reads ONLY rows [ry .. ry + rh) directly from libtiff without loading the full file.
int fc_read_region(
    const char* path,
    int rx,
    int ry,
    int rw,
    int rh,
    int out_w,
    int out_h,
    int invert_mode,
    const float* fixed_inv_params,
    float ir_sensitivity,
    uint8_t* out_rgb8,
    uint8_t* out_ir8,
    uint8_t* out_mask8
) {
    if (!path || rw <= 0 || rh <= 0 || out_w <= 0 || out_h <= 0) return -1;
    TIFFSetWarningHandler(nullptr);
    TIFF* tif = TIFFOpen(path, "r");
    if (!tif) return -2;

    uint32_t w = 0, h = 0;
    uint16_t bps = 8, spp = 3, planar = PLANARCONFIG_CONTIG;
    char* sw = nullptr;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bps);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
    TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &planar);
    std::string software;
    if (TIFFGetField(tif, TIFFTAG_SOFTWARE, &sw) && sw) software = sw;

    bool is_linear = (bps == 16) && (
        software.find("SilverFast") != std::string::npos ||
        software.find("VueScan") != std::string::npos
    );

    rx = std::clamp(rx, 0, std::max(0, static_cast<int>(w) - 1));
    ry = std::clamp(ry, 0, std::max(0, static_cast<int>(h) - 1));
    rw = std::clamp(rw, 1, static_cast<int>(w) - rx);
    rh = std::clamp(rh, 1, static_cast<int>(h) - ry);

    if (planar != PLANARCONFIG_CONTIG) {
        TIFFClose(tif);
        return -3;
    }

    const bool want_ir = (out_ir8 != nullptr || out_mask8 != nullptr);
    const bool ir_on_page0 = (spp >= 4);
    std::vector<uint16_t> reg_rgb(static_cast<size_t>(rw) * rh * 3, 0);
    std::vector<uint16_t> reg_ir;
    if (want_ir && ir_on_page0) reg_ir.assign(static_cast<size_t>(rw) * rh, 0);

    std::vector<uint8_t> rowbuf(static_cast<size_t>(TIFFScanlineSize(tif)));
    for (int y = 0; y < rh; ++y) {
        if (TIFFReadScanline(tif, rowbuf.data(), static_cast<uint32_t>(ry + y), 0) < 0) {
            TIFFClose(tif);
            return -4;
        }
        uint16_t* dst = &reg_rgb[static_cast<size_t>(y) * rw * 3];
        uint16_t* dst_ir = (want_ir && ir_on_page0) ? &reg_ir[static_cast<size_t>(y) * rw] : nullptr;
        if (bps == 16) {
            const uint16_t* src = reinterpret_cast<const uint16_t*>(rowbuf.data());
            for (int x = 0; x < rw; ++x) {
                int sx = rx + x;
                dst[x * 3 + 0] = src[sx * spp + 0];
                dst[x * 3 + 1] = src[sx * spp + (spp > 1 ? 1 : 0)];
                dst[x * 3 + 2] = src[sx * spp + (spp > 2 ? 2 : 0)];
                if (dst_ir) dst_ir[x] = src[sx * spp + 3];
            }
        } else {
            const uint8_t* src = rowbuf.data();
            for (int x = 0; x < rw; ++x) {
                int sx = rx + x;
                dst[x * 3 + 0] = static_cast<uint16_t>(src[sx * spp + 0]) * 257u;
                dst[x * 3 + 1] = static_cast<uint16_t>(src[sx * spp + (spp > 1 ? 1 : 0)]) * 257u;
                dst[x * 3 + 2] = static_cast<uint16_t>(src[sx * spp + (spp > 2 ? 2 : 0)]) * 257u;
                if (dst_ir) dst_ir[x] = static_cast<uint16_t>(src[sx * spp + 3]) * 257u;
            }
        }
    }

    bool has_ir = ir_on_page0;
    if (want_ir && !has_ir) {
        while (TIFFReadDirectory(tif)) {
            uint32_t w2 = 0, h2 = 0;
            uint16_t bps2 = 8, spp2 = 1;
            TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w2);
            TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h2);
            TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bps2);
            TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &spp2);
            if (w2 == w && h2 == h && spp2 == 1 && (bps2 == 16 || bps2 == 8)) {
                has_ir = true;
                reg_ir.assign(static_cast<size_t>(rw) * rh, 0);
                std::vector<uint8_t> ibuf(static_cast<size_t>(TIFFScanlineSize(tif)));
                for (int y = 0; y < rh; ++y) {
                    if (TIFFReadScanline(tif, ibuf.data(), static_cast<uint32_t>(ry + y), 0) < 0) {
                        has_ir = false;
                        reg_ir.clear();
                        break;
                    }
                    uint16_t* dst = &reg_ir[static_cast<size_t>(y) * rw];
                    if (bps2 == 16) {
                        const uint16_t* src = reinterpret_cast<const uint16_t*>(ibuf.data());
                        std::memcpy(dst, src + rx, static_cast<size_t>(rw) * sizeof(uint16_t));
                    } else {
                        for (int x = 0; x < rw; ++x) dst[x] = static_cast<uint16_t>(ibuf[rx + x]) * 257u;
                    }
                }
                break;
            }
        }
    }
    TIFFClose(tif);

    // Resample cropped region into (out_w, out_h)
    std::vector<uint16_t> out_rgb16(static_cast<size_t>(out_w) * out_h * 3, 0);
    std::vector<uint16_t> out_ir16;
    if (has_ir && want_ir) out_ir16.assign(static_cast<size_t>(out_w) * out_h, 0);

    const double sx_step = static_cast<double>(rw) / out_w;
    const double sy_step = static_cast<double>(rh) / out_h;
    for (int py = 0; py < out_h; ++py) {
        int y0 = static_cast<int>(py * sy_step);
        int y1 = std::min(rh, std::max(y0 + 1, static_cast<int>((py + 1) * sy_step)));
        for (int px = 0; px < out_w; ++px) {
            int x0 = static_cast<int>(px * sx_step);
            int x1 = std::min(rw, std::max(x0 + 1, static_cast<int>((px + 1) * sx_step)));
            uint64_t sr = 0, sg = 0, sb = 0, si = 0, cnt = 0;
            for (int y = y0; y < y1; ++y) {
                const size_t row = static_cast<size_t>(y) * rw;
                for (int x = x0; x < x1; ++x) {
                    const size_t idx = row + x;
                    sr += reg_rgb[idx * 3 + 0];
                    sg += reg_rgb[idx * 3 + 1];
                    sb += reg_rgb[idx * 3 + 2];
                    if (has_ir && want_ir) si += reg_ir[idx];
                    cnt++;
                }
            }
            const size_t pidx = static_cast<size_t>(py) * out_w + px;
            out_rgb16[pidx * 3 + 0] = static_cast<uint16_t>(sr / cnt);
            out_rgb16[pidx * 3 + 1] = static_cast<uint16_t>(sg / cnt);
            out_rgb16[pidx * 3 + 2] = static_cast<uint16_t>(sb / cnt);
            if (has_ir && want_ir) out_ir16[pidx] = static_cast<uint16_t>(si / cnt);
        }
    }

    if (out_rgb8) {
        render_rgb8_preview(out_rgb16.data(), out_w, out_h, invert_mode, is_linear, out_rgb8, fixed_inv_params);
    }
    if (out_ir8) {
        if (has_ir) {
            for (size_t i = 0; i < static_cast<size_t>(out_w) * out_h; ++i) {
                out_ir8[i] = static_cast<uint8_t>(out_ir16[i] >> 8);
            }
        } else {
            std::memset(out_ir8, 0, static_cast<size_t>(out_w) * out_h);
        }
    }
    if (out_mask8) {
        if (has_ir && ir_sensitivity > 0.001f) {
            std::vector<uint8_t> m;
            compute_ir_dust_mask(out_ir16.data(), out_w, out_h, 12, ir_sensitivity, 1, m, out_rgb16.data());
            std::memcpy(out_mask8, m.data(), m.size());
        } else {
            std::memset(out_mask8, 0, static_cast<size_t>(out_w) * out_h);
        }
    }
    return 0;
}

// Exports N cropped 16-bit TIFF frames from a single full-holder TIFF in one load pass.
// gamma_mode: 0 = preserve raw linear (1.0), 1 = encode gamma 2.2 (for standard editors)
// apply_ir_clean: 1 = detect dust from IR channel & harmonic-inpaint 16-bit RGB
// positive_mode: 0 = no positive preview export, 1 = color neg, 2 = b&w neg, 3 = slide
int fc_export_frames(
    const char* src_path,
    int num_frames,
    const FrameSpec* frames,
    const char** out_tiff_paths,
    const char** out_pos_ppm_paths, // optional per-frame positive RGB8 PPM for fast proof conversion
    int apply_ir_clean,
    float ir_sensitivity,
    int gamma_mode,
    int positive_mode
) {
    if (!src_path || num_frames <= 0 || !frames || !out_tiff_paths) return -1;
    LoadedScan scan;
    if (!load_tiff(src_path, scan)) return -2;

    // Precompute gamma 2.2 LUT if requested
    uint16_t gamma_lut[65536];
    if (gamma_mode == 1 && scan.is_linear_hdr) {
        for (int i = 0; i < 65536; ++i) {
            gamma_lut[i] = to_u16(std::pow(static_cast<float>(i) / 65535.0f, 1.0f / 2.2f));
        }
    }

    const int ir_radius = std::max(10, static_cast<int>(std::lround(scan.dpi_x / 80.0f)));
    // Tighter dilation now that RGB<->IR shift is auto-aligned (3px at 3200 DPI instead of 5px)
    const int ir_dilate = std::clamp(static_cast<int>(std::lround(scan.dpi_x / 1100.0f)), 1, 3);

    for (int i = 0; i < num_frames; ++i) {
        int ow = 0, oh = 0;
        std::vector<uint16_t> crop_rgb, crop_ir;
        bilinear_crop_rotated(
            scan.rgb.data(),
            (apply_ir_clean && scan.has_ir) ? scan.ir.data() : nullptr,
            static_cast<int>(scan.w),
            static_cast<int>(scan.h),
            frames[i],
            ow,
            oh,
            crop_rgb,
            crop_ir
        );

        if (apply_ir_clean && scan.has_ir && !crop_ir.empty()) {
            std::vector<uint8_t> dust_mask;
            compute_ir_dust_mask(crop_ir.data(), ow, oh, ir_radius, ir_sensitivity, ir_dilate, dust_mask, crop_rgb.data());
            inpaint_rgb16(crop_rgb.data(), dust_mask.data(), ow, oh, 64);
        }

        // Optionally export quick positive proof (PPM -> Python converts to JPEG cleanly)
        if (positive_mode > 0 && out_pos_ppm_paths && out_pos_ppm_paths[i] && out_pos_ppm_paths[i][0]) {
            std::vector<uint8_t> pos8(static_cast<size_t>(ow) * oh * 3);
            render_rgb8_preview(crop_rgb.data(), ow, oh, positive_mode, scan.is_linear_hdr, pos8.data());
            FILE* fp = std::fopen(out_pos_ppm_paths[i], "wb");
            if (fp) {
                std::fprintf(fp, "P6\n%d %d\n255\n", ow, oh);
                std::fwrite(pos8.data(), 1, pos8.size(), fp);
                std::fclose(fp);
            }
        }

        if (gamma_mode == 1 && scan.is_linear_hdr) {
            for (uint16_t& px : crop_rgb) px = gamma_lut[px];
        }

        if (!write_tiff_rgb16(out_tiff_paths[i], crop_rgb.data(), static_cast<uint32_t>(ow), static_cast<uint32_t>(oh), scan.dpi_x, scan.dpi_y)) {
            return -(100 + i);
        }
    }
    return 0;
}

// Streams N horizontal tiles side-by-side into one 64-bit BigTIFF (RGB16 + optional IR16).
// Uses ~2 MB RAM regardless of multi-GB output size.
int fc_stitch_horizontal_bigtiff(
    int num_tiles,
    const char** tile_paths,
    const char* out_path
) {
    if (num_tiles <= 0 || !tile_paths || !out_path) return -1;
    TIFFSetWarningHandler(nullptr);
    TIFFSetErrorHandler(nullptr);

    struct TileSrc {
        TIFF* tif = nullptr;
        uint32_t w = 0, h = 0;
        uint16_t bps = 16, spp = 3;
        int ir_dir = -1;
        uint16_t ir_bps = 16;
        std::vector<uint8_t> sbuf;
    };

    std::vector<TileSrc> tiles(num_tiles);
    float dpi_x = 3200.0f, dpi_y = 3200.0f;
    std::string software = "SilverFast HDRi (FilmCockpit Stitched)";
    uint32_t total_w = 0;
    uint32_t min_h = 0;
    bool all_have_ir = true;

    auto cleanup = [&]() {
        for (auto& t : tiles) {
            if (t.tif) { TIFFClose(t.tif); t.tif = nullptr; }
        }
    };

    for (int i = 0; i < num_tiles; ++i) {
        TIFF* tif = TIFFOpen(tile_paths[i], "r");
        if (!tif) { cleanup(); return -(10 + i); }
        TileSrc& ts = tiles[i];
        ts.tif = tif;
        TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &ts.w);
        TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &ts.h);
        TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &ts.bps);
        TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &ts.spp);
        if (i == 0) {
            float rx = 0, ry = 0;
            uint16_t resunit = RESUNIT_INCH;
            TIFFGetFieldDefaulted(tif, TIFFTAG_RESOLUTIONUNIT, &resunit);
            if (TIFFGetField(tif, TIFFTAG_XRESOLUTION, &rx) && rx > 1.0f) {
                dpi_x = (resunit == RESUNIT_CENTIMETER) ? rx * 2.54f : rx;
            }
            if (TIFFGetField(tif, TIFFTAG_YRESOLUTION, &ry) && ry > 1.0f) {
                dpi_y = (resunit == RESUNIT_CENTIMETER) ? ry * 2.54f : ry;
            }
            const char* sw = nullptr;
            if (TIFFGetField(tif, TIFFTAG_SOFTWARE, &sw) && sw && sw[0]) {
                software = std::string(sw) + " (FilmCockpit BigTIFF)";
            }
        }
        if (ts.w == 0 || ts.h == 0 || ts.spp < 3) { cleanup(); return -(20 + i); }
        total_w += ts.w;
        min_h = (i == 0) ? ts.h : std::min(min_h, ts.h);

        // Locate full-res IR directory if present
        int dir_idx = 0;
        while (TIFFReadDirectory(tif)) {
            dir_idx++;
            uint32_t w2 = 0, h2 = 0;
            uint16_t spp2 = 0, bps2 = 0;
            TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w2);
            TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h2);
            TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &spp2);
            TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bps2);
            if (w2 == ts.w && h2 == ts.h && spp2 == 1) {
                ts.ir_dir = dir_idx;
                ts.ir_bps = bps2;
                break;
            }
        }
        if (ts.ir_dir < 0) all_have_ir = false;
        TIFFSetDirectory(tif, 0);
        ts.sbuf.resize(static_cast<size_t>(TIFFScanlineSize(tif)));
    }

    std::string tmp_path = std::string(out_path) + ".tmp";
    TIFF* out = TIFFOpen(tmp_path.c_str(), "w8");
    if (!out) { cleanup(); return -30; }

    // Directory 0: 16-bit RGB
    TIFFSetField(out, TIFFTAG_IMAGEWIDTH, total_w);
    TIFFSetField(out, TIFFTAG_IMAGELENGTH, min_h);
    TIFFSetField(out, TIFFTAG_SAMPLESPERPIXEL, 3);
    TIFFSetField(out, TIFFTAG_BITSPERSAMPLE, 16);
    TIFFSetField(out, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(out, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(out, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    TIFFSetField(out, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
    TIFFSetField(out, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
    TIFFSetField(out, TIFFTAG_XRESOLUTION, dpi_x);
    TIFFSetField(out, TIFFTAG_YRESOLUTION, dpi_y);
    TIFFSetField(out, TIFFTAG_SOFTWARE, software.c_str());
    TIFFSetField(out, TIFFTAG_ROWSPERSTRIP, 1u);

    std::vector<uint16_t> out_rgb_row(static_cast<size_t>(total_w) * 3);
    for (uint32_t y = 0; y < min_h; ++y) {
        uint32_t x_off = 0;
        for (int i = 0; i < num_tiles; ++i) {
            TileSrc& ts = tiles[i];
            if (TIFFReadScanline(ts.tif, ts.sbuf.data(), y, 0) < 0) {
                TIFFClose(out); std::remove(tmp_path.c_str()); cleanup(); return -40;
            }
            uint16_t* dst = &out_rgb_row[static_cast<size_t>(x_off) * 3];
            if (ts.bps == 16) {
                const uint16_t* src = reinterpret_cast<const uint16_t*>(ts.sbuf.data());
                if (ts.spp == 3) {
                    std::memcpy(dst, src, static_cast<size_t>(ts.w) * 3 * sizeof(uint16_t));
                } else {
                    for (uint32_t x = 0; x < ts.w; ++x) {
                        dst[x * 3 + 0] = src[x * ts.spp + 0];
                        dst[x * 3 + 1] = src[x * ts.spp + 1];
                        dst[x * 3 + 2] = src[x * ts.spp + 2];
                    }
                }
            } else {
                for (uint32_t x = 0; x < ts.w; ++x) {
                    dst[x * 3 + 0] = static_cast<uint16_t>(ts.sbuf[x * ts.spp + 0]) * 257u;
                    dst[x * 3 + 1] = static_cast<uint16_t>(ts.sbuf[x * ts.spp + 1]) * 257u;
                    dst[x * 3 + 2] = static_cast<uint16_t>(ts.sbuf[x * ts.spp + 2]) * 257u;
                }
            }
            x_off += ts.w;
        }
        if (TIFFWriteScanline(out, out_rgb_row.data(), y, 0) < 0) {
            TIFFClose(out); std::remove(tmp_path.c_str()); cleanup(); return -41;
        }
    }

    // Directory 1: 16-bit IR (when all tiles contain IR)
    if (all_have_ir) {
        if (!TIFFWriteDirectory(out)) {
            TIFFClose(out); std::remove(tmp_path.c_str()); cleanup(); return -50;
        }
        for (int i = 0; i < num_tiles; ++i) {
            TileSrc& ts = tiles[i];
            TIFFSetDirectory(ts.tif, static_cast<uint16_t>(ts.ir_dir));
            ts.sbuf.resize(static_cast<size_t>(TIFFScanlineSize(ts.tif)));
        }
        TIFFSetField(out, TIFFTAG_SUBFILETYPE, 4);
        TIFFSetField(out, TIFFTAG_IMAGEWIDTH, total_w);
        TIFFSetField(out, TIFFTAG_IMAGELENGTH, min_h);
        TIFFSetField(out, TIFFTAG_SAMPLESPERPIXEL, 1);
        TIFFSetField(out, TIFFTAG_BITSPERSAMPLE, 16);
        TIFFSetField(out, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
        TIFFSetField(out, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(out, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
        TIFFSetField(out, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
        TIFFSetField(out, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
        TIFFSetField(out, TIFFTAG_XRESOLUTION, dpi_x);
        TIFFSetField(out, TIFFTAG_YRESOLUTION, dpi_y);
        TIFFSetField(out, TIFFTAG_ROWSPERSTRIP, 1u);

        std::vector<uint16_t> out_ir_row(total_w);
        for (uint32_t y = 0; y < min_h; ++y) {
            uint32_t x_off = 0;
            for (int i = 0; i < num_tiles; ++i) {
                TileSrc& ts = tiles[i];
                if (TIFFReadScanline(ts.tif, ts.sbuf.data(), y, 0) < 0) {
                    TIFFClose(out); std::remove(tmp_path.c_str()); cleanup(); return -51;
                }
                uint16_t* dst = &out_ir_row[x_off];
                if (ts.ir_bps == 16) {
                    std::memcpy(dst, ts.sbuf.data(), static_cast<size_t>(ts.w) * sizeof(uint16_t));
                } else {
                    for (uint32_t x = 0; x < ts.w; ++x) dst[x] = static_cast<uint16_t>(ts.sbuf[x]) * 257u;
                }
                x_off += ts.w;
            }
            if (TIFFWriteScanline(out, out_ir_row.data(), y, 0) < 0) {
                TIFFClose(out); std::remove(tmp_path.c_str()); cleanup(); return -52;
            }
        }
    }

    TIFFClose(out);
    cleanup();
    return std::rename(tmp_path.c_str(), out_path) == 0 ? 0 : -60;
}

} // extern "C"

// ============================================================================
// Film Cockpit V2 — Film Lab Color Engine + YCbCr Bilateral NR + Flatbed USM
// ============================================================================

static void apply_ycbcr_denoise_and_usm(
    std::vector<float>& rgb, // interleaved [H*W*3] in [0..1] display sRGB space
    int w,
    int h,
    float chroma_nr,
    float luma_nr,
    float sharpen,
    float res_scale // ~1.0 at preview (1000px), ~2.5 at full 3200 DPI frame
) {
    const size_t npix = static_cast<size_t>(w) * h;
    if (npix < 16) return;
    if (chroma_nr <= 0.001f && luma_nr <= 0.001f && sharpen <= 0.001f) return;

    std::vector<float> Y(npix), Cb(npix), Cr(npix);
    for (size_t i = 0; i < npix; ++i) {
        float r = rgb[i * 3 + 0], g = rgb[i * 3 + 1], b = rgb[i * 3 + 2];
        float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        Y[i]  = y;
        Cb[i] = (b - y) * 0.5389f;
        Cr[i] = (r - y) * 0.6350f;
    }

    // 1. Edge-preserving separable bilateral filter on Cb & Cr (stops strictly on Y edges!)
    if (chroma_nr > 0.005f) {
        int rad = std::clamp(static_cast<int>(std::lround((2.0f + 2.5f * chroma_nr) * std::min(1.8f, std::max(0.8f, res_scale)))), 2, 6);
        float inv_2sig_s = 1.0f / (2.0f * (rad * 0.65f) * (rad * 0.65f));
        float sig_y = 0.075f + 0.08f * chroma_nr;
        float inv_2sig_y = 1.0f / (2.0f * sig_y * sig_y);

        std::vector<float> tmp_cb(npix), tmp_cr(npix);
        // Horizontal pass
        for (int y = 0; y < h; ++y) {
            size_t row = static_cast<size_t>(y) * w;
            for (int x = 0; x < w; ++x) {
                float yc = Y[row + x];
                float scb = 0.0f, scr = 0.0f, sw = 0.0f;
                int x0 = std::max(0, x - rad), x1 = std::min(w - 1, x + rad);
                for (int xx = x0; xx <= x1; ++xx) {
                    float dx = static_cast<float>(xx - x);
                    float dy = Y[row + xx] - yc;
                    float wt = std::exp(-(dx * dx) * inv_2sig_s - (dy * dy) * inv_2sig_y);
                    scb += Cb[row + xx] * wt;
                    scr += Cr[row + xx] * wt;
                    sw += wt;
                }
                tmp_cb[row + x] = scb / sw;
                tmp_cr[row + x] = scr / sw;
            }
        }
        // Vertical pass
        for (int y = 0; y < h; ++y) {
            int y0 = std::max(0, y - rad), y1 = std::min(h - 1, y + rad);
            for (int x = 0; x < w; ++x) {
                size_t idx = static_cast<size_t>(y) * w + x;
                float yc = Y[idx];
                float scb = 0.0f, scr = 0.0f, sw = 0.0f;
                for (int yy = y0; yy <= y1; ++yy) {
                    size_t ni = static_cast<size_t>(yy) * w + x;
                    float dd = static_cast<float>(yy - y);
                    float dy = Y[ni] - yc;
                    float wt = std::exp(-(dd * dd) * inv_2sig_s - (dy * dy) * inv_2sig_y);
                    scb += tmp_cb[ni] * wt;
                    scr += tmp_cr[ni] * wt;
                    sw += wt;
                }
                float mix = std::clamp(chroma_nr * 1.15f, 0.0f, 1.0f);
                Cb[idx] = Cb[idx] * (1.0f - mix) + (scb / sw) * mix;
                Cr[idx] = Cr[idx] * (1.0f - mix) + (scr / sw) * mix;
            }
        }
    }

    // 2. Optional shadow-weighted edge-preserving bilateral luma NR (tames harsh shadow CCD noise)
    if (luma_nr > 0.005f) {
        int rad = std::clamp(static_cast<int>(std::lround((1.5f + 1.5f * luma_nr) * std::min(1.6f, std::max(0.8f, res_scale)))), 1, 4);
        float inv_2sig_s = 1.0f / (2.0f * (rad * 0.7f) * (rad * 0.7f));
        float sig_y = 0.032f + 0.055f * luma_nr;
        float inv_2sig_y = 1.0f / (2.0f * sig_y * sig_y);

        std::vector<float> tmp_y(npix), out_y(npix);
        for (int y = 0; y < h; ++y) {
            size_t row = static_cast<size_t>(y) * w;
            for (int x = 0; x < w; ++x) {
                float yc = Y[row + x];
                float sy = 0.0f, sw = 0.0f;
                int x0 = std::max(0, x - rad), x1 = std::min(w - 1, x + rad);
                for (int xx = x0; xx <= x1; ++xx) {
                    float dx = static_cast<float>(xx - x);
                    float dy = Y[row + xx] - yc;
                    float wt = std::exp(-(dx * dx) * inv_2sig_s - (dy * dy) * inv_2sig_y);
                    sy += Y[row + xx] * wt;
                    sw += wt;
                }
                tmp_y[row + x] = sy / sw;
            }
        }
        for (int y = 0; y < h; ++y) {
            int y0 = std::max(0, y - rad), y1 = std::min(h - 1, y + rad);
            for (int x = 0; x < w; ++x) {
                size_t idx = static_cast<size_t>(y) * w + x;
                float yc = Y[idx];
                float sy = 0.0f, sw = 0.0f;
                for (int yy = y0; yy <= y1; ++yy) {
                    size_t ni = static_cast<size_t>(yy) * w + x;
                    float dd = static_cast<float>(yy - y);
                    float dy = tmp_y[ni] - yc;
                    float wt = std::exp(-(dd * dd) * inv_2sig_s - (dy * dy) * inv_2sig_y);
                    sy += tmp_y[ni] * wt;
                    sw += wt;
                }
                float filt = sy / sw;
                // Weight stronger in dark shadows where flatbed CCD noise dominates
                float shadow_w = 0.30f + 0.70f * std::pow(std::clamp(1.0f - yc, 0.0f, 1.0f), 1.4f);
                float mix = std::clamp(luma_nr * shadow_w, 0.0f, 0.92f);
                out_y[idx] = yc * (1.0f - mix) + filt * mix;
            }
        }
        Y.swap(out_y);
    }

    // 3. Luminance-only thresholded Unsharp Mask (compensates V750 flatbed optical MTF without boosting grain)
    if (sharpen > 0.005f) {
        int r = std::clamp(static_cast<int>(std::lround(1.2f * std::min(2.0f, std::max(0.8f, res_scale)))), 1, 3);
        std::vector<float> blur_h(npix), blur_v(npix);
        for (int y = 0; y < h; ++y) {
            size_t row = static_cast<size_t>(y) * w;
            for (int x = 0; x < w; ++x) {
                float s = 0.0f; int cnt = 0;
                for (int xx = std::max(0, x - r); xx <= std::min(w - 1, x + r); ++xx) { s += Y[row + xx]; cnt++; }
                blur_h[row + x] = s / cnt;
            }
        }
        for (int y = 0; y < h; ++y) {
            int y0 = std::max(0, y - r), y1 = std::min(h - 1, y + r);
            for (int x = 0; x < w; ++x) {
                float s = 0.0f; int cnt = 0;
                for (int yy = y0; yy <= y1; ++yy) { s += blur_h[static_cast<size_t>(yy) * w + x]; cnt++; }
                blur_v[static_cast<size_t>(y) * w + x] = s / cnt;
            }
        }
        float amt = sharpen * 1.15f;
        float thresh = 0.010f;
        for (size_t i = 0; i < npix; ++i) {
            float detail = Y[i] - blur_v[i];
            float ad = std::fabs(detail);
            if (ad > thresh) {
                float soft = (ad - thresh) / ad;
                Y[i] = std::clamp(Y[i] + amt * detail * soft, 0.0f, 1.0f);
            }
        }
    }

    // Convert YCbCr back to RGB
    for (size_t i = 0; i < npix; ++i) {
        float y = Y[i], cb = Cb[i], cr = Cr[i];
        float r = y + 1.5748f * cr;
        float g = y - 0.1873f * cb - 0.4681f * cr;
        float b = y + 1.8556f * cb;
        rgb[i * 3 + 0] = std::clamp(r, 0.0f, 1.0f);
        rgb[i * 3 + 1] = std::clamp(g, 0.0f, 1.0f);
        rgb[i * 3 + 2] = std::clamp(b, 0.0f, 1.0f);
    }
}

// Physical Film Lab Engine: converts raw 16-bit scan frame into developed [0..1] display RGB float buffer.
static void develop_rgb16_to_float(
    const uint16_t* rgb16,
    int w,
    int h,
    int film_type, // 1=color_neg, 2=bw_neg, 3=slide
    bool is_linear,
    const DevelopParams& p,
    float res_scale,
    std::vector<float>& out_rgb
) {
    const size_t npix = static_cast<size_t>(w) * h;
    out_rgb.assign(npix * 3, 0.0f);
    if (!rgb16 || npix == 0) return;

    // Profile dye-unmixing matrices (row-sum = 1.0 so neutrals stay neutral!) + curve defaults
    struct ProfileSpec {
        float m[3][3];
        float grade_mul;
        float sat_mul;
        float warm_r;
        float warm_b;
        float black_floor;
    };
    static const ProfileSpec PROFILES[5] = {
        // 0: NATURAL
        { {{1.16f, -0.14f, -0.02f}, {-0.08f, 1.18f, -0.10f}, {-0.02f, -0.14f, 1.16f}}, 1.02f, 1.06f, 0.015f, -0.008f, 0.00f },
        // 1: FRONTIER (Fuji SP-3000 style — punchy greens, clean sky blues, warm skin, rich blacks)
        { {{1.24f, -0.20f, -0.04f}, {-0.09f, 1.24f, -0.15f}, {-0.03f, -0.18f, 1.21f}}, 1.12f, 1.15f, 0.028f, -0.015f, -0.015f },
        // 2: NORITSU (HS-1800 style — smooth open shadows, rich reds, gentle highlight shoulder)
        { {{1.22f, -0.16f, -0.06f}, {-0.10f, 1.20f, -0.10f}, {-0.02f, -0.15f, 1.17f}}, 1.05f, 1.11f, 0.022f, -0.010f, 0.01f },
        // 3: WARM (Portra-inspired golden warmth & soft pastel highlights)
        { {{1.18f, -0.13f, -0.05f}, {-0.08f, 1.17f, -0.09f}, {-0.04f, -0.14f, 1.18f}}, 0.96f, 1.05f, 0.045f, -0.028f, 0.015f },
        // 4: BW_CLASSIC (panchromatic monochrome)
        { {{1.00f,  0.00f,  0.00f}, { 0.00f, 1.00f,  0.00f}, { 0.00f,  0.00f, 1.00f}}, 1.10f, 0.00f, 0.00f,  0.00f, -0.01f },
    };
    const int prof_idx = std::clamp(p.profile, 0, 4);
    const ProfileSpec& prof = PROFILES[prof_idx];
    const bool is_bw = (film_type == 2 || prof_idx == 4);

    // 1. Determine film base D-min (true inter-frame orange rebate if provided; else 98.5th pct interior)
    float base[3] = { p.base_r, p.base_g, p.base_b };
    const int pad_x = std::max(2, w / 16);
    const int pad_y = std::max(2, h / 16);

    auto linearize = [is_linear](float v) -> float {
        return is_linear ? v : std::pow(std::max(v, 0.0f), 2.2f);
    };

    if (film_type != 3 && (base[0] <= 0.001f || base[1] <= 0.001f || base[2] <= 0.001f)) {
        std::vector<float> sr, sg, sb;
        int step = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(npix) / 35000.0)));
        for (int y = pad_y; y < h - pad_y; y += step) {
            for (int x = pad_x; x < w - pad_x; x += step) {
                size_t idx = static_cast<size_t>(y) * w + x;
                sr.push_back(std::max(1e-4f, linearize(rgb16[idx * 3 + 0] / 65535.0f)));
                sg.push_back(std::max(1e-4f, linearize(rgb16[idx * 3 + 1] / 65535.0f)));
                sb.push_back(std::max(1e-4f, linearize(rgb16[idx * 3 + 2] / 65535.0f)));
            }
        }
        auto pct = [](std::vector<float>& v, float q) -> float {
            if (v.empty()) return 0.5f;
            size_t i = std::min(v.size() - 1, static_cast<size_t>(q * v.size()));
            std::nth_element(v.begin(), v.begin() + i, v.end());
            return v[i];
        };
        base[0] = std::max(0.01f, pct(sr, 0.985f));
        base[1] = std::max(0.01f, pct(sg, 0.985f));
        base[2] = std::max(0.01f, pct(sb, 0.985f));
    }

    // 2. Convert to optical density (for negatives) or log transmission (for slides) & apply 3x3 dye unmixing
    std::vector<float> dens(npix * 3);
    if (film_type == 3) {
        // Slide positive: log10 linear transmission
        for (size_t i = 0; i < npix; ++i) {
            float r = std::max(1e-4f, linearize(rgb16[i * 3 + 0] / 65535.0f));
            float g = std::max(1e-4f, linearize(rgb16[i * 3 + 1] / 65535.0f));
            float b = std::max(1e-4f, linearize(rgb16[i * 3 + 2] / 65535.0f));
            dens[i * 3 + 0] = std::log10(r);
            dens[i * 3 + 1] = std::log10(g);
            dens[i * 3 + 2] = std::log10(b);
        }
    } else {
        for (size_t i = 0; i < npix; ++i) {
            float r = std::max(1e-4f, linearize(rgb16[i * 3 + 0] / 65535.0f));
            float g = std::max(1e-4f, linearize(rgb16[i * 3 + 1] / 65535.0f));
            float b = std::max(1e-4f, linearize(rgb16[i * 3 + 2] / 65535.0f));
            float dr = std::max(0.0f, -std::log10(r / base[0]));
            float dg = std::max(0.0f, -std::log10(g / base[1]));
            float db = std::max(0.0f, -std::log10(b / base[2]));
            if (is_bw) {
                float dl = 0.299f * dr + 0.587f * dg + 0.114f * db;
                dens[i * 3 + 0] = dens[i * 3 + 1] = dens[i * 3 + 2] = dl;
            } else {
                // 3x3 Beer-Lambert spectral dye unmixing
                dens[i * 3 + 0] = prof.m[0][0] * dr + prof.m[0][1] * dg + prof.m[0][2] * db;
                dens[i * 3 + 1] = prof.m[1][0] * dr + prof.m[1][1] * dg + prof.m[1][2] * db;
                dens[i * 3 + 2] = prof.m[2][0] * dr + prof.m[2][1] * dg + prof.m[2][2] * db;
            }
        }
    }

    // 3. Measure interior frame density range [p01..p99] & Shades-of-Gray illuminant balance
    std::vector<float> dr_s, dg_s, db_s, dl_s;
    int step = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(npix) / 40000.0)));
    for (int y = pad_y; y < h - pad_y; y += step) {
        for (int x = pad_x; x < w - pad_x; x += step) {
            size_t idx = static_cast<size_t>(y) * w + x;
            float r = dens[idx * 3 + 0], g = dens[idx * 3 + 1], b = dens[idx * 3 + 2];
            dr_s.push_back(r);
            dg_s.push_back(g);
            db_s.push_back(b);
            dl_s.push_back(0.299f * r + 0.587f * g + 0.114f * b);
        }
    }
    auto pct = [](std::vector<float>& v, float q) -> float {
        if (v.empty()) return 0.0f;
        size_t i = std::min(v.size() - 1, static_cast<size_t>(q * v.size()));
        std::nth_element(v.begin(), v.begin() + i, v.end());
        return v[i];
    };

    float lo[3] = { pct(dr_s, 0.012f), pct(dg_s, 0.012f), pct(db_s, 0.012f) };
    float hi[3] = { pct(dr_s, 0.990f), pct(dg_s, 0.990f), pct(db_s, 0.990f) };
    float l_lo = pct(dl_s, 0.012f);
    float l_hi = pct(dl_s, 0.990f);
    float l_span = std::max(0.32f, l_hi - l_lo);

    float black_pt[3], span[3];
    for (int c = 0; c < 3; ++c) {
        float c_span = std::max(0.28f, hi[c] - lo[c]);
        if (p.auto_wb && !is_bw) {
            // Blend 86% per-channel neutral balance + 14% common luminance anchor
            // -> neutral sunlit whites while preserving real sunset/neon mood!
            black_pt[c] = 0.86f * lo[c] + 0.14f * l_lo;
            float white_pt = 0.86f * hi[c] + 0.14f * (black_pt[c] + l_span);
            span[c] = std::max(0.30f, white_pt - black_pt[c]);
        } else {
            // Preserve raw film-base color balance (manual temp/tint only)
            black_pt[c] = l_lo;
            span[c] = l_span;
        }
    }

    // 4. Apply enlarger exposure, dichroic Temp/Tint, blacks, RA-4 S-curve, and saturation
    const float exp_shift = p.exposure * 0.24f;
    const float grade = std::clamp(p.contrast * prof.grade_mul, 0.50f, 1.90f);
    const float black_shift = p.blacks * 0.35f + prof.black_floor;
    const float temp_r =  p.temp * 0.14f + prof.warm_r;
    const float temp_b = -p.temp * 0.14f + prof.warm_b;
    const float tint_g = -p.tint * 0.12f;
    const float sat = is_bw ? 0.0f : std::clamp(p.saturation * prof.sat_mul, 0.0f, 2.2f);

    // Smooth photographic RA-4 print curve: sigmoid toe + gentle highlight shoulder
    auto ra4_curve = [grade](float x) -> float {
        float xc = std::clamp(x, 0.0f, 1.35f);
        // Soft highlight roll-off above 0.80 so bright skies/lamps keep detail like print paper
        if (xc > 0.78f) {
            float over = xc - 0.78f;
            xc = 0.78f + (over * 0.22f) / (over + 0.22f);
        }
        // Subtle photographic S-curve centered at 0.46 midgray
        float c = std::clamp(xc, 0.0f, 1.0f);
        float s = c * c * (3.0f - 2.0f * c); // smoothstep S
        float blended = c + (s - c) * std::clamp((grade - 0.72f) * 0.55f, -0.25f, 0.65f);
        // Contrast pivot around midtone 0.45
        float out = 0.45f + (blended - 0.45f) * (0.65f + 0.35f * grade);
        return std::clamp(out, 0.0f, 1.0f);
    };

    for (size_t i = 0; i < npix; ++i) {
        float r = (dens[i * 3 + 0] - black_pt[0]) / span[0] + exp_shift + black_shift + temp_r;
        float g = (dens[i * 3 + 1] - black_pt[1]) / span[1] + exp_shift + black_shift + tint_g;
        float b = (dens[i * 3 + 2] - black_pt[2]) / span[2] + exp_shift + black_shift + temp_b;

        r = ra4_curve(r);
        g = ra4_curve(g);
        b = ra4_curve(b);

        if (is_bw) {
            float y = 0.299f * r + 0.587f * g + 0.114f * b;
            out_rgb[i * 3 + 0] = out_rgb[i * 3 + 1] = out_rgb[i * 3 + 2] = y;
        } else if (std::fabs(sat - 1.0f) > 0.001f) {
            float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            // Subtle vibrance-like protection near pure white/black so shadows don't turn neon
            float chroma_guard = 1.0f - 0.35f * std::pow(std::fabs(2.0f * y - 1.0f), 2.0f);
            float eff_sat = 1.0f + (sat - 1.0f) * chroma_guard;
            out_rgb[i * 3 + 0] = std::clamp(y + (r - y) * eff_sat, 0.0f, 1.0f);
            out_rgb[i * 3 + 1] = std::clamp(y + (g - y) * eff_sat, 0.0f, 1.0f);
            out_rgb[i * 3 + 2] = std::clamp(y + (b - y) * eff_sat, 0.0f, 1.0f);
        } else {
            out_rgb[i * 3 + 0] = r;
            out_rgb[i * 3 + 1] = g;
            out_rgb[i * 3 + 2] = b;
        }
    }

    // 5. YCbCr Chroma/Luma Bilateral NR + Flatbed USM
    apply_ycbcr_denoise_and_usm(out_rgb, w, h, p.chroma_nr, p.luma_nr, p.sharpen, res_scale);
}

extern "C" {

// Samples the true unexposed orange film base (D-min) directly from N inter-frame rebate boxes [x, y, w, h].
// Uses fast random-access TIFFReadScanline (<15 ms even on a 5 GB BigTIFF).
int fc_sample_film_base(
    const char* path,
    int num_boxes,
    const int* boxes_xywh, // 4 ints per box: x, y, w, h
    float* out_base_rgb    // [3] linear 0..1
) {
    if (!path || num_boxes <= 0 || !boxes_xywh || !out_base_rgb) return -1;
    TIFFSetWarningHandler(nullptr);
    TIFFSetErrorHandler(nullptr);
    TIFF* tif = TIFFOpen(path, "r");
    if (!tif) return -2;

    uint32_t sw = 0, sh = 0;
    uint16_t bps = 16, spp = 3;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &sw);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &sh);
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bps);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
    const char* soft = nullptr;
    bool is_linear = false;
    if (TIFFGetField(tif, TIFFTAG_SOFTWARE, &soft) && soft) {
        std::string s(soft);
        if (s.find("SilverFast") != std::string::npos || s.find("HDR") != std::string::npos || s.find("VueScan") != std::string::npos) {
            is_linear = (bps == 16);
        }
    }

    std::vector<uint8_t> sbuf(static_cast<size_t>(TIFFScanlineSize(tif)));
    std::vector<float> sr, sg, sb;
    sr.reserve(8192); sg.reserve(8192); sb.reserve(8192);

    for (int bi = 0; bi < num_boxes; ++bi) {
        int rx = std::clamp(boxes_xywh[bi * 4 + 0], 0, static_cast<int>(sw) - 1);
        int ry = std::clamp(boxes_xywh[bi * 4 + 1], 0, static_cast<int>(sh) - 1);
        int rw = std::clamp(boxes_xywh[bi * 4 + 2], 1, static_cast<int>(sw) - rx);
        int rh = std::clamp(boxes_xywh[bi * 4 + 3], 1, static_cast<int>(sh) - ry);
        int ystep = std::max(1, rh / 8);
        int xstep = std::max(1, rw / 32);
        for (int y = ry; y < ry + rh; y += ystep) {
            if (TIFFReadScanline(tif, sbuf.data(), static_cast<uint32_t>(y), 0) < 0) continue;
            for (int x = rx; x < rx + rw; x += xstep) {
                float r, g, b;
                if (bps == 16) {
                    const uint16_t* p16 = reinterpret_cast<const uint16_t*>(sbuf.data());
                    r = p16[x * spp + 0] / 65535.0f;
                    g = p16[x * spp + 1] / 65535.0f;
                    b = p16[x * spp + 2] / 65535.0f;
                } else {
                    r = sbuf[x * spp + 0] / 255.0f;
                    g = sbuf[x * spp + 1] / 255.0f;
                    b = sbuf[x * spp + 2] / 255.0f;
                }
                if (!is_linear) {
                    r = std::pow(r, 2.2f);
                    g = std::pow(g, 2.2f);
                    b = std::pow(b, 2.2f);
                }
                float lum = 0.299f * r + 0.587f * g + 0.114f * b;
                // Exclude opaque black holder plastic and blown-out bare white glass
                if (lum > 0.03f && lum < 0.92f) {
                    sr.push_back(r);
                    sg.push_back(g);
                    sb.push_back(b);
                }
            }
        }
    }
    TIFFClose(tif);
    if (sr.size() < 20) return -3;

    auto med = [](std::vector<float>& v) -> float {
        size_t i = v.size() / 2;
        std::nth_element(v.begin(), v.begin() + i, v.end());
        return v[i];
    };
    out_base_rgb[0] = med(sr);
    out_base_rgb[1] = med(sg);
    out_base_rgb[2] = med(sb);
    return 0;
}

// Fast random-access live preview of a single developed frame (supports rotation, V2 IR clean,
// Color Lab profiles, WB, YCbCr denoise, USM) for real-time [DEVELOP] tab sliders!
int fc_develop_frame_preview(
    const char* src_path,
    const FrameSpec* frame,
    const DevelopParams* dev,
    int max_dim,
    int apply_ir_clean,
    float ir_sensitivity,
    int film_type, // 1=color_neg, 2=bw_neg, 3=slide
    int* out_w,
    int* out_h,
    uint8_t* out_rgb8 // caller allocates max_dim * max_dim * 3
) {
    if (!src_path || !frame || !dev || !out_w || !out_h || !out_rgb8) return -1;
    TIFFSetWarningHandler(nullptr);
    TIFFSetErrorHandler(nullptr);
    TIFF* tif = TIFFOpen(src_path, "r");
    if (!tif) return -2;

    uint32_t sw = 0, sh = 0;
    uint16_t bps = 16, spp = 3;
    float dpi_x = 300.0f;
    uint16_t resunit = RESUNIT_INCH;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &sw);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &sh);
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bps);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
    TIFFGetFieldDefaulted(tif, TIFFTAG_RESOLUTIONUNIT, &resunit);
    if (TIFFGetField(tif, TIFFTAG_XRESOLUTION, &dpi_x) && resunit == RESUNIT_CENTIMETER) dpi_x *= 2.54f;
    const char* soft = nullptr;
    bool is_linear = false;
    if (TIFFGetField(tif, TIFFTAG_SOFTWARE, &soft) && soft) {
        std::string s(soft);
        if (s.find("SilverFast") != std::string::npos || s.find("HDR") != std::string::npos || s.find("VueScan") != std::string::npos) {
            is_linear = (bps == 16);
        }
    }

    // Compute axis-aligned bounding box of rotated frame in scan coordinates
    const double scale = std::max(0.1, 1.0 - 2.0 * (frame->inset_pct / 100.0));
    const double fw = std::max(4.0, frame->width * scale);
    const double fh = std::max(4.0, frame->height * scale);
    const double rad = frame->angle_deg * (3.14159265358979323846 / 180.0);
    const double ca = std::fabs(std::cos(rad)), sa = std::fabs(std::sin(rad));
    const double bbox_w = fw * ca + fh * sa;
    const double bbox_h = fw * sa + fh * ca;

    int rx = std::clamp(static_cast<int>(std::floor(frame->cx - bbox_w * 0.5)) - 4, 0, static_cast<int>(sw) - 1);
    int ry = std::clamp(static_cast<int>(std::floor(frame->cy - bbox_h * 0.5)) - 4, 0, static_cast<int>(sh) - 1);
    int rx1 = std::clamp(static_cast<int>(std::ceil(frame->cx + bbox_w * 0.5)) + 4, rx + 1, static_cast<int>(sw));
    int ry1 = std::clamp(static_cast<int>(std::ceil(frame->cy + bbox_h * 0.5)) + 4, ry + 1, static_cast<int>(sh));
    int rw = rx1 - rx;
    int rh = ry1 - ry;

    // Read only the bounding scanlines via O(1) random-access TIFFReadScanline!
    std::vector<uint16_t> reg_rgb(static_cast<size_t>(rw) * rh * 3, 0);
    std::vector<uint16_t> reg_ir;
    bool has_ir = false;

    std::vector<uint8_t> sbuf(static_cast<size_t>(TIFFScanlineSize(tif)));
    for (int y = 0; y < rh; ++y) {
        if (TIFFReadScanline(tif, sbuf.data(), static_cast<uint32_t>(ry + y), 0) < 0) continue;
        uint16_t* dst = &reg_rgb[static_cast<size_t>(y) * rw * 3];
        if (bps == 16) {
            const uint16_t* src = reinterpret_cast<const uint16_t*>(sbuf.data());
            if (spp == 3) {
                std::memcpy(dst, src + static_cast<size_t>(rx) * 3, static_cast<size_t>(rw) * 3 * sizeof(uint16_t));
            } else {
                for (int x = 0; x < rw; ++x) {
                    dst[x * 3 + 0] = src[(rx + x) * spp + 0];
                    dst[x * 3 + 1] = src[(rx + x) * spp + 1];
                    dst[x * 3 + 2] = src[(rx + x) * spp + 2];
                }
            }
        } else {
            for (int x = 0; x < rw; ++x) {
                dst[x * 3 + 0] = static_cast<uint16_t>(sbuf[(rx + x) * spp + 0]) * 257u;
                dst[x * 3 + 1] = static_cast<uint16_t>(sbuf[(rx + x) * spp + 1]) * 257u;
                dst[x * 3 + 2] = static_cast<uint16_t>(sbuf[(rx + x) * spp + 2]) * 257u;
            }
        }
    }

    if (apply_ir_clean) {
        while (TIFFReadDirectory(tif)) {
            uint32_t w2 = 0, h2 = 0;
            uint16_t spp2 = 0, bps2 = 0;
            TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w2);
            TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h2);
            TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &spp2);
            TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bps2);
            if (w2 == sw && h2 == sh && spp2 == 1) {
                has_ir = true;
                reg_ir.assign(static_cast<size_t>(rw) * rh, 0);
                std::vector<uint8_t> ibuf(static_cast<size_t>(TIFFScanlineSize(tif)));
                for (int y = 0; y < rh; ++y) {
                    if (TIFFReadScanline(tif, ibuf.data(), static_cast<uint32_t>(ry + y), 0) < 0) continue;
                    uint16_t* dst = &reg_ir[static_cast<size_t>(y) * rw];
                    if (bps2 == 16) {
                        std::memcpy(dst, reinterpret_cast<const uint16_t*>(ibuf.data()) + rx, static_cast<size_t>(rw) * sizeof(uint16_t));
                    } else {
                        for (int x = 0; x < rw; ++x) dst[x] = static_cast<uint16_t>(ibuf[rx + x]) * 257u;
                    }
                }
                break;
            }
        }
    }
    TIFFClose(tif);

    // Compute downscaled preview dimensions
    double down = 1.0;
    double longest = std::max(fw, fh);
    if (max_dim > 0 && longest > max_dim) down = static_cast<double>(max_dim) / longest;
    int pw = std::max(8, static_cast<int>(std::lround(fw * down)));
    int ph = std::max(8, static_cast<int>(std::lround(fh * down)));
    *out_w = pw;
    *out_h = ph;

    // Sample rotated frame directly into preview resolution
    std::vector<uint16_t> crop_rgb(static_cast<size_t>(pw) * ph * 3, 0);
    std::vector<uint16_t> crop_ir;
    if (has_ir) crop_ir.assign(static_cast<size_t>(pw) * ph, 0);

    const double c_ang = std::cos(rad), s_ang = std::sin(rad);
    const double lcx = frame->cx - rx;
    const double lcy = frame->cy - ry;
    const double step_src = 1.0 / down;
    const double hx = (pw - 1) * 0.5;
    const double hy = (ph - 1) * 0.5;

    for (int y = 0; y < ph; ++y) {
        double dy = (y - hy) * step_src;
        for (int x = 0; x < pw; ++x) {
            double dx = (x - hx) * step_src;
            double sx = lcx + dx * c_ang - dy * s_ang;
            double sy = lcy + dx * s_ang + dy * c_ang;
            int ix = std::clamp(static_cast<int>(std::floor(sx)), 0, rw - 1);
            int iy = std::clamp(static_cast<int>(std::floor(sy)), 0, rh - 1);
            int ix1 = std::min(rw - 1, ix + 1);
            int iy1 = std::min(rh - 1, iy + 1);
            double fx = sx - ix, fy = sy - iy;
            double w00 = (1.0 - fx) * (1.0 - fy), w10 = fx * (1.0 - fy), w01 = (1.0 - fx) * fy, w11 = fx * fy;
            size_t i00 = static_cast<size_t>(iy) * rw + ix;
            size_t i10 = static_cast<size_t>(iy) * rw + ix1;
            size_t i01 = static_cast<size_t>(iy1) * rw + ix;
            size_t i11 = static_cast<size_t>(iy1) * rw + ix1;
            size_t dst = static_cast<size_t>(y) * pw + x;
            for (int c = 0; c < 3; ++c) {
                double val = w00 * reg_rgb[i00 * 3 + c] + w10 * reg_rgb[i10 * 3 + c]
                           + w01 * reg_rgb[i01 * 3 + c] + w11 * reg_rgb[i11 * 3 + c];
                crop_rgb[dst * 3 + c] = static_cast<uint16_t>(std::clamp(std::lround(val), 0l, 65535l));
            }
            if (has_ir) {
                double iv = w00 * reg_ir[i00] + w10 * reg_ir[i10] + w01 * reg_ir[i01] + w11 * reg_ir[i11];
                crop_ir[dst] = static_cast<uint16_t>(std::clamp(std::lround(iv), 0l, 65535l));
            }
        }
    }

    if (apply_ir_clean && has_ir && !crop_ir.empty()) {
        int eff_rad = std::max(8, static_cast<int>(std::lround((dpi_x * down) / 80.0)));
        int eff_dil = std::clamp(static_cast<int>(std::lround((dpi_x * down) / 1100.0)), 1, 2);
        std::vector<uint8_t> dmask;
        compute_ir_dust_mask(crop_ir.data(), pw, ph, eff_rad, ir_sensitivity, eff_dil, dmask, crop_rgb.data());
        inpaint_rgb16(crop_rgb.data(), dmask.data(), pw, ph, 48);
    }

    if (dev->enabled) {
        std::vector<float> f_rgb;
        develop_rgb16_to_float(crop_rgb.data(), pw, ph, film_type, is_linear, *dev, 1.0f, f_rgb);
        for (size_t i = 0; i < static_cast<size_t>(pw) * ph * 3; ++i) {
            out_rgb8[i] = static_cast<uint8_t>(std::clamp(std::lround(f_rgb[i] * 255.0f), 0l, 255l));
        }
    } else {
        render_rgb8_preview(crop_rgb.data(), pw, ph, film_type, is_linear, out_rgb8);
    }
    return 0;
}

// V2 full-resolution 16-bit batch exporter supporting per-frame DevelopParams:
//  - Optional 16-bit Linear/Gamma RAW TIFF (out_raw_tiff_paths[i])
//  - Optional 16-bit Developed Positive TIFF (out_dev_tiff_paths[i])
//  - Optional 8-bit Developed Positive PPM for JPEG conversion (out_jpg_ppm_paths[i])
int fc_export_frames_v2(
    const char* src_path,
    int num_frames,
    const FrameSpec* frames,
    const DevelopParams* dev_params, // array of length num_frames
    const char** out_raw_tiff_paths, // nullable entries
    const char** out_dev_tiff_paths, // nullable entries
    const char** out_jpg_ppm_paths,  // nullable entries
    int apply_ir_clean,
    float ir_sensitivity,
    int gamma_mode,
    int film_type // 1=color_neg, 2=bw_neg, 3=slide
) {
    if (!src_path || num_frames <= 0 || !frames || !dev_params) return -1;
    LoadedScan scan;
    if (!load_tiff(src_path, scan)) return -2;

    uint16_t gamma_lut[65536];
    if (gamma_mode == 1 && scan.is_linear_hdr) {
        for (int i = 0; i < 65536; ++i) {
            gamma_lut[i] = to_u16(std::pow(static_cast<float>(i) / 65535.0f, 1.0f / 2.2f));
        }
    }

    const int ir_radius = std::max(10, static_cast<int>(std::lround(scan.dpi_x / 80.0f)));
    const int ir_dilate = std::clamp(static_cast<int>(std::lround(scan.dpi_x / 1100.0f)), 1, 3);
    const float res_scale = std::clamp(scan.dpi_x / 1600.0f, 0.7f, 2.2f);

    for (int i = 0; i < num_frames; ++i) {
        int ow = 0, oh = 0;
        std::vector<uint16_t> crop_rgb, crop_ir;
        bilinear_crop_rotated(
            scan.rgb.data(),
            (apply_ir_clean && scan.has_ir) ? scan.ir.data() : nullptr,
            static_cast<int>(scan.w),
            static_cast<int>(scan.h),
            frames[i],
            ow,
            oh,
            crop_rgb,
            crop_ir
        );

        if (apply_ir_clean && scan.has_ir && !crop_ir.empty()) {
            std::vector<uint8_t> dust_mask;
            compute_ir_dust_mask(crop_ir.data(), ow, oh, ir_radius, ir_sensitivity, ir_dilate, dust_mask, crop_rgb.data());
            inpaint_rgb16(crop_rgb.data(), dust_mask.data(), ow, oh, 64);
        }

        const bool want_dev_tiff = (out_dev_tiff_paths && out_dev_tiff_paths[i] && out_dev_tiff_paths[i][0]);
        const bool want_jpg_ppm  = (out_jpg_ppm_paths  && out_jpg_ppm_paths[i]  && out_jpg_ppm_paths[i][0]);
        const bool want_raw_tiff = (out_raw_tiff_paths && out_raw_tiff_paths[i] && out_raw_tiff_paths[i][0]);

        if (want_dev_tiff || want_jpg_ppm) {
            std::vector<float> dev_f;
            if (dev_params[i].enabled) {
                develop_rgb16_to_float(crop_rgb.data(), ow, oh, film_type, scan.is_linear_hdr, dev_params[i], res_scale, dev_f);
            } else {
                std::vector<uint8_t> tmp8(static_cast<size_t>(ow) * oh * 3);
                render_rgb8_preview(crop_rgb.data(), ow, oh, film_type, scan.is_linear_hdr, tmp8.data());
                dev_f.resize(tmp8.size());
                for (size_t k = 0; k < tmp8.size(); ++k) dev_f[k] = tmp8[k] / 255.0f;
            }

            if (want_dev_tiff) {
                std::vector<uint16_t> dev16(dev_f.size());
                for (size_t k = 0; k < dev_f.size(); ++k) dev16[k] = to_u16(dev_f[k]);
                if (!write_tiff_rgb16(out_dev_tiff_paths[i], dev16.data(), static_cast<uint32_t>(ow), static_cast<uint32_t>(oh), scan.dpi_x, scan.dpi_y)) {
                    return -(200 + i);
                }
            }
            if (want_jpg_ppm) {
                std::vector<uint8_t> dev8(dev_f.size());
                for (size_t k = 0; k < dev_f.size(); ++k) {
                    dev8[k] = static_cast<uint8_t>(std::clamp(std::lround(dev_f[k] * 255.0f), 0l, 255l));
                }
                FILE* fp = std::fopen(out_jpg_ppm_paths[i], "wb");
                if (fp) {
                    std::fprintf(fp, "P6\n%d %d\n255\n", ow, oh);
                    std::fwrite(dev8.data(), 1, dev8.size(), fp);
                    std::fclose(fp);
                }
            }
        }

        if (want_raw_tiff) {
            if (gamma_mode == 1 && scan.is_linear_hdr) {
                for (uint16_t& px : crop_rgb) px = gamma_lut[px];
            }
            if (!write_tiff_rgb16(out_raw_tiff_paths[i], crop_rgb.data(), static_cast<uint32_t>(ow), static_cast<uint32_t>(oh), scan.dpi_x, scan.dpi_y)) {
                return -(100 + i);
            }
        }
    }
    return 0;
}

} // extern "C"
