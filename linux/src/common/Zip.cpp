#include "common/Zip.h"

#include <miniz.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace ct {

struct ZipArchive::Impl {
    mz_zip_archive zip{};
    bool open = false;
    FILE* file = nullptr;
    std::unordered_map<std::string, int> index; // normalized name -> file index

    ~Impl() {
        if (open) mz_zip_reader_end(&zip);
        if (file) fclose(file);
    }

    void BuildIndex() {
        mz_uint n = mz_zip_reader_get_num_files(&zip);
        for (mz_uint i = 0; i < n; ++i) {
            char name[1024];
            mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
            index.emplace(NormalizeZipPath(name), (int)i);
        }
    }
};

std::string NormalizeZipPath(const std::string& in) {
    std::string s;
    s.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '%' && i + 2 < in.size() && isxdigit((unsigned char)in[i + 1]) && isxdigit((unsigned char)in[i + 2])) {
            c = (char)strtol(in.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        }
        if (c == '\\') c = '/';
        s.push_back(c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c);
    }
    size_t start = 0;
    while (start < s.size() && s[start] == '/') ++start;
    return s.substr(start);
}

ZipArchive::ZipArchive() : impl_(std::make_unique<Impl>()) {}
ZipArchive::~ZipArchive() = default;

bool ZipArchive::OpenFile(const std::string& path) {
    impl_ = std::make_unique<Impl>();
    impl_->file = fopen(path.c_str(), "rb");
    if (!impl_->file) return false;
    if (fseeko(impl_->file, 0, SEEK_END) != 0) return false;
    mz_uint64 size = (mz_uint64)ftello(impl_->file);
    fseeko(impl_->file, 0, SEEK_SET);
    if (!mz_zip_reader_init_cfile(&impl_->zip, impl_->file, size, 0)) return false;
    impl_->open = true;
    impl_->BuildIndex();
    return true;
}

int ZipArchive::Count() const { return impl_->open ? (int)mz_zip_reader_get_num_files(&impl_->zip) : 0; }

std::string ZipArchive::Name(int index) const {
    char name[1024] = {};
    if (impl_->open) mz_zip_reader_get_filename(&impl_->zip, (mz_uint)index, name, sizeof(name));
    return name;
}

uint64_t ZipArchive::UncompressedSize(int index) const {
    mz_zip_archive_file_stat st{};
    if (!impl_->open || !mz_zip_reader_file_stat(&impl_->zip, (mz_uint)index, &st)) return 0;
    return st.m_uncomp_size;
}

int ZipArchive::Find(const std::string& name) const {
    auto it = impl_->index.find(NormalizeZipPath(name));
    return it == impl_->index.end() ? -1 : it->second;
}

bool ZipArchive::Extract(int index, std::vector<char>& out, uint64_t maxSize) const {
    if (!impl_->open || index < 0 || index >= Count()) return false;
    uint64_t size = UncompressedSize(index);
    if (size > maxSize) return false;
    out.resize((size_t)size);
    if (size == 0) return true;
    return mz_zip_reader_extract_to_mem(&impl_->zip, (mz_uint)index, out.data(), out.size(), 0) != 0;
}

} // namespace ct
