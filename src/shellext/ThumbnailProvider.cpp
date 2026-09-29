#include "shellext/ThumbnailProvider.h"

#include "common/Cache.h"
#include "common/FileType.h"
#include "common/ImageIO.h"
#include "common/Log.h"
#include "common/Paths.h"
#include "common/PipeClient.h"
#include "common/Settings.h"
#include "common/ThreeMfPackage.h"
#include "common/Zip.h"

#include <shlwapi.h>
#include <algorithm>
#include <cwchar>

namespace ct {

ThumbnailProvider::ThumbnailProvider() { InterlockedIncrement(&g_dllRefs); }

ThumbnailProvider::~ThumbnailProvider() {
    if (stream_) stream_->Release();
    InterlockedDecrement(&g_dllRefs);
}

IFACEMETHODIMP ThumbnailProvider::QueryInterface(REFIID riid, void** ppv) {
    static const QITAB qit[] = {
        QITABENT(ThumbnailProvider, IInitializeWithStream),
        QITABENT(ThumbnailProvider, IThumbnailProvider),
        {nullptr, 0},
    };
    return QISearch(this, qit, riid, ppv);
}

IFACEMETHODIMP_(ULONG) ThumbnailProvider::AddRef() { return InterlockedIncrement(&refs_); }

IFACEMETHODIMP_(ULONG) ThumbnailProvider::Release() {
    long r = InterlockedDecrement(&refs_);
    if (r == 0) delete this;
    return r;
}

IFACEMETHODIMP ThumbnailProvider::Initialize(IStream* stream, DWORD) {
    if (!stream) return E_INVALIDARG;
    if (stream_) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    stream_ = stream;
    stream_->AddRef();
    return S_OK;
}

HRESULT ThumbnailProvider::ProduceNoThrow(UINT cx, HBITMAP* bitmap, WTS_ALPHATYPE* alpha) {
    try {
        return Produce(cx, bitmap, alpha);
    } catch (...) {
        Log(L"unexpected exception in GetThumbnail");
        return E_FAIL;
    }
}

// When registered per user, the handler runs inside explorer.exe: never let a hardware exception
// (e.g. a malformed 3MF hitting a bug) escape and take Explorer down.
static HRESULT GuardedProduce(ThumbnailProvider* self, UINT cx, HBITMAP* bitmap, WTS_ALPHATYPE* alpha) {
    __try {
        return self->ProduceNoThrow(cx, bitmap, alpha);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_UNEXPECTED;
    }
}

IFACEMETHODIMP ThumbnailProvider::GetThumbnail(UINT cx, HBITMAP* bitmap, WTS_ALPHATYPE* alpha) {
    if (!bitmap || !alpha) return E_POINTER;
    *bitmap = nullptr;
    *alpha = WTSAT_UNKNOWN;
    if (cx == 0 || cx > 16384) return E_INVALIDARG;
    if (!stream_) return E_UNEXPECTED;
    HRESULT hr = GuardedProduce(this, cx, bitmap, alpha);
    if (hr == E_UNEXPECTED) Log(L"structured exception caught in GetThumbnail");
    return hr;
}

static HRESULT Deliver(const Image& img, UINT cx, HBITMAP* bitmap, WTS_ALPHATYPE* alpha) {
    if (img.empty()) return E_FAIL;
    int target = std::min<int>((int)cx, std::max(img.w, img.h)); // never upscale
    HBITMAP bmp = CreateThumbnailBitmap(ScaleToFit(img, target));
    if (!bmp) return E_OUTOFMEMORY;
    *bitmap = bmp;
    *alpha = WTSAT_ARGB;
    return S_OK;
}

HRESULT ThumbnailProvider::Produce(UINT cx, HBITMAP* bitmap, WTS_ALPHATYPE* alpha) {
    const Settings s = Settings::Get();
    const ULONGLONG t0 = GetTickCount64();

    STATSTG st{};
    std::wstring name;
    if (SUCCEEDED(stream_->Stat(&st, STATFLAG_DEFAULT))) {
        if (st.pwcsName) {
            name = FileNameOf(st.pwcsName);
            CoTaskMemFree(st.pwcsName);
        }
    } else if (FAILED(stream_->Stat(&st, STATFLAG_NONAME))) {
        return E_FAIL;
    }
    const uint64_t size = st.cbSize.QuadPart;
    const uint64_t mtime = (uint64_t(st.mtime.dwHighDateTime) << 32) | st.mtime.dwLowDateTime;

    FileType type = FileTypeFromExtension(ExtOf(name));
    if (type == FileType::Unknown) {
        uint8_t head[512] = {};
        ULONG got = 0;
        LARGE_INTEGER zero{};
        stream_->Seek(zero, STREAM_SEEK_SET, nullptr);
        stream_->Read(head, sizeof(head), &got);
        type = FileTypeSniff(head, got);
    }
    if (!FileTypeEnabled(type, s)) return E_FAIL;

    // 3MF: slicers usually store a ready picture inside the package — use it, no rendering needed.
    if (type == FileType::ThreeMf && s.prefer3mfEmbedded) {
        ZipArchive zip;
        std::vector<char> bytes;
        Image img;
        if (zip.OpenStream(stream_, size) && Find3mfThumbnail(zip, bytes) &&
            DecodeImage(bytes.data(), bytes.size(), img)) {
            LogVerbose(L"%s: embedded 3MF thumbnail %dx%d (%llu ms)", name.c_str(), img.w, img.h,
                       GetTickCount64() - t0);
            return Deliver(img, cx, bitmap, alpha);
        }
    }

    if (size > uint64_t(s.maxFileSizeMB) * 1024 * 1024) {
        LogVerbose(L"%s: skipped, %llu MB > MaxFileSizeMB", name.c_str(), size >> 20);
        return E_FAIL;
    }

    const int bucket = SizeBucket(cx);
    std::string key;
    if (!CacheKeyFromStream(stream_, size, mtime, type, bucket, s.renderSignature, key)) return E_FAIL;

    Image img;
    switch (QueryCache(key, s.failRetryHours)) {
    case CacheState::Ready:
        if (LoadImageFile(CachePngPath(key), img)) {
            LogVerbose(L"%s: cache hit (%llu ms)", name.c_str(), GetTickCount64() - t0);
            return Deliver(img, cx, bitmap, alpha);
        }
        RemoveCacheEntry(key); // A corrupt PNG must not prevent regeneration forever.
        break;
    case CacheState::Failed:
        LogVerbose(L"%s: known failure, skipped", name.c_str());
        return E_FAIL;
    default:
        break;
    }

    PipeRequest req;
    req.kind = ReqRender;
    req.fileType = uint32_t(type);
    req.bucket = (uint32_t)bucket;
    req.dataSize = size;
    strncpy_s(req.key, key.c_str(), _TRUNCATE);
    wcsncpy_s(req.name, name.c_str(), _TRUNCATE);

    const std::wstring exe = DirOf(ModulePath(g_module)) + L"\\" + kExeName;
    RenderRequestResult r = RequestRender(req, stream_, DWORD(s.handlerWaitSec) * 1000, exe);
    switch (r) {
    case RenderRequestResult::Done:
        if (LoadImageFile(CachePngPath(key), img)) {
            LogVerbose(L"%s: rendered (%llu ms)", name.c_str(), GetTickCount64() - t0);
            return Deliver(img, cx, bitmap, alpha);
        }
        Log(L"%s: render reported done but cache file is missing", name.c_str());
        return E_FAIL;
    case RenderRequestResult::Timeout:
        Log(L"%s: not ready after %d s, continues in background", name.c_str(), s.handlerWaitSec);
        return E_PENDING;
    case RenderRequestResult::NoHost:
        Log(L"%s: CadThumb host is not available (%s)", name.c_str(), exe.c_str());
        return E_FAIL;
    default:
        LogVerbose(L"%s: render failed", name.c_str());
        return E_FAIL;
    }
}

} // namespace ct
