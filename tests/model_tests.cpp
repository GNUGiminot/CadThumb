#include "render/Loaders.h"
#include "viewer/ViewModel.h"
#include "common/Cache.h"

#include <miniz.h>
#include <windows.h>
#include <shlwapi.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        fprintf(stderr, "FAIL: %s\n", message);
    }
}

bool WriteBytes(const std::wstring& path, const void* data, size_t size) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, size, f) == size;
    fclose(f);
    return ok;
}

bool Write3mf(const std::wstring& path, const std::string& model,
              const std::string& modelName = "3D/3dmodel.model",
              const std::string& child = "") {
    mz_zip_archive zip{};
    std::string narrow;
    for (wchar_t c : path) {
        if (c > 127) return false; // miniz's narrow writer is only used for ASCII test paths
        narrow.push_back(char(c));
    }
    if (!mz_zip_writer_init_file(&zip, narrow.c_str(), 0)) return false;
    const std::string rels =
        "<Relationships xmlns='http://schemas.openxmlformats.org/package/2006/relationships'>"
        "<Relationship Id='r1' Type='http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel' "
        "Target='/3D/3dmodel.model'/></Relationships>";
    bool ok = mz_zip_writer_add_mem(&zip, "_rels/.rels", rels.data(), rels.size(), MZ_DEFAULT_COMPRESSION) &&
              mz_zip_writer_add_mem(&zip, modelName.c_str(), model.data(), model.size(), MZ_DEFAULT_COMPRESSION);
    if (ok && !child.empty())
        ok = mz_zip_writer_add_mem(&zip, "3D/child.model", child.data(), child.size(), MZ_DEFAULT_COMPRESSION);
    if (ok) ok = mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    return ok;
}

std::string Model(const std::string& unit, const std::string& triangles,
                  const std::string& build, const std::string& more = "") {
    return "<?xml version='1.0'?><model unit='" + unit + "' xmlns='http://schemas.microsoft.com/3dmanufacturing/core/2015/02'>"
           "<resources><object id='1' type='model'><mesh><vertices>"
           "<vertex x='0' y='0' z='0'/><vertex x='1' y='0' z='0'/>"
           "<vertex x='0' y='1' z='0'/><vertex x='0' y='0' z='1'/>"
           "</vertices><triangles>" + triangles + "</triangles></mesh></object>" + more +
           "</resources>" + build + "</model>";
}

const char* tetra =
    "<triangle v1='0' v2='2' v3='1'/><triangle v1='0' v2='1' v3='3'/>"
    "<triangle v1='0' v2='3' v3='2'/><triangle v1='1' v2='2' v3='3'/>";

bool Near(double a, double b) { return std::fabs(a - b) < 0.02; }

void Test3mf(const std::wstring& dir) {
    std::string err;
    ct::Mesh mesh;
    std::wstring file = dir + L"\\inches.3mf";
    std::string build = "<build><item objectid='1'/><item objectid='1' "
        "transform='-1 0 0 0 1 0 0 0 1 3 0 0'/></build>";
    Check(Write3mf(file, Model("inch", tetra, build)), "create inch fixture");
    Check(ct::Load3mf(file, mesh, err), "load inch and reflected 3MF");
    if (mesh.TriangleCount() == 8) {
        ct::ViewModel view;
        ct::BuildViewModel(std::move(mesh), 'Z', 30.0f, view);
        Check(Near(view.bmax[0], 76.2), "inch coordinates convert to millimeters");
        Check(Near(view.volume, std::pow(25.4, 3) / 3), "reflected component retains positive volume");
    } else Check(false, "two tetrahedra were instantiated");

    mesh = {};
    file = dir + L"\\empty-build.3mf";
    Check(Write3mf(file, Model("millimeter", tetra, "<build/>")), "create empty-build fixture");
    Check(!ct::Load3mf(file, mesh, err), "empty build does not render unreferenced resources");

    mesh = {};
    file = dir + L"\\bad-index.3mf";
    Check(Write3mf(file, Model("millimeter", "<triangle v1='0' v2='1' v3='9'/>",
                                 "<build><item objectid='1'/></build>")), "create invalid-index fixture");
    Check(!ct::Load3mf(file, mesh, err), "invalid triangle index is rejected");

    mesh = {};
    file = dir + L"\\cycle.3mf";
    std::string cycle = "<object id='2' type='model'><components><component objectid='2'/></components></object>";
    Check(Write3mf(file, Model("millimeter", tetra, "<build><item objectid='2'/></build>", cycle)),
          "create cyclic fixture");
    Check(!ct::Load3mf(file, mesh, err), "cyclic components are rejected");

    mesh = {};
    file = dir + L"\\child-unit.3mf";
    const std::string root = "<model unit='inch' xmlns='http://schemas.microsoft.com/3dmanufacturing/core/2015/02'>"
        "<resources/><build><item objectid='1' path='/3D/child.model'/></build></model>";
    const std::string child = Model("centimeter", tetra, "<build/>");
    Check(Write3mf(file, root, "3D/3dmodel.model", child), "create multi-part unit fixture");
    Check(ct::Load3mf(file, mesh, err), "load referenced model in different unit");
    if (!mesh.pos.empty()) {
        ct::ViewModel view;
        ct::BuildViewModel(std::move(mesh), 'Z', 30.0f, view);
        Check(Near(view.bmax[0], 10.0), "child model unit controls its vertices");
    }
}

