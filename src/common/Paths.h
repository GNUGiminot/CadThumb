#pragma once
#include <windows.h>
#include <string>

namespace ct {

// {77C009E1-FE4D-4FFE-BBAE-BAB9216EF31C}
inline constexpr CLSID CLSID_CadThumbProvider =
    {0x77c009e1, 0xfe4d, 0x4ffe, {0xbb, 0xae, 0xba, 0xb9, 0x21, 0x6e, 0xf3, 0x1c}};
inline constexpr wchar_t kClsidString[] = L"{77C009E1-FE4D-4FFE-BBAE-BAB9216EF31C}";
inline constexpr wchar_t kThumbnailHandlerKey[] = L"{e357fccd-a995-4576-b01f-234630154e96}";
inline constexpr wchar_t kExeName[] = L"CadThumb.exe";
inline constexpr wchar_t kDllName[] = L"CadThumbShell.dll";
inline constexpr wchar_t kHostWindowClass[] = L"CadThumbHostWindow";

std::wstring AppDataDir();   // %LOCALAPPDATA%\CadThumb
std::wstring CacheDir();     // ...\cache
std::wstring TempDir();      // ...\tmp
std::wstring SettingsPath(); // ...\settings.ini
std::wstring LogPath();      // ...\cadthumb.log

std::wstring ModulePath(HMODULE module);
std::wstring DirOf(const std::wstring& path);
std::wstring FileNameOf(const std::wstring& path);
std::wstring ExtOf(const std::wstring& path); // lower case, with dot
std::wstring ToLower(std::wstring s);

std::string Utf8(const std::wstring& w);
std::wstring Wide(const std::string& s);

std::wstring CurrentUserSid();
std::wstring PipeName();        // \\.\pipe\CadThumb.<sid>
std::wstring HostMutexName();

bool FileExists(const std::wstring& path);
bool EnsureDir(const std::wstring& path);

} // namespace ct
