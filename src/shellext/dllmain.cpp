#include "common/Log.h"
#include "common/Paths.h"
#include "common/Registration.h"
#include "shellext/ThumbnailProvider.h"

#include <windows.h>
#include <new>
#include <olectl.h>

HMODULE g_module = nullptr;
long g_dllRefs = 0;

namespace {

class ClassFactory : public IClassFactory {
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        long r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return r;
    }
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* p = new (std::nothrow) ct::ThumbnailProvider();
        if (!p) return E_OUTOFMEMORY;
        HRESULT hr = p->QueryInterface(riid, ppv);
        p->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL lock) override {
        if (lock)
            InterlockedIncrement(&g_dllRefs);
        else
            InterlockedDecrement(&g_dllRefs);
        return S_OK;
    }
    ClassFactory() { InterlockedIncrement(&g_dllRefs); }
    ~ClassFactory() { InterlockedDecrement(&g_dllRefs); }

private:
    long refs_ = 1;
};

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        ct::SetLogTag(L"shell");
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (clsid != ct::CLSID_CadThumbProvider) return CLASS_E_CLASSNOTAVAILABLE;
    auto* f = new (std::nothrow) ClassFactory();
    if (!f) return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}

STDAPI DllCanUnloadNow() { return g_dllRefs == 0 ? S_OK : S_FALSE; }

// regsvr32 support: per-user registration pointing to CadThumb.exe next to the DLL.
STDAPI DllRegisterServer() {
    ct::RegisterOptions opt;
    opt.dllPath = ct::ModulePath(g_module);
    opt.exePath = ct::DirOf(opt.dllPath) + L"\\" + ct::kExeName;
    std::wstring report;
    return ct::RegisterShellExtension(opt, report) ? S_OK : SELFREG_E_CLASS;
}

STDAPI DllUnregisterServer() {
    std::wstring report;
    return ct::UnregisterShellExtension(false, report) ? S_OK : SELFREG_E_CLASS;
}
