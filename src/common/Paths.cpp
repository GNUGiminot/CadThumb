#include "common/Paths.h"

#include <shlobj.h>
#include <sddl.h>
#include <algorithm>
#include <cwctype>

namespace ct {

static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p)) && p) r = p;
    CoTaskMemFree(p);
    return r;
}

bool EnsureDir(const std::wstring& path) {
    if (path.empty()) return false;
    DWORD a = GetFileAttributesW(path.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    int rc = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    return rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS || rc == ERROR_FILE_EXISTS;
}

std::wstring AppDataDir() {
    static const std::wstring dir = [] {
        std::wstring d = KnownFolder(FOLDERID_LocalAppData);
        if (d.empty()) {
            wchar_t buf[MAX_PATH]{};
            GetTempPathW(MAX_PATH, buf);
            d = buf;
        }
        if (!d.empty() && d.back() != L'\\') d += L'\\';
        d += L"CadThumb";
        EnsureDir(d);
        return d;
    }();
    return dir;
}

std::wstring CacheDir() {
    std::wstring d = AppDataDir() + L"\\cache";
    EnsureDir(d);
    return d;
}

std::wstring TempDir() {
    std::wstring d = AppDataDir() + L"\\tmp";
    EnsureDir(d);
    return d;
}

std::wstring SettingsPath() { return AppDataDir() + L"\\settings.ini"; }
std::wstring LogPath() { return AppDataDir() + L"\\cadthumb.log"; }

std::wstring ModulePath(HMODULE module) {
    std::wstring buf(1024, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(module, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

std::wstring DirOf(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? std::wstring() : path.substr(0, p);
}

std::wstring FileNameOf(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

std::wstring ToLower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

std::wstring ExtOf(const std::wstring& path) {
    std::wstring name = FileNameOf(path);
    size_t p = name.find_last_of(L'.');
    return p == std::wstring::npos ? std::wstring() : ToLower(name.substr(p));
}

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::wstring CurrentUserSid() {
    static const std::wstring sid = [] {
        std::wstring r = L"unknown";
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return r;
        DWORD len = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &len);
        std::string buf(len, '\0');
        if (len && GetTokenInformation(token, TokenUser, buf.data(), len, &len)) {
            auto* tu = reinterpret_cast<TOKEN_USER*>(buf.data());
            LPWSTR str = nullptr;
            if (ConvertSidToStringSidW(tu->User.Sid, &str)) {
                r = str;
                LocalFree(str);
            }
        }
        CloseHandle(token);
        return r;
    }();
    return sid;
}

std::wstring PipeName() { return L"\\\\.\\pipe\\CadThumb." + CurrentUserSid(); }
std::wstring HostMutexName() { return L"Local\\CadThumb.Host." + CurrentUserSid(); }

bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

} // namespace ct
