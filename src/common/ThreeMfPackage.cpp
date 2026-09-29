#include "common/ThreeMfPackage.h"

#include <algorithm>
#include <cstring>
#include <pugixml.hpp>

namespace ct {

std::vector<OpcRelationship> ParseRels(const std::vector<char>& xmlBytes) {
    std::vector<OpcRelationship> out;
    pugi::xml_document doc;
    if (!doc.load_buffer(xmlBytes.data(), xmlBytes.size())) return out;
    for (auto node : doc.document_element().children()) {
        const char* name = strrchr(node.name(), ':');
        name = name ? name + 1 : node.name();
        if (strcmp(name, "Relationship") || !strcmp(node.attribute("TargetMode").value(), "External")) continue;
        OpcRelationship r{node.attribute("Type").value(), node.attribute("Target").value()};
        if (!r.target.empty()) out.push_back(std::move(r));
    }
    return out;
}

static bool EndsWith(const std::string& s, const char* suffix) {
    size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

static std::string Lower(std::string s) {
    for (auto& c : s) c = (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
    return s;
}

std::string Find3mfRootModel(const ZipArchive& zip) {
    std::vector<char> rels;
    int idx = zip.Find("_rels/.rels");
    if (idx >= 0 && zip.Extract(idx, rels, 4 << 20)) {
        for (const auto& r : ParseRels(rels)) {
            if (Lower(r.type).find("3dmanufacturing") != std::string::npos &&
                EndsWith(Lower(r.type), "/3dmodel") && zip.Find(r.target) >= 0)
                return r.target;
        }
    }
    if (zip.Find("3D/3dmodel.model") >= 0) return "3D/3dmodel.model";
    // any *.model in 3D/
    for (int i = 0; i < zip.Count(); ++i) {
        std::string n = NormalizeZipPath(zip.Name(i));
        if (EndsWith(n, ".model")) return n;
    }
    return {};
}

static bool TryExtractImage(const ZipArchive& zip, const std::string& name, std::vector<char>& out) {
    int idx = zip.Find(name);
    if (idx < 0) return false;
    if (zip.UncompressedSize(idx) < 64) return false;
    return zip.Extract(idx, out, 64 << 20);
}

bool Find3mfThumbnail(const ZipArchive& zip, std::vector<char>& out) {
    // 1. package-level thumbnail relationship (3MF core spec)
    std::vector<char> rels;
    int idx = zip.Find("_rels/.rels");
    if (idx >= 0 && zip.Extract(idx, rels, 4 << 20)) {
        for (const auto& r : ParseRels(rels)) {
            if (EndsWith(Lower(r.type), "/metadata/thumbnail") && TryExtractImage(zip, r.target, out)) return true;
        }
    }
    // 2. well-known names used by slicers (PrusaSlicer, Bambu Studio, Orca, Cura, QIDI Studio...)
    static const char* known[] = {"Metadata/thumbnail.png", "Metadata/plate_1.png", "Metadata/thumbnail.jpg",
                                  "Metadata/thumbnail.jpeg", "Metadata/top_1.png", "Thumbnails/thumbnail.png"};
    for (const char* k : known)
        if (TryExtractImage(zip, k, out)) return true;
    // 3. any image in Metadata/ except masks and helper pictures
    for (int i = 0; i < zip.Count(); ++i) {
        std::string n = NormalizeZipPath(zip.Name(i));
        if (n.rfind("metadata/", 0) != 0) continue;
        if (!(EndsWith(n, ".png") || EndsWith(n, ".jpg") || EndsWith(n, ".jpeg"))) continue;
        if (n.find("pick") != std::string::npos || n.find("no_light") != std::string::npos ||
            n.find("_small") != std::string::npos)
            continue;
        if (TryExtractImage(zip, n, out)) return true;
    }
    return false;
}

} // namespace ct
