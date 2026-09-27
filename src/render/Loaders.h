#pragma once
#include "render/Mesh.h"

#include <string>

namespace ct {

bool LoadStl(const std::wstring& path, Mesh& mesh, std::string& error);
bool Load3mf(const std::wstring& path, Mesh& mesh, std::string& error);

struct StepInfo {
    std::string originatingSystem; // from the STEP header, used for the "auto" up axis
    unsigned long long readMs = 0, transferMs = 0, meshMs = 0, collectMs = 0;
};
// size: target thumbnail size in pixels (controls tessellation density).
bool LoadStep(const std::wstring& path, int size, double quality, Mesh& mesh, StepInfo& info, std::string& error);

// Reads FILE_NAME originating_system / preprocessor from the STEP header (first 64 KB).
std::string ReadStepOriginatingSystem(const std::wstring& path);

} // namespace ct
