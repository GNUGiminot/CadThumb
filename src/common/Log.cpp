#include "common/Log.h"
#include "common/Paths.h"
#include "common/Settings.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace ct {

static const wchar_t* g_tag = L"?";

void SetLogTag(const wchar_t* tag) { g_tag = tag; }

static void WriteLine(const wchar_t* fmt, va_list args) {
    wchar_t msg[2048];
    _vsnwprintf_s(msg, _TRUNCATE, fmt, args);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[2300];
    _snwprintf_s(line, _TRUNCATE, L"%04u-%02u-%02u %02u:%02u:%02u.%03u [%s %lu] %s\r\n", st.wYear, st.wMonth,
                 st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, g_tag, GetCurrentProcessId(), msg);
    std::string u8 = Utf8(line);

    std::wstring path = LogPath();
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) && fad.nFileSizeLow > 2 * 1024 * 1024) {
        std::wstring old = path + L".old";
        MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(f, &size) && size.QuadPart == 0) WriteFile(f, "\xEF\xBB\xBF", 3, &w, nullptr); // UTF-8 BOM
    WriteFile(f, u8.data(), (DWORD)u8.size(), &w, nullptr);
    CloseHandle(f);
}

void Log(const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    WriteLine(fmt, args);
    va_end(args);
}

void LogVerbose(const wchar_t* fmt, ...) {
    if (!Settings::Get().verboseLog) return;
    va_list args;
    va_start(args, fmt);
    WriteLine(fmt, args);
    va_end(args);
}

} // namespace ct
