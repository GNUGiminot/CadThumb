#pragma once
#include "common/Zip.h"

#include <string>
#include <vector>

namespace ct {

struct OpcRelationship {
    std::string type;
    std::string target;
};

// Parse OPC relationships with XML entity and encoding support; ignore external targets.
std::vector<OpcRelationship> ParseRels(const std::vector<char>& xml);

// Path of the root 3D model part (default "3D/3dmodel.model").
std::string Find3mfRootModel(const ZipArchive& zip);

// Thumbnail image saved by a slicer / CAD package (PNG or JPEG bytes). Returns false if none.
bool Find3mfThumbnail(const ZipArchive& zip, std::vector<char>& imageBytes);

} // namespace ct
