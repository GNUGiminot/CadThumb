#include "app/App.h"
#include "app/JobManager.h"

#include "common/Cache.h"
#include "common/Log.h"
#include "common/Paths.h"
#include "common/ThreeMfPackage.h"
#include "common/Zip.h"

#include <shlobj.h>
#include <thumbcache.h>

namespace ct {

std::vector<std::wstring> CollectModelFiles(const std::wstring& folder, bool recursive) {
    std::vector<std::wstring> out;
    std::vector<std::wstring> stack{folder};
    while (!stack.empty()) {
        std::wstring dir = stack.back();
        stack.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            std::wstring full = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (recursive && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) stack.push_back(full);
            } else if (FileTypeFromExtension(ExtOf(name)) != FileType::Unknown) {
                out.push_back(full);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return out;
}

void UpdateExplorerThumbnail(const std::wstring& path) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        IThumbnailCache* cache = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_LocalThumbnailCache, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&cache)))) {
            ISharedBitmap* bmp = nullptr;
            WTS_CACHEFLAGS flags{};
            WTS_THUMBNAILID id{};
            // Re-extract through our handler (it now hits our cache) and replace Explorer's cached copy.
            cache->GetThumbnail(item, 256, WTS_EXTRACT | WTS_FORCEEXTRACTION, &bmp, &flags, &id);
            if (bmp) bmp->Release();
            cache->Release();
        }
        item->Release();
    }
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, path.c_str(), nullptr);
}

static bool HasEmbedded3mfThumbnail(const std::wstring& path) {
    ZipArchive zip;
    std::vector<char> bytes;
    return zip.OpenFile(path) && Find3mfThumbnail(zip, bytes);
}

void PrewarmFolder(const std::wstring& folder, PrewarmState& st) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    st.running = true;
    st.cancel = false;
    st.folder = folder;
    st.total = st.done = st.rendered = st.failed = st.skipped = 0;
    Log(L"prewarm started: %s", folder.c_str());

    auto files = CollectModelFiles(folder, true);
    st.total = (int)files.size();
    auto& jm = JobManager::Instance();

    struct Pending {
        std::wstring path;
        JobManager::JobPtr job;
    };
    std::vector<Pending> pending;
    const Settings s = Settings::Get();

    for (const auto& path : files) {
        if (st.cancel) break;
        FileType type = FileTypeFromExtension(ExtOf(path));
        if (!FileTypeEnabled(type, s) || (type == FileType::ThreeMf && s.prefer3mfEmbedded && HasEmbedded3mfThumbnail(path))) {
            ++st.skipped;
            ++st.done;
            continue;
        }
        std::string key;
        uint64_t size = 0;
        if (!CacheKeyFromFile(path, type, 256, s.renderSignature, key, &size) ||
            size > uint64_t(s.maxFileSizeMB) * 1024 * 1024) {
            ++st.skipped;
            ++st.done;
            continue;
        }
        JobManager::JobPtr job;
        switch (jm.Reserve(key, type, 256, FileNameOf(path), job)) {
        case JobManager::ReserveResult::Cached:
        case JobManager::ReserveResult::KnownFailure:
            ++st.skipped;
            ++st.done;
            continue;
        case JobManager::ReserveResult::New:
            jm.Enqueue(job, path, false, false);
            [[fallthrough]];
        case JobManager::ReserveResult::Existing:
            pending.push_back({path, job});
            break;
        }
    }

    for (auto& p : pending) {
        while (!jm.Wait(p.job, 1000)) {
            if (st.cancel) break;
        }
        if (st.cancel) break;
        if (p.job->state == JobManager::Job::Done) {
            ++st.rendered;
            UpdateExplorerThumbnail(p.path);
        } else {
            ++st.failed;
        }
        ++st.done;
    }
    Log(L"prewarm finished: %d files, %d rendered, %d failed, %d skipped", st.total.load(), st.rendered.load(),
        st.failed.load(), st.skipped.load());
    st.running = false;
    CoUninitialize();
}

int RefreshFiles(const std::vector<std::wstring>& paths) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const Settings s = Settings::Get();
    int failures = 0;
    std::wstring report;
    for (const auto& path : paths) {
        FileType type = FileTypeFromExtension(ExtOf(path));
        if (type == FileType::Unknown) continue;
        // drop cached results of every size bucket, then render the default one right away
        for (int bucket : {256, 512, 1024}) {
            std::string k;
            if (CacheKeyFromFile(path, type, bucket, s.renderSignature, k)) RemoveCacheEntry(k);
        }
        std::string key;
        std::wstring error;
        bool embedded = type == FileType::ThreeMf && s.prefer3mfEmbedded && HasEmbedded3mfThumbnail(path);
        if (!embedded) {
            if (!CacheKeyFromFile(path, type, 256, s.renderSignature, key) ||
                !RenderToCache(path, type, 256, key, s, &error)) {
                ++failures;
                report += FileNameOf(path) + L": " + error + L"\r\n";
                continue;
            }
        }
        UpdateExplorerThumbnail(path);
    }
    if (failures) {
        Out(L"Не удалось построить эскиз:\r\n" + report);
        FlushOutAsMessageBox(L"CadThumb", true);
    }
    CoUninitialize();
    return failures ? 2 : 0;
}

} // namespace ct
