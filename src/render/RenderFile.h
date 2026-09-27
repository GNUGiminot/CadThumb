#pragma once
#include "common/FileType.h"
#include "common/ImageIO.h"
#include "common/Settings.h"
#include "render/Mesh.h"

#include <string>

namespace ct {

// Loads the model file and renders a size x size thumbnail. Used by `CadThumb.exe --render`.
bool RenderFileToImage(const std::wstring& path, FileType type, int size, const Settings& settings, Image& out,
                       std::string& error, std::string* details = nullptr);

// Loads a model as a triangle mesh. detailSize: target size in pixels, controls STEP tessellation density.
// upAxis receives 'Z' or 'Y' (STEP files from Y-up CAD systems, see UpAxisStep).
bool LoadModelMesh(const std::wstring& path, FileType type, int detailSize, const Settings& settings, Mesh& mesh,
                   char& upAxis, std::string& error, std::string* phases = nullptr);

} // namespace ct
