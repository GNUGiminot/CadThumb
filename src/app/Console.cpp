#include "app/App.h"
#include "common/Paths.h"

#include <cstdio>

namespace ct {

static bool g_console = false;
static std::wstring g_buffer;

void AttachParentConsole() {
    // Output redirected to a file/pipe (e.g. `CadThumb.exe --status > out.txt`): use it as is.
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD type = (h && h != INVALID_HANDLE_VALUE) ? GetFileType(h) : FILE_TYPE_UNKNOWN;
    if (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE) {
        g_console = true;
        return;
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS)) g_console = true;
}

void Out(const std::wstring& text) {
    if (g_console) {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD w = 0;
        if (GetFileType(h) == FILE_TYPE_CHAR) {
            WriteConsoleW(h, text.c_str(), (DWORD)text.size(), &w, nullptr);
        } else {
            std::string u8 = Utf8(text); // redirected to a file/pipe
            WriteFile(h, u8.data(), (DWORD)u8.size(), &w, nullptr);
        }
    }
    g_buffer += text;
}

void FlushOutAsMessageBox(const wchar_t* title, bool error) {
    if (!g_console && !g_buffer.empty())
        MessageBoxW(nullptr, g_buffer.c_str(), title, MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
    g_buffer.clear();
}

std::wstring SelfExePath() { return ModulePath(nullptr); }

} // namespace ct
