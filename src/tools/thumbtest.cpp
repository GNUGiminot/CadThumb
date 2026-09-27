// Test tool: runs the thumbnail provider exactly like Explorer would, without registering it.
//
//   thumbtest <CadThumbShell.dll> <model file> <out.png> [size]     direct COM call into the DLL
//   thumbtest --shell <model file> <out.png> [size]                   through the real shell pipeline
#include "common/ImageIO.h"
#include "common/Paths.h"

#include <windows.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <thumbcache.h>
#include <cstdio>

using namespace ct;

static bool BitmapToImage(HBITMAP bmp, Image& img) {
    // GetDIBits converts any orientation to the top-down layout we request (keeps the alpha byte).
    BITMAP bm{};
    if (!GetObjectW(bmp, sizeof(bm), &bm)) return false;
    img.resize(bm.bmWidth, bm.bmHeight);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = bm.bmWidth;
    bi.bmiHeader.biHeight = -bm.bmHeight;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(nullptr);
    int lines = GetDIBits(dc, bmp, 0, bm.bmHeight, img.px.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    return lines == bm.bmHeight;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) {
        fwprintf(stderr, L"usage: thumbtest <dll> <file> <out.png> [size]\n       thumbtest --shell <file> <out.png> [size]\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    UINT size = argc > 4 ? (UINT)_wtoi(argv[4]) : 256;
    ULONGLONG t0 = GetTickCount64();
    HBITMAP bmp = nullptr;
    HRESULT hr = E_FAIL;

    if (_wcsicmp(argv[1], L"--shell") == 0) {
        IShellItemImageFactory* f = nullptr;
        hr = SHCreateItemFromParsingName(argv[2], nullptr, IID_PPV_ARGS(&f));
        if (SUCCEEDED(hr)) {
            hr = f->GetImage({(LONG)size, (LONG)size}, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &bmp);
            f->Release();
        }
    } else {
        HMODULE dll = LoadLibraryW(argv[1]);
        if (!dll) {
            fwprintf(stderr, L"cannot load %s (%lu)\n", argv[1], GetLastError());
            return 1;
        }
        using GetClassObjectFn = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
        auto getClassObject = (GetClassObjectFn)GetProcAddress(dll, "DllGetClassObject");
        IClassFactory* factory = nullptr;
        hr = getClassObject(CLSID_CadThumbProvider, IID_PPV_ARGS(&factory));
        IInitializeWithStream* init = nullptr;
        if (SUCCEEDED(hr)) hr = factory->CreateInstance(nullptr, IID_PPV_ARGS(&init));
        IStream* stream = nullptr;
        if (SUCCEEDED(hr)) hr = SHCreateStreamOnFileEx(argv[2], STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &stream);
        if (SUCCEEDED(hr)) hr = init->Initialize(stream, STGM_READ);
        IThumbnailProvider* prov = nullptr;
        if (SUCCEEDED(hr)) hr = init->QueryInterface(IID_PPV_ARGS(&prov));
        WTS_ALPHATYPE alpha{};
        if (SUCCEEDED(hr)) hr = prov->GetThumbnail(size, &bmp, &alpha);
        if (prov) prov->Release();
        if (stream) stream->Release();
        if (init) init->Release();
        if (factory) factory->Release();
    }

    ULONGLONG ms = GetTickCount64() - t0;
    if (FAILED(hr) || !bmp) {
        wprintf(L"FAILED hr=0x%08lX after %llu ms\n", hr, ms);
        return 1;
    }
    Image img;
    BitmapToImage(bmp, img);
    DeleteObject(bmp);
    if (!SavePng(argv[3], img)) {
        wprintf(L"cannot save %s\n", argv[3]);
        return 1;
    }
    wprintf(L"OK %dx%d in %llu ms -> %s\n", img.w, img.h, ms, argv[3]);
    return 0;
}
