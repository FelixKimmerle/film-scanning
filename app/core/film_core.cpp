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
static void compute_ir_dust_mask(
    const uint16_t* ir,
    int w,
    int h,
    int radius,
    float sensitivity, // 0.0 .. 1.0 (default ~0.5)
    int dilate_px,
    std::vector<uint8_t>& mask_out
) {
    const size_t npix = static_cast<size_t>(w) * h;
    mask_out.assign(npix, 0);
    if (!ir || w < 8 || h < 8 || sensitivity <= 0.001f) return;

    // Build integral images of IR values and illuminated-pixel counts
    // so pitch-black opaque plastic film holder bars never trigger false dust hits.
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
            // Only evaluate pixels surrounded by illuminated film (not plastic holder dividers)
            if (vcnt < area * 0.85) continue;

            double sum = integ[static_cast<size_t>(y1) * iw + x1]
                       - integ[static_cast<size_t>(y0) * iw + x1]
                       - integ[static_cast<size_t>(y1) * iw + x0]
                       + integ[static_cast<size_t>(y0) * iw + x0];
            double local_mean = sum / static_cast<double>(vcnt);

            double val = static_cast<double>(ir[static_cast<size_t>(y) * w + x]);
            if ((local_mean - val) > local_mean * rel_drop_thresh) {
                raw_mask[static_cast<size_t>(y) * w + x] = 255;
            }
        }
    }

    // Remove isolated 1-pixel IR sensor noise spikes (require at least 1 neighbor in 3x3)
    std::vector<uint8_t> denoised(npix, 0);
    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            size_t idx = static_cast<size_t>(y) * w + x;
            if (!raw_mask[idx]) continue;
            int nb = (raw_mask[idx - 1] > 0) + (raw_mask[idx + 1] > 0)
                   + (raw_mask[idx - w] > 0) + (raw_mask[idx + w] > 0)
                   + (raw_mask[idx - w - 1] > 0) + (raw_mask[idx - w + 1] > 0)
                   + (raw_mask[idx + w - 1] > 0) + (raw_mask[idx + w + 1] > 0);
            if (nb >= 1) denoised[idx] = 255;
        }
    }
    raw_mask.swap(denoised);

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

// 16-bit harmonic diffusion inpainting ONLY on masked dust pixels.
// Untouched pixels remain 100% bit-exact.
static void inpaint_rgb16(
    uint16_t* rgb,
    const uint8_t* mask,
    int w,
    int h,
    int iterations = 36
) {
    std::vector<int> masked_indices;
    masked_indices.reserve(4096);
    const size_t npix = static_cast<size_t>(w) * h;
    for (size_t i = 0; i < npix; ++i) {
        if (mask[i]) masked_indices.push_back(static_cast<int>(i));
    }
    if (masked_indices.empty()) return;

    // Work in float for smooth sub-bit convergence
    std::vector<float> buf(npix * 3);
    for (size_t i = 0; i < npix * 3; ++i) buf[i] = static_cast<float>(rgb[i]);

    // Seed masked pixels from nearest valid neighbor horizontally/vertically
    // so large dust blobs converge rapidly
    std::vector<uint8_t> filled(npix);
    for (size_t i = 0; i < npix; ++i) filled[i] = mask[i] ? 0 : 1;

    std::vector<int> frontier;
    frontier.reserve(masked_indices.size());
    for (int pass = 0; pass < 24; ++pass) {
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

    // Gauss-Seidel harmonic relaxation on masked pixels
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
                buf[static_cast<size_t>(idx) * 3 + 0] = sr / cnt;
                buf[static_cast<size_t>(idx) * 3 + 1] = sg / cnt;
                buf[static_cast<size_t>(idx) * 3 + 2] = sb / cnt;
            }
        }
    }

    for (int idx : masked_indices) {
        size_t o = static_cast<size_t>(idx) * 3;
        rgb[o + 0] = static_cast<uint16_t>(std::clamp(std::lround(buf[o + 0]), 0l, 65535l));
        rgb[o + 1] = static_cast<uint16_t>(std::clamp(std::lround(buf[o + 1]), 0l, 65535l));
        rgb[o + 2] = static_cast<uint16_t>(std::clamp(std::lround(buf[o + 2]), 0l, 65535l));
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
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tif, w * 3));

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
            compute_ir_dust_mask(out_ir16.data(), out_w, out_h, 12, ir_sensitivity, 1, m);
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
    const int ir_dilate = std::max(1, static_cast<int>(std::lround(scan.dpi_x / 600.0f)));

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
            compute_ir_dust_mask(crop_ir.data(), ow, oh, ir_radius, ir_sensitivity, ir_dilate, dust_mask);
            inpaint_rgb16(crop_rgb.data(), dust_mask.data(), ow, oh, 36);
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

} // extern "C"
