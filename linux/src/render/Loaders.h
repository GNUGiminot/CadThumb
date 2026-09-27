#pragma once
#include "render/Mesh.h"

#include <string>

namespace ct {

bool LoadStl(const std::string& path, Mesh& mesh, std::string& error);
bool Load3mf(const std::string& path, Mesh& mesh, std::string& error);

struct StepInfo {
    std::string originatingSystem; // from the STEP header, used for the "auto" up axis
    long long readMs = 0, transferMs = 0, meshMs = 0, collectMs = 0;
};
// size: target thumbnail size in pixels (controls tessellation density).
bool LoadStep(const std::string& path, int size, double quality, Mesh& mesh, StepInfo& info, std::string& error);

} // namespace ct
