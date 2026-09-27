#pragma once
#include <objidl.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ct {

// Read-only ZIP access (miniz) from a file or from an IStream (no full copy into memory).
class ZipArchive {
public:
    ZipArchive();
    ~ZipArchive();
    ZipArchive(const ZipArchive&) = delete;
    ZipArchive& operator=(const ZipArchive&) = delete;

    bool OpenFile(const std::wstring& path);
    bool OpenStream(IStream* stream, uint64_t size);

    int Count() const;
    std::string Name(int index) const;
    uint64_t UncompressedSize(int index) const;
    // Case-insensitive lookup; accepts "/3D/3dmodel.model", "3D/3dmodel.model", percent-encoded names.
    int Find(const std::string& name) const;
    bool Extract(int index, std::vector<char>& out, uint64_t maxSize = 1ull << 31) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::string NormalizeZipPath(const std::string& name); // strip leading '/', decode %XX, lower-case

} // namespace ct
