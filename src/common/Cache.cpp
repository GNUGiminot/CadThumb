#include "common/Cache.h"
#include "common/Hash.h"
#include "common/Paths.h"

#include <windows.h>
#include <algorithm>
#include <vector>

namespace ct {

int SizeBucket(unsigned cx) {
    if (cx <= 256) return 256;
    if (cx <= 512) return 512;
    return 1024;
}

std::string ComputeCacheKey(const ReadAtFn& read, uint64_t size, uint64_t mtime, FileType type, int bucket,
                            uint64_t renderSignature) {
    Hasher h(0xCAD7);
    h.Add(size);
    h.Add(mtime);
    h.Add(uint32_t(type));
    h.Add(bucket);
    h.Add(renderSignature);

    std::vector<char> buf(64 * 1024);
    auto sample = [&](uint64_t off, size_t len) {
        if (off >= size) return;
        len = (size_t)std::min<uint64_t>(len, size - off);
        size_t got = read(off, buf.data(), len);
        h.Add(off);
        h.Update(buf.data(), got);
    };
    sample(0, 64 * 1024);
    if (size > 256 * 1024) {
        for (int i = 1; i <= 4; ++i) sample(size / 5 * i, 4096);
    }
    if (size > 64 * 1024) sample(size - std::min<uint64_t>(size, 64 * 1024), 64 * 1024);
    return h.HexDigest128();
}

bool CacheKeyFromStream(IStream* stream, uint64_t size, uint64_t mtime, FileType type, int bucket,
                        uint64_t renderSignature, std::string& key) {
    bool ok = true;
    auto read = [&](uint64_t off, void* dst, size_t len) -> size_t {
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)off;
        if (FAILED(stream->Seek(li, STREAM_SEEK_SET, nullptr))) {
            ok = false;
            return 0;
        }
        size_t total = 0;
        while (total < len) {
            ULONG got = 0;
            HRESULT hr = stream->Read(static_cast<char*>(dst) + total, (ULONG)(len - total), &got);
            if (FAILED(hr) || got == 0) break;
            total += got;
        }
        return total;
    };
    key = ComputeCacheKey(read, size, mtime, type, bucket, renderSignature);
    return ok;
}

bool CacheKeyFromFile(const std::wstring& path, FileType type, int bucket, uint64_t renderSignature,
                      std::string& key, uint64_t* sizeOut) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    GetFileInformationByHandle(f, &info);
    uint64_t size = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    uint64_t mtime = (uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
    auto read = [&](uint64_t off, void* dst, size_t len) -> size_t {
        OVERLAPPED ov{};
        ov.Offset = (DWORD)off;
        ov.OffsetHigh = (DWORD)(off >> 32);
        DWORD got = 0;
        if (!ReadFile(f, dst, (DWORD)len, &got, &ov)) return 0;
        return got;
    };
    key = ComputeCacheKey(read, size, mtime, type, bucket, renderSignature);
    CloseHandle(f);
    if (sizeOut) *sizeOut = size;
    return true;
}

std::wstring CachePngPath(const std::string& key) { return CacheDir() + L"\\" + Wide(key) + L".png"; }
std::wstring CacheFailPath(const std::string& key) { return CacheDir() + L"\\" + Wide(key) + L".fail"; }

static uint64_t FileAgeHours(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return 0;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a{}, b{};
    a.LowPart = fad.ftLastWriteTime.dwLowDateTime;
    a.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    b.LowPart = now.dwLowDateTime;
    b.HighPart = now.dwHighDateTime;
    if (b.QuadPart <= a.QuadPart) return 0;
    return (b.QuadPart - a.QuadPart) / (10000000ull * 3600);
}

CacheState QueryCache(const std::string& key, int failRetryHours) {
    if (FileExists(CachePngPath(key))) return CacheState::Ready;
    std::wstring fail = CacheFailPath(key);
    if (FileExists(fail)) {
        if (FileAgeHours(fail) < (uint64_t)failRetryHours) return CacheState::Failed;
    }
    return CacheState::Missing;
}

bool CommitSuccess(const std::string& key, const std::wstring& renderedPng) {
    DeleteFileW(CacheFailPath(key).c_str());
    return MoveFileExW(renderedPng.c_str(), CachePngPath(key).c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0;
}

void CommitFailure(const std::string& key, const std::wstring& reason) {
    HANDLE f = CreateFileW(CacheFailPath(key).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    std::string u8 = Utf8(reason);
    DWORD w = 0;
    WriteFile(f, u8.data(), (DWORD)u8.size(), &w, nullptr);
    CloseHandle(f);
}

void RemoveCacheEntry(const std::string& key) {
    DeleteFileW(CachePngPath(key).c_str());
    DeleteFileW(CacheFailPath(key).c_str());
}

struct Entry {
    std::wstring path;
    uint64_t size;
    FILETIME time;
    bool fail;
};

static std::vector<Entry> ListCache() {
    std::vector<Entry> out;
    std::wstring dir = CacheDir();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring ext = ExtOf(fd.cFileName);
        if (ext != L".png" && ext != L".fail") continue;
        out.push_back({dir + L"\\" + fd.cFileName, (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow,
                       fd.ftLastWriteTime, ext == L".fail"});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

CacheStats GetCacheStats() {
    CacheStats s;
    for (const auto& e : ListCache()) {
        if (e.fail)
            ++s.failures;
        else {
            ++s.files;
            s.bytes += e.size;
        }
    }
    return s;
}

void PruneCache(int maxMB, int failRetryHours) {
    auto entries = ListCache();
    uint64_t total = 0;
    for (auto& e : entries) {
        if (e.fail && FileAgeHours(e.path) >= (uint64_t)failRetryHours) DeleteFileW(e.path.c_str());
        else if (!e.fail) total += e.size;
    }
    uint64_t limit = uint64_t(maxMB) * 1024 * 1024;
    if (total <= limit) return;
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return CompareFileTime(&a.time, &b.time) < 0; });
    for (auto& e : entries) {
        if (total <= limit * 8 / 10) break;
        if (e.fail) continue;
        if (DeleteFileW(e.path.c_str())) total -= e.size;
    }
}

void ClearCache() {
    for (auto& e : ListCache()) DeleteFileW(e.path.c_str());
}

void CleanupTempDir(int olderThanHours) {
    std::wstring dir = TempDir();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring p = dir + L"\\" + fd.cFileName;
        if (FileAgeHours(p) >= (uint64_t)olderThanHours) DeleteFileW(p.c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

} // namespace ct
