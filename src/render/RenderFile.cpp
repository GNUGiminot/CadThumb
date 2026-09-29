#include "render/RenderFile.h"

#include "common/ThreeMfPackage.h"
#include "common/Zip.h"
#include "render/Loaders.h"
#include "render/Rasterizer.h"

#include <windows.h>
#include <cstdio>
#include <algorithm>

namespace ct {

static char AutoUpAxis(const std::string& originatingSystem) {
    // CAD systems whose default "top" is +Y. Everything else (CATIA, NX, FreeCAD, Onshape, KOMPAS,
    // Fusion 360 in Z-up mode, slicers...) is treated as Z-up.
    static const char* yUp[] = {"solidworks", "inventor", "creo", "pro/engineer", "proe", "spaceclaim"};
    for (const char* s : yUp)
        if (originatingSystem.find(s) != std::string::npos) return 'Y';
    return 'Z';
}

bool LoadModelMesh(const std::wstring& path, FileType type, int detailSize, const Settings& s, Mesh& mesh,
                   char& upAxis, std::string& error, std::string* phases) {
    upAxis = 'Z';
    switch (type) {
    case FileType::ThreeMf:
        mesh.defaultColor = s.color3mf;
        return Load3mf(path, mesh, error);
    case FileType::Stl:
        mesh.defaultColor = s.colorStl;
        return LoadStl(path, mesh, error);
    case FileType::Step: {
#ifdef CADTHUMB_WITH_STEP
        mesh.defaultColor = s.colorStep;
        StepInfo info;
        if (!LoadStep(path, detailSize, s.quality, mesh, info, error)) return false;
        upAxis = s.upAxisStep == L'Y' ? 'Y' : s.upAxisStep == L'Z' ? 'Z' : AutoUpAxis(info.originatingSystem);
        if (phases) {
            char b[160];
            snprintf(b, sizeof(b), " [read %llu, transfer %llu, mesh %llu, collect %llu ms]", info.readMs,
                     info.transferMs, info.meshMs, info.collectMs);
            *phases = b;
        }
        return true;
#else
        error = "built without STEP support";
        return false;
#endif
    }
    default:
        error = "unsupported file type";
        return false;
    }
}

bool RenderFileToImage(const std::wstring& path, FileType type, int size, const Settings& s, Image& out,
                       std::string& error, std::string* details) {
    size = std::clamp(size, 16, 2048);
    const ULONGLONG t0 = GetTickCount64();
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
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) &&
        ((uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow) >
            uint64_t(s.maxFileSizeMB) * 1024 * 1024) {
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
    std::string phases;
    if (!LoadModelMesh(path, type, size, s, mesh, prm.upAxis, error, &phases)) return false;

    const ULONGLONG t1 = GetTickCount64();
    if (!RenderMesh(mesh, prm, out, error)) return false;
    const ULONGLONG t2 = GetTickCount64();
    if (details) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%zu triangles, %zu lines, up=%c, load %llu ms, raster %llu ms",
                 mesh.TriangleCount(), mesh.lines.size() / 6, prm.upAxis, t1 - t0, t2 - t1);
        *details = buf + phases;
    }
    return true;
}

} // namespace ct
