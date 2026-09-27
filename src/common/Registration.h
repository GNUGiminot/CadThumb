#pragma once
#include <string>
#include <vector>

namespace ct {

struct RegisterOptions {
    bool machine = false;     // HKLM (all users, needs admin) instead of HKCU
    bool includeStl = false;  // STL is off by default: usually another handler is installed
    bool contextMenu = true;  // "Обновить эскиз (CadThumb)" verb
    bool autostart = true;    // start the host at logon
    std::wstring dllPath;
    std::wstring exePath;
};

bool RegisterShellExtension(const RegisterOptions& opt, std::wstring& report);
bool UnregisterShellExtension(bool machine, std::wstring& report);

bool IsAutostartEnabled();
bool SetAutostart(bool enable, const std::wstring& exePath);

std::vector<std::wstring> HandledExtensions(bool includeStl);

// Describes who currently provides thumbnails for each extension (for diagnostics).
std::wstring DescribeThumbnailHandlers();

} // namespace ct