void TestStl(const std::wstring& dir) {
    const std::string ascii = "\xef\xbb\xbf solid valid\nfacet normal 0 0 1\nouter loop\n"
        "vertex +0 +0 +0\nvertex +1 0 0\nvertex 0 +1 0\nendloop\nendfacet\nendsolid\n";
    std::wstring file = dir + L"\\ascii.stl";
    Check(WriteBytes(file, ascii.data(), ascii.size()), "create ASCII STL");
    ct::Mesh mesh;
    std::string err;
    Check(ct::LoadStl(file, mesh, err) && mesh.TriangleCount() == 1, "ASCII STL BOM and plus sign");

    std::vector<char> binary(84 + 50, 0);
    uint32_t count = 2; // the second facet is missing
    memcpy(binary.data() + 80, &count, sizeof(count));
    file = dir + L"\\truncated.stl";
    Check(WriteBytes(file, binary.data(), binary.size()), "create truncated STL");
    mesh = {};
    Check(!ct::LoadStl(file, mesh, err), "truncated binary STL is rejected");
}

void TestCacheKey(const std::wstring& dir) {
    const std::wstring file = dir + L"\\sampled.bin";
    std::vector<char> bytes(1024 * 1024, 'a');
    bytes[bytes.size() / 5 + 100] = 'b';
    Check(WriteBytes(file, bytes.data(), bytes.size()), "create sampled cache fixture");
    std::string fromFile, fromStream;
    Check(ct::CacheKeyFromFile(file, ct::FileType::Stl, 256, 123, fromFile), "compute file cache key");
    IStream* stream = nullptr;
    HRESULT hr = SHCreateStreamOnFileEx(file.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE,
                                        nullptr, &stream);
    Check(SUCCEEDED(hr) && stream, "open stream for cache key");
    if (stream) {
        STATSTG stat{};
        Check(SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)), "stat sampled stream");
        uint64_t mtime = (uint64_t(stat.mtime.dwHighDateTime) << 32) | stat.mtime.dwLowDateTime;
        Check(ct::CacheKeyFromStream(stream, bytes.size(), mtime, ct::FileType::Stl, 256, 123, fromStream),
              "compute stream cache key");
        Check(fromFile == fromStream, "file and stream sample the same offsets");
        stream->Release();
    }
    const std::string before = fromFile;
    bytes[bytes.size() / 5 + 100] = 'c';
    Check(WriteBytes(file, bytes.data(), bytes.size()), "update middle cache sample");
    Check(ct::CacheKeyFromFile(file, ct::FileType::Stl, 256, 123, fromFile) && fromFile != before,
          "middle sample changes cache key");
}

} // namespace

int main() {
    const std::wstring dir = CADTHUMB_TEST_DIR;
    std::filesystem::create_directories(dir);
    Test3mf(dir);
    TestStl(dir);
    TestCacheKey(dir);
    if (!failures) puts("model format checks passed");
    return failures ? 1 : 0;
}
