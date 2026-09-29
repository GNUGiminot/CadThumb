#include "render/RenderFile.h"

#include "common/ThreeMfPackage.h"
#include "common/Zip.h"
#include "render/Loaders.h"
#include "render/Rasterizer.h"

#include <chrono>
#include <cstdio>
#include <algorithm>
#include <sys/stat.h>

namespace ct {

namespace {
long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

char AutoUpAxis(const std::string& originatingSystem) {
    // CAD systems whose default "top" is +Y. Everything else (CATIA, NX, FreeCAD, Onshape, KOMPAS,
    // Fusion 360 in Z-up mode, slicers...) is treated as Z-up.
    static const char* yUp[] = {"solidworks", "inventor", "creo", "pro/engineer", "proe", "spaceclaim"};
    for (const char* s : yUp)
        if (originatingSystem.find(s) != std::string::npos) return 'Y';
    return 'Z';
}
} // namespace

bool RenderFileToImage(const std::string& path, FileType type, int size, const Settings& s, Image& out,
                       std::string& error, std::string* details) {
    size = std::clamp(size, 16, 2048);
    const long long t0 = NowMs();
    if (type == FileType::ThreeMf && s.prefer3mfEmbedded) {
        ZipArchive zip;
        std::vector<char> bytes;
        Image img;
        if (zip.OpenFile(path) && Find3mfThumbnail(zip, bytes) && DecodeImage(bytes.data(), bytes.size(), img)) {
            out = ScaleToFit(img, size);
            if (details) *details = "embedded thumbnail";
            return true;
        }
    }
    struct stat fileInfo{};
    if (stat(path.c_str(), &fileInfo) == 0 && fileInfo.st_size >
        int64_t(s.maxFileSizeMB) * 1024 * 1024) {
        error = "file exceeds MaxFileSizeMB";
        return false;
    }

    Mesh mesh;
    RenderParams prm;
    prm.size = size;
    prm.ssaa = size <= 256 ? 3 : 2;
    prm.yawDeg = s.yawDeg;
    prm.pitchDeg = s.pitchDeg;
    prm.outline = s.outline;
    prm.background = s.background;
    prm.upAxis = 'Z';
    std::string phases;

    switch (type) {
    case FileType::ThreeMf:
        mesh.defaultColor = s.color3mf;
        if (!Load3mf(path, mesh, error)) return false;
        break;
    case FileType::Stl:
        mesh.defaultColor = s.colorStl;
        if (!LoadStl(path, mesh, error)) return false;
        break;
    case FileType::Step: {
        mesh.defaultColor = s.colorStep;
        StepInfo info;
        if (!LoadStep(path, size, s.quality, mesh, info, error)) return false;
        prm.upAxis = s.upAxisStep == 'Y' ? 'Y' : s.upAxisStep == 'Z' ? 'Z' : AutoUpAxis(info.originatingSystem);
        if (details) {
            char b[160];
            snprintf(b, sizeof(b), " [read %lld, transfer %lld, mesh %lld, collect %lld ms]", info.readMs,
                     info.transferMs, info.meshMs, info.collectMs);
            phases = b;
        }
        break;
    }
    default:
        error = "unsupported file type";
        return false;
    }

    const long long t1 = NowMs();
    if (!RenderMesh(mesh, prm, out, error)) return false;
    const long long t2 = NowMs();
    if (details) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%zu triangles, %zu lines, up=%c, load %lld ms, raster %lld ms",
                 mesh.TriangleCount(), mesh.lines.size() / 6, prm.upAxis, t1 - t0, t2 - t1);
        *details = buf + phases;
    }
    return true;
}

} // namespace ct
