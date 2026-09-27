#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace ct {

enum class FileType : uint32_t { Unknown = 0, Step = 1, ThreeMf = 2, Stl = 3 };

FileType FileTypeFromExtension(const std::string& ext); // ".step" -> Step
FileType FileTypeSniff(const uint8_t* head, size_t len); // by content
const char* FileTypeName(FileType t);                    // "step"

struct Settings;
bool FileTypeEnabled(FileType t, const Settings& s);

} // namespace ct
