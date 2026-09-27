#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace ct {

// 32-bit BGRA, straight (non-premultiplied) alpha, top-down rows.
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    bool empty() const { return w <= 0 || h <= 0; }
    void resize(int nw, int nh) {
        w = nw;
        h = nh;
        px.assign(size_t(nw) * nh * 4, 0);
    }
};

bool DecodeImage(const void* data, size_t size, Image& out);   // PNG/JPEG/BMP via WIC
bool LoadImageFile(const std::wstring& path, Image& out);
bool SavePng(const std::wstring& path, const Image& img);

// Area-averaging resample that keeps aspect ratio and fits into maxDim x maxDim.
Image ScaleToFit(const Image& src, int maxDim);

// Top-down 32bpp DIB section suitable for IThumbnailProvider (WTSAT_ARGB).
HBITMAP CreateThumbnailBitmap(const Image& img);

} // namespace ct
