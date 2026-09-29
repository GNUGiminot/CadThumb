#include "render/Loaders.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <charconv>
#include <cstring>
#include <cmath>
#include <memory>

namespace ct {

namespace {

// Read-only memory mapping of the whole file (POSIX mmap).
struct MappedFile {
    int fd = -1;
    const char* data = nullptr;
    uint64_t size = 0;

    bool Open(const std::string& path) {
        fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) return false;
        struct stat st{};
        if (fstat(fd, &st) != 0 || st.st_size == 0) return false;
        size = (uint64_t)st.st_size;
        void* p = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) return false;
        data = static_cast<const char*>(p);
        return true;
    }
    ~MappedFile() {
        if (data) munmap(const_cast<char*>(data), size);
        if (fd >= 0) close(fd);
    }
};

bool ParseAscii(const char* p, const char* end, Mesh& mesh) {
    // Only "vertex x y z" lines matter; every 3 vertices form a facet.
    float v[9];
    int n = 0;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
        if (end - p >= 7 && memcmp(p, "vertex", 6) == 0 && (p[6] == ' ' || p[6] == '\t')) {
            p += 6;
            for (int k = 0; k < 3; ++k) {
                while (p < end && (*p == ' ' || *p == '\t')) ++p;
                float f = 0;
                if (p < end && *p == '+') ++p;
                auto res = std::from_chars(p, end, f);
                if (res.ec != std::errc() || !std::isfinite(f)) return false;
                p = res.ptr;
                if (p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') return false;
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
            if (n != 0) return false;
            n = 0;
            p += 8;
        }
        while (p < end && *p != '\n') ++p;
    }
    return n == 0 && !mesh.idx.empty();
}

} // namespace

bool LoadStl(const std::string& path, Mesh& mesh, std::string& error) {
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
    const char* start = f.data;
    const char* end = f.data + f.size;
    if (end - start >= 3 && memcmp(start, "\xEF\xBB\xBF", 3) == 0) start += 3;
    while (start < end && (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')) ++start;
    bool asciiLike = end - start >= 5 && memcmp(start, "solid", 5) == 0;
    if (!binary && !asciiLike && f.size >= 84) {
        // Padding is harmless; truncation must not silently produce a partial model.
        binary = count > 0 && 84ull + 50ull * count <= f.size;
    }
    if (!binary && !asciiLike) {
        error = "STL: invalid or truncated file";
        return false;
    }

    try {
        if (binary) {
            mesh.pos.reserve(size_t(count) * 9);
            mesh.idx.reserve(size_t(count) * 3);
            const char* p = f.data + 84;
            for (uint32_t i = 0; i < count; ++i, p += 50) {
                float v[9];
                memcpy(v, p + 12, sizeof(v));
                for (float value : v) {
                    if (!std::isfinite(value)) {
                        error = "STL: non-finite coordinate";
                        return false;
                    }
                }
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
