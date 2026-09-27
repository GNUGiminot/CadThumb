#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ct {

// 32-bit BGRA, straight (non-premultiplied) alpha, top-down rows. Same layout as the Windows build's
// Image, so render/Rasterizer.cpp (which writes BGRA directly) is shared byte-for-byte between platforms.
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

bool DecodeImage(const void* data, size_t size, Image& out); // PNG/JPEG/BMP via stb_image
bool SavePng(const std::string& path, const Image& img);

// Area-averaging resample that keeps aspect ratio and fits into maxDim x maxDim.
Image ScaleToFit(const Image& src, int maxDim);

} // namespace ct
