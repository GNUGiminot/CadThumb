#include "render/Loaders.h"

#include <windows.h>
#include <algorithm>
#include <charconv>
#include <string>
#include <cstring>
#include <memory>

namespace ct {

namespace {

// Read-only memory mapping of the whole file.
struct MappedFile {
    HANDLE file = INVALID_HANDLE_VALUE, map = nullptr;
    const char* data = nullptr;
    uint64_t size = 0;
    bool Open(const std::wstring& path) {
        file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER li{};
        GetFileSizeEx(file, &li);
        size = (uint64_t)li.QuadPart;
        if (size == 0) return false;
        map = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!map) return false;
        data = static_cast<const char*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0));
        return data != nullptr;
    }
    ~MappedFile() {
        if (data) UnmapViewOfFile(data);
        if (map) CloseHandle(map);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
};

bool ParseAscii(const char* p, const char* end, Mesh& mesh) {
    // Only "vertex x y z" lines matter; every 3 vertices form a facet.
    float v[9];
    int n = 0;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
        if (end - p >= 6 && memcmp(p, "vertex", 6) == 0) {
            p += 6;
            for (int k = 0; k < 3; ++k) {
                while (p < end && (*p == ' ' || *p == '\t')) ++p;
                float f = 0;
                auto res = std::from_chars(p, end, f);
                if (res.ec != std::errc()) return false;
                p = res.ptr;
                v[n * 3 + k] = f;
            }
            if (++n == 3) {
                uint32_t a = mesh.AddVertex(v[0], v[1], v[2]);
                uint32_t b = mesh.AddVertex(v[3], v[4], v[5]);
                uint32_t c = mesh.AddVertex(v[6], v[7], v[8]);
                mesh.AddTriangle(a, b, c);
                n = 0;
            }
        } else if (end - p >= 8 && memcmp(p, "endfacet", 8) == 0) {
            n = 0;
            p += 8;
        }
        while (p < end && *p != '\n') ++p;
    }
    return !mesh.idx.empty();
}

} // namespace

bool LoadStl(const std::wstring& path, Mesh& mesh, std::string& error) {
    MappedFile f;
    if (!f.Open(path)) {
        error = "cannot open file";
        return false;
    }
    bool binary = false;
    uint32_t count = 0;
    if (f.size >= 84) {
        memcpy(&count, f.data + 80, 4);
        binary = 84ull + 50ull * count == f.size;
    }
    bool asciiLike = f.size >= 5 && memcmp(f.data, "solid", 5) == 0;
    if (!binary && asciiLike) {
        size_t probe = (size_t)std::min<uint64_t>(f.size, 4096);
        asciiLike = std::string(f.data, probe).find("facet") != std::string::npos;
    }
    if (!binary && !asciiLike && f.size >= 84) {
        // Truncated or padded binary file: read as many whole facets as present.
        count = (uint32_t)std::min<uint64_t>(count, (f.size - 84) / 50);
        binary = count > 0;
    }

    try {
        if (binary) {
            mesh.pos.reserve(size_t(count) * 9);
            mesh.idx.reserve(size_t(count) * 3);
            const char* p = f.data + 84;
            for (uint32_t i = 0; i < count; ++i, p += 50) {
                float v[9];
                memcpy(v, p + 12, sizeof(v));
                uint32_t a = mesh.AddVertex(v[0], v[1], v[2]);
                uint32_t b = mesh.AddVertex(v[3], v[4], v[5]);
                uint32_t c = mesh.AddVertex(v[6], v[7], v[8]);
                mesh.AddTriangle(a, b, c);
            }
        } else if (!ParseAscii(f.data, f.data + f.size, mesh)) {
            error = "no facets found";
            return false;
        }
    } catch (const std::bad_alloc&) {
        error = "out of memory";
        return false;
    }
    if (mesh.idx.empty()) {
        error = "no facets";
        return false;
    }
    return true;
}

} // namespace ct
