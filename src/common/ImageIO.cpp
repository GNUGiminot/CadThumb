#include "common/ImageIO.h"

#include <shlwapi.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>

namespace ct {

template <class T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

static bool Factory(ComPtr<IWICImagingFactory>& f) {
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&f.p)));
}

static bool DecodeFrom(IWICImagingFactory* factory, IWICBitmapDecoder* decoder, Image& out) {
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return false;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(factory->CreateFormatConverter(&conv))) return false;
    if (FAILED(conv->Initialize(frame.p, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return false;
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    // Embedded previews are decoded inside Explorer: cap decoded memory at 64 MiB.
    if (!w || !h || w > 16384 || h > 16384 || uint64_t(w) * h > 16ull * 1024 * 1024) return false;
    out.resize((int)w, (int)h);
    return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)out.px.size(), out.px.data()));
}

bool DecodeImage(const void* data, size_t size, Image& out) {
    ComPtr<IWICImagingFactory> factory;
    if (!Factory(factory)) return false;
    ComPtr<IStream> stream;
    stream.p = SHCreateMemStream(static_cast<const BYTE*>(data), (UINT)size);
    if (!stream) return false;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.p, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)))
        return false;
    return DecodeFrom(factory.p, decoder.p, out);
}

bool LoadImageFile(const std::wstring& path, Image& out) {
    ComPtr<IWICImagingFactory> factory;
    if (!Factory(factory)) return false;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, &decoder)))
        return false;
    return DecodeFrom(factory.p, decoder.p, out);
}

bool SavePng(const std::wstring& path, const Image& img) {
    if (img.empty()) return false;
    ComPtr<IWICImagingFactory> factory;
    if (!Factory(factory)) return false;
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
    ComPtr<IWICBitmapEncoder> enc;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc))) return false;
    if (FAILED(enc->Initialize(stream.p, WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(enc->CreateNewFrame(&frame, nullptr))) return false;
    if (FAILED(frame->Initialize(nullptr))) return false;
    frame->SetSize(img.w, img.h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&fmt);
    if (fmt != GUID_WICPixelFormat32bppBGRA) return false;
    if (FAILED(frame->WritePixels(img.h, img.w * 4, (UINT)img.px.size(), const_cast<BYTE*>(img.px.data()))))
        return false;
    if (FAILED(frame->Commit())) return false;
    return SUCCEEDED(enc->Commit());
}

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
        // downscale: exact area average with premultiplied alpha
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
        // upscale: bilinear with premultiplied alpha
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

HBITMAP CreateThumbnailBitmap(const Image& img) {
    if (img.empty()) return nullptr;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = img.w;
    bi.bmiHeader.biHeight = -img.h; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) {
        auto* dst = static_cast<uint8_t*>(bits);
        for (size_t i = 0; i < img.px.size(); i += 4) {
            const unsigned a = img.px[i + 3];
            for (int c = 0; c < 3; ++c) dst[i + c] = uint8_t((img.px[i + c] * a + 127) / 255);
            dst[i + 3] = uint8_t(a);
        }
    }
    return bmp;
}

} // namespace ct
