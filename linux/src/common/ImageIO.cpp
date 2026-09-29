#include "common/ImageIO.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO // we decode from memory only (embedded 3MF thumbnails)
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <climits>

namespace ct {

static void RgbaToBgra(uint8_t* px, size_t count) {
    for (size_t i = 0; i < count; ++i) std::swap(px[i * 4], px[i * 4 + 2]);
}

bool DecodeImage(const void* data, size_t size, Image& out) {
    int w = 0, h = 0, comp = 0;
    if (size > INT_MAX || !stbi_info_from_memory(static_cast<const uint8_t*>(data), (int)size, &w, &h, &comp) ||
        w <= 0 || h <= 0 || w > 16384 || h > 16384 || uint64_t(w) * h > 16ull * 1024 * 1024) return false;
    uint8_t* rgba = stbi_load_from_memory(static_cast<const uint8_t*>(data), (int)size, &w, &h, &comp, 4);
    if (!rgba) return false;
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) {
        stbi_image_free(rgba);
        return false;
    }
    out.resize(w, h);
    memcpy(out.px.data(), rgba, out.px.size());
    stbi_image_free(rgba);
    RgbaToBgra(out.px.data(), out.px.size() / 4); // stb decodes RGBA; Image (and Rasterizer.cpp) use BGRA
    return true;
}

bool SavePng(const std::string& path, const Image& img) {
    if (img.empty()) return false;
    std::vector<uint8_t> rgba = img.px;
    RgbaToBgra(rgba.data(), rgba.size() / 4); // BGRA -> RGBA for stb_image_write; the swap is its own inverse
    return stbi_write_png(path.c_str(), img.w, img.h, 4, rgba.data(), img.w * 4) != 0;
}

// Identical algorithm to the Windows build's common/ImageIO.cpp (pure Image-struct math, no platform API).
Image ScaleToFit(const Image& src, int maxDim) {
    if (src.empty() || maxDim <= 0) return {};
    double k = std::min(double(maxDim) / src.w, double(maxDim) / src.h);
    int dw = std::max(1, (int)std::lround(src.w * k));
    int dh = std::max(1, (int)std::lround(src.h * k));
    if (dw == src.w && dh == src.h) return src;

    Image dst;
    dst.resize(dw, dh);
    const double sx = double(src.w) / dw, sy = double(src.h) / dh;

    if (sx >= 1.0 && sy >= 1.0) {
        for (int y = 0; y < dh; ++y) {
            double y0 = y * sy, y1 = y0 + sy;
            for (int x = 0; x < dw; ++x) {
                double x0 = x * sx, x1 = x0 + sx;
                double acc[4] = {0, 0, 0, 0}, wsum = 0;
                for (int iy = (int)y0; iy < std::min((int)std::ceil(y1), src.h); ++iy) {
                    double wy = std::min(y1, iy + 1.0) - std::max(y0, (double)iy);
                    if (wy <= 0) continue;
                    for (int ix = (int)x0; ix < std::min((int)std::ceil(x1), src.w); ++ix) {
                        double wx = std::min(x1, ix + 1.0) - std::max(x0, (double)ix);
                        if (wx <= 0) continue;
                        const uint8_t* p = &src.px[(size_t(iy) * src.w + ix) * 4];
                        double wgt = wx * wy, a = p[3] / 255.0;
                        acc[0] += p[0] * a * wgt;
                        acc[1] += p[1] * a * wgt;
                        acc[2] += p[2] * a * wgt;
                        acc[3] += a * wgt;
                        wsum += wgt;
                    }
                }
                uint8_t* d = &dst.px[(size_t(y) * dw + x) * 4];
                if (acc[3] > 1e-9) {
                    d[0] = (uint8_t)std::clamp(acc[0] / acc[3] + 0.5, 0.0, 255.0);
                    d[1] = (uint8_t)std::clamp(acc[1] / acc[3] + 0.5, 0.0, 255.0);
                    d[2] = (uint8_t)std::clamp(acc[2] / acc[3] + 0.5, 0.0, 255.0);
                }
                d[3] = (uint8_t)std::clamp(acc[3] / std::max(wsum, 1e-9) * 255.0 + 0.5, 0.0, 255.0);
            }
        }
    } else {
        for (int y = 0; y < dh; ++y) {
            double fy = std::clamp((y + 0.5) * sy - 0.5, 0.0, src.h - 1.0);
            int y0 = (int)fy, y1 = std::min(y0 + 1, src.h - 1);
            double ty = fy - y0;
            for (int x = 0; x < dw; ++x) {
                double fx = std::clamp((x + 0.5) * sx - 0.5, 0.0, src.w - 1.0);
                int x0 = (int)fx, x1 = std::min(x0 + 1, src.w - 1);
                double tx = fx - x0;
                const uint8_t* p[4] = {&src.px[(size_t(y0) * src.w + x0) * 4], &src.px[(size_t(y0) * src.w + x1) * 4],
                                       &src.px[(size_t(y1) * src.w + x0) * 4], &src.px[(size_t(y1) * src.w + x1) * 4]};
                double w[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
                double acc[4] = {0, 0, 0, 0};
                for (int i = 0; i < 4; ++i) {
                    double a = p[i][3] / 255.0 * w[i];
                    acc[0] += p[i][0] * a;
                    acc[1] += p[i][1] * a;
                    acc[2] += p[i][2] * a;
                    acc[3] += a;
                }
                uint8_t* d = &dst.px[(size_t(y) * dw + x) * 4];
                if (acc[3] > 1e-9)
                    for (int c = 0; c < 3; ++c) d[c] = (uint8_t)std::clamp(acc[c] / acc[3] + 0.5, 0.0, 255.0);
                d[3] = (uint8_t)std::clamp(acc[3] * 255.0 + 0.5, 0.0, 255.0);
            }
        }
    }
    return dst;
}

} // namespace ct
