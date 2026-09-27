#pragma once
#include <windows.h>
#include <propsys.h>
#include <shobjidl.h>
#include <thumbcache.h>

extern HMODULE g_module;
extern long g_dllRefs;

namespace ct {

// Explorer thumbnail handler. Implements IInitializeWithStream (not IInitializeWithFile), which lets
// Windows host it in its isolated surrogate process when registered machine-wide. Per-user
// registrations run inside Explorer (see Registration.cpp), so the DLL stays small: no model
// parsing here, only the cache, the pipe to CadThumb.exe and embedded 3MF pictures.
class ThumbnailProvider : public IInitializeWithStream, public IThumbnailProvider {
public:
    ThumbnailProvider();

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    // IInitializeWithStream
    IFACEMETHODIMP Initialize(IStream* stream, DWORD mode) override;

    // IThumbnailProvider
    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* bitmap, WTS_ALPHATYPE* alpha) override;

    HRESULT ProduceNoThrow(UINT cx, HBITMAP* bitmap, WTS_ALPHATYPE* alpha);

private:
    ~ThumbnailProvider();
    HRESULT Produce(UINT cx, HBITMAP* bitmap, WTS_ALPHATYPE* alpha);

    long refs_ = 1;
    IStream* stream_ = nullptr;
};

} // namespace ct
