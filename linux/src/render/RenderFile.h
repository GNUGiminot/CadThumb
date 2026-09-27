#pragma once
#include "common/FileType.h"
#include "common/ImageIO.h"
#include "common/Settings.h"

#include <string>

namespace ct {

// Loads the model file and renders a size x size thumbnail.
bool RenderFileToImage(const std::string& path, FileType type, int size, const Settings& settings, Image& out,
                       std::string& error, std::string* details = nullptr);

} // namespace ct
