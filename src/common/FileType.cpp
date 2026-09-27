#include "common/FileType.h"
#include "common/Settings.h"

#include <cstring>

namespace ct {

FileType FileTypeFromExtension(const std::wstring& ext) {
    if (ext == L".step" || ext == L".stp" || ext == L".p21") return FileType::Step;
    if (ext == L".3mf") return FileType::ThreeMf;
    if (ext == L".stl") return FileType::Stl;
    return FileType::Unknown;
}

FileType FileTypeSniff(const uint8_t* head, size_t len) {
    if (len >= 4 && head[0] == 'P' && head[1] == 'K' && head[2] == 3 && head[3] == 4) return FileType::ThreeMf;
    // STEP: "ISO-10303-21;" possibly after BOM / whitespace
    size_t i = 0;
    if (len >= 3 && head[0] == 0xEF && head[1] == 0xBB && head[2] == 0xBF) i = 3;
    while (i < len && (head[i] == ' ' || head[i] == '\r' || head[i] == '\n' || head[i] == '\t')) ++i;
    if (len - i >= 12 && memcmp(head + i, "ISO-10303-21", 12) == 0) return FileType::Step;
    if (len >= 84) return FileType::Stl; // binary STL has no signature; ASCII starts with "solid"
    if (len - i >= 5 && memcmp(head + i, "solid", 5) == 0) return FileType::Stl;
    return FileType::Unknown;
}

const wchar_t* FileTypeName(FileType t) {
    switch (t) {
    case FileType::Step: return L"step";
    case FileType::ThreeMf: return L"3mf";
    case FileType::Stl: return L"stl";
    default: return L"unknown";
    }
}

const wchar_t* FileTypeExt(FileType t) {
    switch (t) {
    case FileType::Step: return L".step";
    case FileType::ThreeMf: return L".3mf";
    case FileType::Stl: return L".stl";
    default: return L".bin";
    }
}

FileType FileTypeFromName(const std::wstring& name) {
    if (name == L"step" || name == L"stp") return FileType::Step;
    if (name == L"3mf") return FileType::ThreeMf;
    if (name == L"stl") return FileType::Stl;
    return FileType::Unknown;
}

bool FileTypeEnabled(FileType t, const Settings& s) {
    switch (t) {
    case FileType::Step: return s.enableStep;
    case FileType::ThreeMf: return s.enable3mf;
    case FileType::Stl: return s.enableStl;
    default: return false;
    }
}

} // namespace ct
