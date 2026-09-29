// CadThumbSetup.exe — single-file installer/uninstaller. CadThumb.exe and CadThumbShell.dll are embedded
// as RCDATA resources; registration itself is delegated to `CadThumb.exe --register/--unregister`.
//
//   CadThumbSetup.exe                         install or update, including STL by default
//   CadThumbSetup.exe /S [/NOSTL] [/MACHINE] [/NOMENU] [/NOAUTOSTART] [/RESTARTEXPLORER]
//   CadThumbSetup.exe /uninstall [/S] [/PURGE]                           uninstall (/PURGE: settings + cache too)
#include "common/ExplorerRestart.h"
#include "common/Paths.h"
#include "common/Registration.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <restartmanager.h>
#include <filesystem>
#include <string>
#include <vector>
#include <cwchar>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace ct;
#pragma comment(lib, "rstrtmgr.lib")

namespace {

constexpr WORD kResExe = 101;
constexpr WORD kResDll = 102;
constexpr wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\CadThumb";
constexpr wchar_t kTitle[] = L"CadThumb";

struct Options {
    bool silent = false, uninstall = false, purge = false, confirmed = false;
    bool machine = false, stl = true, noMenu = false, noAutostart = false, restartExplorer = false;
    bool scopeExplicit = false, stlExplicit = false, menuExplicit = false, autostartExplicit = false;
};

struct InstalledState {
    bool present = false, hasEntry = false, machine = false, stl = false, noMenu = false, noAutostart = false;
    std::wstring version, dir;
};

// Restart Manager closes only applications holding the old program files.
struct UpdateSession {
    DWORD id = 0;
    bool started = false, stopped = false;
    void Finish() {
        if (started) {
            if (stopped) RmRestart(id, 0, nullptr);
            RmEndSession(id);
            started = false;
        }
    }
    ~UpdateSession() { Finish(); }
};

bool RegString(HKEY root, const wchar_t* subkey, const wchar_t* name, std::wstring& value) {
    wchar_t buf[4096]{};
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, subkey, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                     nullptr, buf, &size) != ERROR_SUCCESS) return false;
    value = buf;
    return true;
}

bool RegDword(HKEY root, const wchar_t* subkey, const wchar_t* name, DWORD& value) {
    DWORD size = sizeof(value);
    return RegGetValueW(root, subkey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS;
}

bool HasExtension(HKEY root, const wchar_t* ext) {
    DWORD size = 0;
    if (RegGetValueW(root, L"Software\\CadThumb", L"Extensions", RRF_RT_REG_MULTI_SZ,
                     nullptr, nullptr, &size) != ERROR_SUCCESS || size < 2 * sizeof(wchar_t) || size > 65536) return false;
    std::vector<wchar_t> names(size / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(root, L"Software\\CadThumb", L"Extensions", RRF_RT_REG_MULTI_SZ,
                     nullptr, names.data(), &size) != ERROR_SUCCESS) return false;
    const wchar_t* end = names.data() + size / sizeof(wchar_t);
    for (const wchar_t* p = names.data(); p < end && *p;) {
        size_t left = end - p;
        size_t len = wcsnlen_s(p, left);
        if (len == left) break;
        if (_wcsicmp(p, ext) == 0) return true;
        p += len + 1;
    }
    return false;
}

InstalledState ReadInstalled(bool machine) {
    InstalledState s;
    s.machine = machine;
    HKEY root = machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    std::wstring displayName;
    std::wstring installDir;
    bool hasEntry = RegString(root, kUninstallKey, L"DisplayName", displayName);
    bool hasState = RegString(root, L"Software\\CadThumb", L"InstallDir", installDir);
    s.present = hasEntry || hasState;
    s.hasEntry = hasEntry;
    if (!s.present) return s;
    RegString(root, kUninstallKey, L"DisplayVersion", s.version);
    s.dir = installDir;
    if (s.dir.empty()) {
        std::wstring icon;
        if (RegString(root, kUninstallKey, L"DisplayIcon", icon)) {
            size_t comma = icon.rfind(L",0");
            if (comma != std::wstring::npos && comma + 2 == icon.size()) icon.resize(comma);
            s.dir = DirOf(icon);
        }
    }
    DWORD setting = 0;
    s.stl = RegDword(root, kUninstallKey, L"StlEnabled", setting)
        ? setting != 0 : HasExtension(root, L".stl");
    s.noMenu = RegDword(root, kUninstallKey, L"ContextMenuEnabled", setting)
        ? setting == 0 : !RegString(root, L"Software\\Classes\\SystemFileAssociations\\.step\\shell\\CadThumb.View",
                                    nullptr, displayName);
    s.noAutostart = RegDword(root, kUninstallKey, L"AutostartEnabled", setting)
        ? setting == 0 : !RegString(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"CadThumb", displayName);
    return s;
}

void KeepPreviousChoices(Options& o, const InstalledState& existing) {
    if (!existing.present) return;
    o.machine = existing.machine;
    if (!o.stlExplicit) o.stl = existing.stl;
    if (!o.menuExplicit) o.noMenu = existing.noMenu;
    if (!o.autostartExplicit) o.noAutostart = existing.noAutostart;
}

int CompareVersions(const std::wstring& installed, const wchar_t* incoming) {
    unsigned a[3]{}, b[3]{};
    if (swscanf_s(installed.c_str(), L"%u.%u.%u", &a[0], &a[1], &a[2]) != 3 ||
        swscanf_s(incoming, L"%u.%u.%u", &b[0], &b[1], &b[2]) != 3) return 0;
    for (int i = 0; i < 3; ++i) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p)) && p) r = p;
    CoTaskMemFree(p);
    return r;
}

std::wstring InstallRoot(bool machine) {
    return KnownFolder(machine ? FOLDERID_ProgramFilesX64 : FOLDERID_UserProgramFiles) + L"\\CadThumb";
}

bool CloseInstalledApplications(bool silent, UpdateSession& session, std::wstring& error) {
    std::vector<std::wstring> files;
    for (bool machine : {false, true}) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(InstallRoot(machine), ec)) {
            DWORD attr = GetFileAttributesW(entry.path().c_str());
            if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY) ||
                (attr & FILE_ATTRIBUTE_REPARSE_POINT) || entry.path().filename().wstring().rfind(L"app-", 0) != 0) continue;
            for (const wchar_t* name : {L"CadThumb.exe", L"CadThumbShell.dll"}) {
                auto path = entry.path() / name;
                if (FileExists(path.wstring())) files.push_back(path.wstring());
            }
        }
    }
    if (files.empty()) return true;
    wchar_t key[CCH_RM_SESSION_KEY + 1]{};
    if (RmStartSession(&session.id, 0, key) != ERROR_SUCCESS) {
        error = L"Не удалось проверить запущенные приложения.";
        return false;
    }
    session.started = true;
    std::vector<LPCWSTR> resources;
    for (const auto& file : files) resources.push_back(file.c_str());
    if (RmRegisterResources(session.id, UINT(resources.size()), resources.data(), 0, nullptr, 0, nullptr) != ERROR_SUCCESS) {
        error = L"Не удалось проверить занятые файлы CadThumb.";
        return false;
    }
    UINT needed = 0, count = 0;
    DWORD reasons = 0;
    DWORD result = RmGetList(session.id, &needed, &count, nullptr, &reasons);
    if (result == ERROR_SUCCESS && needed == 0) return true;
    if (result != ERROR_MORE_DATA) {
        error = L"Не удалось получить список приложений, использующих CadThumb.";
        return false;
    }
    if (!silent) {
        const TASKDIALOG_BUTTON buttons[] = {{1001, L"Закрыть приложения и обновить"}, {IDCANCEL, L"Отмена"}};
        TASKDIALOGCONFIG cfg{sizeof(cfg)};
        cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
        cfg.pszWindowTitle = kTitle;
        cfg.pszMainIcon = TD_INFORMATION_ICON;
        cfg.pszMainInstruction = L"CadThumb используется";
        cfg.pszContent = L"Для обновления нужно закрыть CadThumb и приложения, использующие его эскизы. "
                         L"Если среди них Проводник, его окна временно закроются и затем будут восстановлены. "
                         L"Сохраните незавершённую работу перед продолжением.";
        cfg.pButtons = buttons;
        cfg.cButtons = ARRAYSIZE(buttons);
        cfg.nDefaultButton = 1001;
        int selected = 0;
        if (FAILED(TaskDialogIndirect(&cfg, &selected, nullptr, nullptr)) || selected != 1001) {
            error = L"Обновление отменено. Установленная версия сохранена.";
            return false;
        }
    }
    // Do not forcibly terminate a process that refuses to close (e.g. unsaved work).
    session.stopped = true;
    if (RmShutdown(session.id, 0, nullptr) != ERROR_SUCCESS) {
        error = L"Приложение не удалось закрыть. Закройте его и повторите обновление.";
        return false;
    }
    return true;
}

bool IsElevated() {
    BOOL elevated = FALSE;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION e{};
        DWORD size = sizeof(e);
        if (GetTokenInformation(token, TokenElevation, &e, size, &size)) elevated = e.TokenIsElevated;
        CloseHandle(token);
    }
    return elevated != FALSE;
}

bool StartsWithI(const std::wstring& s, const std::wstring& prefix) {
    return !prefix.empty() && s.size() >= prefix.size() && _wcsnicmp(s.c_str(), prefix.c_str(), prefix.size()) == 0;
}

bool ExtractResource(WORD id, const std::wstring& path) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL mem = res ? LoadResource(nullptr, res) : nullptr;
    const void* data = mem ? LockResource(mem) : nullptr;
    DWORD size = res ? SizeofResource(nullptr, res) : 0;
    if (!data || !size) return false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(f, data, size, &written, nullptr) && written == size;
    CloseHandle(f);
    return ok != FALSE;
}

// Runs the program with stdout/stderr redirected to a pipe; CadThumb.exe writes UTF-8 there.
int RunCaptured(const std::wstring& exe, const std::wstring& args, std::wstring& output) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    BOOL created = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                  DirOf(exe).c_str(), &si, &pi);
    CloseHandle(wr);
    if (!created) {
        CloseHandle(rd);
        return -1;
    }
    std::string buf;
    char chunk[4096];
    DWORD n = 0;
    while (ReadFile(rd, chunk, sizeof(chunk), &n, nullptr) && n > 0) buf.append(chunk, n);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    output = Wide(buf);
    return (int)code;
}

bool SetRegStr(HKEY k, const wchar_t* name, const std::wstring& v) {
    return RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                          DWORD((v.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool SetRegDword(HKEY k, const wchar_t* name, DWORD v) {
    return RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v)) == ERROR_SUCCESS;
}

// Relaunches this exe elevated and returns the actual install/uninstall exit code.
int RelaunchElevated(const std::wstring& args) {
    std::wstring self = ModulePath(nullptr);
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = self.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) return 5;
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return int(code);
}

std::wstring FlagsFor(const Options& o) {
    std::wstring a = L"/CONFIRMED";
    if (o.silent) a += L" /S";
    if (o.uninstall) a += L" /uninstall";
    if (o.purge) a += L" /PURGE";
    if (o.machine) a += L" /MACHINE";
    a += o.stl ? L" /STL" : L" /NOSTL";
    if (o.noMenu) a += L" /NOMENU";
    if (o.noAutostart) a += L" /NOAUTOSTART";
    if (o.restartExplorer) a += L" /RESTARTEXPLORER";
    return a;
}

std::vector<std::filesystem::path> RemoveOldVersions(const std::wstring& root, const std::wstring& keep) {
    std::vector<std::filesystem::path> pending;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
        if (!e.is_directory(ec)) continue;
        std::wstring name = e.path().filename().wstring();
        if (name.rfind(L"app-", 0) != 0 || _wcsicmp(e.path().wstring().c_str(), keep.c_str()) == 0) continue;
        // Never follow an installer-directory junction into unrelated data.
        DWORD attrs = GetFileAttributesW(e.path().c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        std::filesystem::remove_all(e.path(), ec);
        if (ec) pending.push_back(e.path());
    }
    return pending;
}

bool ScheduleDirectoryRemoval(const std::filesystem::path& dir) {
    std::error_code ec;
    bool ok = true;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        DWORD attrs = GetFileAttributesW(entry.path().c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) continue;
        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT))
            ok &= ScheduleDirectoryRemoval(entry.path());
        else
            ok &= MoveFileExW(entry.path().c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT) != FALSE;
    }
    if (ec) return false;
    return (MoveFileExW(dir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT) != FALSE) && ok;
}

bool CleanPreviousVersions(bool machine, const std::wstring& active, std::wstring& report) {
    const auto root = std::filesystem::path(InstallRoot(machine)).lexically_normal();
    const auto current = std::filesystem::path(active).lexically_normal();
    if (_wcsicmp(current.parent_path().c_str(), root.c_str()) != 0 ||
        current.filename().wstring().rfind(L"app-", 0) != 0) return false;
    auto pending = RemoveOldVersions(root.wstring(), current.wstring());
    constexpr wchar_t runOnce[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
    if (pending.empty()) {
        if (!machine) {
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, runOnce, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
                RegDeleteValueW(key, L"CadThumb.Cleanup");
                RegCloseKey(key);
            }
        }
        return true;
    }
    bool scheduled = true;
    if (machine) {
        for (const auto& dir : pending) scheduled &= ScheduleDirectoryRemoval(dir);
    } else {
        const std::wstring cmd = L"\"" + active + L"\\CadThumbSetup.exe\" /CLEANUPOLD /S";
        scheduled = RegSetKeyValueW(HKEY_CURRENT_USER, runOnce, L"CadThumb.Cleanup", REG_SZ,
                                    cmd.c_str(), DWORD((cmd.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    }
    report += scheduled
        ? (machine ? L"\r\nЗанятые файлы старой версии будут автоматически удалены при перезагрузке Windows."
                   : L"\r\nЗанятые файлы старой версии будут автоматически удалены при следующем входе в Windows.")
        : L"\r\nНе удалось запланировать удаление занятых файлов старой версии.";
    return scheduled;
}

bool WriteInstalledEntry(const Options& o, const std::wstring& root, const std::wstring& exe,
                         const std::wstring& dll, const std::wstring& setupCopy,
                         const std::wstring& version = L"" CADTHUMB_VERSION) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(o.machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, kUninstallKey,
                        0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    bool ok = true;
    ok &= SetRegStr(k, L"DisplayName", L"CadThumb — эскизы STEP/3MF/STL");
    ok &= SetRegStr(k, L"DisplayVersion", version);
    ok &= SetRegStr(k, L"Publisher", L"GNUGiminot");
    ok &= SetRegStr(k, L"URLInfoAbout", L"https://github.com/GNUGiminot/CadThumb");
    ok &= SetRegStr(k, L"HelpLink", L"https://github.com/GNUGiminot/CadThumb/issues");
    ok &= SetRegStr(k, L"DisplayIcon", exe + L",0");
    ok &= SetRegStr(k, L"InstallLocation", root);
    ok &= SetRegStr(k, L"UninstallString", L"\"" + setupCopy + L"\" /uninstall");
    ok &= SetRegStr(k, L"QuietUninstallString", L"\"" + setupCopy + L"\" /uninstall /S");
    ok &= SetRegDword(k, L"NoModify", 1);
    ok &= SetRegDword(k, L"NoRepair", 1);
    ok &= SetRegDword(k, L"StlEnabled", o.stl ? 1 : 0);
    ok &= SetRegDword(k, L"ContextMenuEnabled", o.noMenu ? 0 : 1);
    ok &= SetRegDword(k, L"AutostartEnabled", o.noAutostart ? 0 : 1);
    WIN32_FILE_ATTRIBUTE_DATA a{}, b{};
    if (GetFileAttributesExW(exe.c_str(), GetFileExInfoStandard, &a) &&
        GetFileAttributesExW(dll.c_str(), GetFileExInfoStandard, &b)) {
        ULONGLONG bytes = (ULONGLONG(a.nFileSizeHigh) << 32) + a.nFileSizeLow +
                          (ULONGLONG(b.nFileSizeHigh) << 32) + b.nFileSizeLow;
        ok &= SetRegDword(k, L"EstimatedSize", DWORD(bytes / 1024));
    }
    RegCloseKey(k);
    return ok;
}

std::wstring RegistrationArgs(const Options& o) {
    std::wstring args = L"--register";
    if (o.stl) args += L" --stl";
    if (o.machine) args += L" --machine";
    if (o.noMenu) args += L" --no-menu";
    if (o.noAutostart) args += L" --no-autostart";
    return args;
}

bool DoInstall(const Options& o, const InstalledState& previous, std::wstring& report,
               const InstalledState* duplicate = nullptr) {
    const std::wstring root = InstallRoot(o.machine);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamp[40];
    swprintf_s(stamp, L"\\app-%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
               st.wSecond);
    // A new folder per install: the previous DLL may still be loaded by Explorer and cannot be overwritten.
    const std::wstring dir = root + stamp + L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64());
    if (!EnsureDir(dir)) {
        report = L"Не удалось создать папку " + dir;
        return false;
    }
    const std::wstring exe = dir + L"\\CadThumb.exe";
    const std::wstring dll = dir + L"\\CadThumbShell.dll";
    const std::wstring setupCopy = dir + L"\\CadThumbSetup.exe";
    if (!ExtractResource(kResExe, exe) || !ExtractResource(kResDll, dll)) {
        report = L"Не удалось распаковать файлы программы в " + dir;
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        return false;
    }
    if (!CopyFileW(ModulePath(nullptr).c_str(), setupCopy.c_str(), FALSE)) {
        report = L"Не удалось скопировать программу удаления в " + setupCopy;
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        return false;
    }

    if (duplicate && !UnregisterShellExtension(duplicate->machine, report)) {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        return false;
    }
    int rc = RunCaptured(exe, RegistrationArgs(o) + L" --no-start-host", report);
    bool registered = rc == 0;
    if (!registered && report.empty())
        report = L"CadThumb.exe --register завершился с кодом " + std::to_wstring(rc);
    if (registered && !WriteInstalledEntry(o, root, exe, dll, setupCopy)) {
        report += L"\r\nНе удалось записать запись в «Установленных приложениях».";
        registered = false;
    }
    if (!registered) {
        std::wstring rollback;
        if (previous.present && FileExists(previous.dir + L"\\CadThumb.exe")) {
            Options old;
            old.machine = previous.machine;
            old.stl = previous.stl;
            old.noMenu = previous.noMenu;
            old.noAutostart = previous.noAutostart;
            if (RunCaptured(previous.dir + L"\\CadThumb.exe", RegistrationArgs(old), rollback) == 0) {
                if (previous.hasEntry)
                    WriteInstalledEntry(old, root, previous.dir + L"\\CadThumb.exe",
                                        previous.dir + L"\\CadThumbShell.dll",
                                        previous.dir + L"\\CadThumbSetup.exe", previous.version);
                else
                    RegDeleteTreeW(o.machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, kUninstallKey);
                report += L"\r\nПредыдущая версия восстановлена.";
            } else {
                report += L"\r\nНе удалось восстановить регистрацию предыдущей версии: " + rollback;
            }
        } else {
            RunCaptured(exe, o.machine ? L"--unregister --machine" : L"--unregister", rollback);
            RegDeleteTreeW(o.machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, kUninstallKey);
        }
        if (duplicate && FileExists(duplicate->dir + L"\\CadThumb.exe")) {
            Options oldDuplicate;
            oldDuplicate.machine = duplicate->machine;
            oldDuplicate.stl = duplicate->stl;
            oldDuplicate.noMenu = duplicate->noMenu;
            oldDuplicate.noAutostart = duplicate->noAutostart;
            RunCaptured(duplicate->dir + L"\\CadThumb.exe", RegistrationArgs(oldDuplicate), rollback);
        }
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        return false;
    }
    if (duplicate) {
        LSTATUS removed = RegDeleteTreeW(duplicate->machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, kUninstallKey);
        if (removed != ERROR_SUCCESS && removed != ERROR_FILE_NOT_FOUND) {
            report += L"\r\nНе удалось удалить запись старой копии CadThumb.";
            return false;
        }
        auto pending = RemoveOldVersions(InstallRoot(duplicate->machine), L"");
        for (const auto& path : pending) {
            if (!ScheduleDirectoryRemoval(path)) {
                report += L"\r\nНе удалось удалить занятые файлы старой копии.";
                return false;
            }
        }
        RemoveDirectoryW(InstallRoot(duplicate->machine).c_str());
    }
    if (!CleanPreviousVersions(o.machine, dir, report)) {
        report += L"\r\nНовая версия установлена, но очистка старой версии не завершена.";
        return false;
    }
    report = (previous.present ? L"Обновлено до версии " : L"Установлена версия ") +
             std::wstring(L"" CADTHUMB_VERSION) + L".\r\n" + report;
    return true;
}

bool DoUninstall(const Options& o, const InstalledState& installed, std::wstring& report) {
    const std::wstring self = ModulePath(nullptr);
    const bool machine = o.machine || StartsWithI(self, InstallRoot(true) + L"\\");
    std::wstring root = InstallRoot(machine);

    // Use the registered active version, even when removal was launched from a newer external setup.
    std::wstring exe = installed.dir.empty() ? DirOf(self) + L"\\CadThumb.exe"
                                            : installed.dir + L"\\CadThumb.exe";
    if (!FileExists(exe)) {
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(root, ec))
            if (FileExists(e.path().wstring() + L"\\CadThumb.exe")) exe = e.path().wstring() + L"\\CadThumb.exe";
    }
    if (FileExists(exe)) {
        if (RunCaptured(exe, machine ? L"--unregister --machine" : L"--unregister", report) != 0) return false;
    } else {
        report = L"CadThumb.exe не найден — регистрация не снята\r\n";
        return false;
    }
    LSTATUS registry = RegDeleteTreeW(machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, kUninstallKey);
    if (registry != ERROR_SUCCESS && registry != ERROR_FILE_NOT_FOUND) {
        report += L"Не удалось удалить запись из «Установленных приложений».\r\n";
        return false;
    }
    if (!machine) {
        HKEY cleanupKey = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
                         0, KEY_SET_VALUE, &cleanupKey) == ERROR_SUCCESS) {
            RegDeleteValueW(cleanupKey, L"CadThumb.Cleanup");
            RegCloseKey(cleanupKey);
        }
    }

    if (o.purge) {
        std::error_code ec;
        std::filesystem::remove_all(AppDataDir(), ec);
        report += L"Настройки и кэш удалены\r\n";
    }

    // Everything except the running uninstaller is removed now; the folder with it — right after exit.
    std::error_code ec;
    const std::wstring selfDir = DirOf(self);
    const bool runningFromInstall = StartsWithI(self, root + L"\\");
    bool pendingFiles = false;
    for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
        if (!e.is_directory(ec) || e.path().filename().wstring().rfind(L"app-", 0) != 0) continue;
        if (runningFromInstall && _wcsicmp(e.path().wstring().c_str(), selfDir.c_str()) == 0) continue;
        ec.clear();
        std::filesystem::remove_all(e.path(), ec);
        if (ec) pendingFiles = true;
    }
    if (runningFromInstall) {
        std::wstring cmd = L"cmd.exe /c ping -n 3 127.0.0.1 >nul & rmdir /s /q \"" + selfDir + L"\" & rmdir \"" +
                           root + L"\"";
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr,
                           sys, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        } else {
            pendingFiles = true;
        }
    } else {
        if (!RemoveDirectoryW(root.c_str())) {
            DWORD code = GetLastError();
            if (code != ERROR_DIR_NOT_EMPTY && code != ERROR_PATH_NOT_FOUND) pendingFiles = true;
        }
    }
    if (pendingFiles)
        report += L"Часть файлов ещё занята Проводником. Они будут удалены при следующем обновлении или вручную после перезапуска Проводника.\r\n";
    return true;
}

// ---- UI (TaskDialog: native look, no dialog templates)

bool AskInstall(Options& o, const InstalledState& previous) {
    const TASKDIALOG_BUTTON newButtons[] = {
        {1001, L"Установить для меня\nЭскизы и просмотрщик доступны только в моей учётной записи"},
        {1002, L"Установить для всех\nЭскизы доступны всем пользователям этого компьютера; потребуются права администратора"},
    };
    const TASKDIALOG_BUTTON updateButton[] = {
        {1001, L"Обновить CadThumb\nНастройки и выбранный способ установки сохранятся"},
    };
    const TASKDIALOG_BUTTON repairButton[] = {
        {1001, L"Переустановить CadThumb\nИсправить файлы программы, сохранив настройки"},
    };
    bool sameVersion = previous.present && CompareVersions(previous.version, L"" CADTHUMB_VERSION) == 0 &&
                       previous.version == L"" CADTHUMB_VERSION;
    std::wstring content = previous.present
        ? L"Сейчас установлена версия " + (previous.version.empty() ? std::wstring(L"неизвестна") : previous.version) +
          L". Новая версия " CADTHUMB_VERSION L" заменит её в той же папке установки.\n\n"
          L"Старые копии программы будут удалены автоматически. Настройки и выбор приложений для открытия моделей сохранятся."
        : L"Показывает эскизы STEP, STP, P21, 3MF и STL прямо в Проводнике. "
          L"Включает быстрый просмотрщик 3D-моделей.\n\n"
          L"Программы, которыми вы открываете модели, не изменятся.";
    std::wstring footer = previous.present
        ? L"Установлено: " + previous.dir
        : L"CadThumb не меняет приложение по умолчанию для файлов моделей.";
    TASKDIALOGCONFIG cfg{sizeof(cfg)};
    cfg.hInstance = GetModuleHandleW(nullptr);
    cfg.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    if (o.stl) cfg.dwFlags |= TDF_VERIFICATION_FLAG_CHECKED;
    cfg.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    cfg.pszWindowTitle = previous.present ? L"Обновление CadThumb" : L"Установка CadThumb";
    cfg.pszMainIcon = MAKEINTRESOURCEW(1);
    cfg.pszMainInstruction = previous.present
        ? (sameVersion ? L"Переустановка CadThumb" : L"Доступно обновление CadThumb")
        : L"Эскизы моделей в Проводнике";
    cfg.pszContent = content.c_str();
    cfg.pszFooter = footer.c_str();
    cfg.pszFooterIcon = TD_INFORMATION_ICON;
    cfg.pszVerificationText = L"Показывать эскизы STL";
    cfg.pButtons = previous.present ? (sameVersion ? repairButton : updateButton) : newButtons;
    cfg.cButtons = previous.present ? ARRAYSIZE(updateButton) : ARRAYSIZE(newButtons);
    cfg.nDefaultButton = 1001;
    int pressed = 0;
    BOOL stl = FALSE;
    if (FAILED(TaskDialogIndirect(&cfg, &pressed, nullptr, &stl)) ||
        (pressed != 1001 && (previous.present || pressed != 1002))) return false;
    if (!previous.present) o.machine = pressed == 1002;
    o.stl = stl != FALSE;
    return true;
}

bool AskUninstall(Options& o) {
    TASKDIALOGCONFIG cfg{sizeof(cfg)};
    cfg.hInstance = GetModuleHandleW(nullptr);
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    cfg.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
    cfg.nDefaultButton = IDNO;
    cfg.pszWindowTitle = kTitle;
    cfg.pszMainIcon = MAKEINTRESOURCEW(1);
    cfg.pszMainInstruction = L"Удалить CadThumb?";
    cfg.pszContent = L"Эскизы STEP/3MF перестанут строиться. Сами модели не затрагиваются.";
    cfg.pszVerificationText = L"Удалить также настройки и кэш эскизов";
    int pressed = 0;
    BOOL purge = FALSE;
    if (FAILED(TaskDialogIndirect(&cfg, &pressed, nullptr, &purge)) || pressed != IDYES) return false;
    o.purge = purge != FALSE;
    return true;
}

void ShowResult(bool ok, const wchar_t* instruction, const wchar_t* content, const std::wstring& details) {
    TASKDIALOGCONFIG cfg{sizeof(cfg)};
    cfg.hInstance = GetModuleHandleW(nullptr);
    cfg.dwCommonButtons = TDCBF_OK_BUTTON;
    cfg.pszWindowTitle = kTitle;
    cfg.pszMainIcon = ok ? TD_INFORMATION_ICON : TD_ERROR_ICON;
    cfg.pszMainInstruction = instruction;
    cfg.pszContent = content;
    if (!details.empty()) {
        cfg.pszExpandedInformation = details.c_str();
        cfg.pszExpandedControlText = L"Скрыть подробности";
        cfg.pszCollapsedControlText = L"Подробности";
        if (!ok) cfg.dwFlags |= TDF_EXPANDED_BY_DEFAULT;
    }
    TaskDialogIndirect(&cfg, nullptr, nullptr, nullptr);
}

// Final page after a successful install: restart Explorer now (recommended) or later.
bool AskRestartExplorer(const std::wstring& details, bool updated) {
    const TASKDIALOG_BUTTON buttons[] = {
        {1001, L"Перезапустить Проводник сейчас\nЭскизы сразу появятся везде, в том числе на рабочем столе. "
               L"Панель задач мигнёт, открытые окна папок закроются."},
        {1002, L"Готово\nВ папках эскизы появятся сразу, на рабочем столе — после следующего входа в Windows."},
    };
    TASKDIALOGCONFIG cfg{sizeof(cfg)};
    cfg.hInstance = GetModuleHandleW(nullptr);
    cfg.dwFlags = TDF_USE_COMMAND_LINKS;
    cfg.pszWindowTitle = kTitle;
    cfg.pszMainIcon = TD_INFORMATION_ICON;
    cfg.pszMainInstruction = updated ? L"CadThumb обновлён" : L"CadThumb установлен";
    cfg.pszContent = L"Правый клик по модели → «Просмотр 3D (CadThumb)» открывает просмотрщик.\n"
                     L"Значок CadThumb в трее: настройки, подготовка эскизов для папки, журнал.";
    cfg.pButtons = buttons;
    cfg.cButtons = ARRAYSIZE(buttons);
    cfg.nDefaultButton = 1001;
    if (!details.empty()) {
        cfg.pszExpandedInformation = details.c_str();
        cfg.pszExpandedControlText = L"Скрыть подробности";
        cfg.pszCollapsedControlText = L"Подробности";
    }
    int pressed = 0;
    TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr);
    return pressed == 1001;
}

bool AskRestartAfterUninstall(const std::wstring& details) {
    const TASKDIALOG_BUTTON buttons[] = {
        {1001, L"Перезапустить Проводник\nОсвободить DLL обработчика и убрать оставшиеся файлы"},
        {1002, L"Готово\nОставить Проводник запущенным; занятые файлы можно удалить позже"},
    };
    TASKDIALOGCONFIG cfg{sizeof(cfg)};
    cfg.hInstance = GetModuleHandleW(nullptr);
    cfg.dwFlags = TDF_USE_COMMAND_LINKS | TDF_SIZE_TO_CONTENT;
    cfg.pszWindowTitle = kTitle;
    cfg.pszMainIcon = TD_INFORMATION_ICON;
    cfg.pszMainInstruction = L"CadThumb удалён";
    cfg.pszContent = L"Регистрация эскизов и запись в «Установленных приложениях» удалены. Файлы моделей не затронуты.";
    cfg.pButtons = buttons;
    cfg.cButtons = ARRAYSIZE(buttons);
    cfg.nDefaultButton = 1001;
    if (!details.empty()) {
        cfg.pszExpandedInformation = details.c_str();
        cfg.pszCollapsedControlText = L"Подробности";
        cfg.pszExpandedControlText = L"Скрыть подробности";
    }
    int pressed = 0;
    TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr);
    return pressed == 1001;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Options o;
    bool cleanupOnly = false;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        const wchar_t* a = argv[i];
        if (_wcsicmp(a, L"/S") == 0 || _wcsicmp(a, L"/silent") == 0) o.silent = true;
        else if (_wcsicmp(a, L"/uninstall") == 0 || _wcsicmp(a, L"/U") == 0) o.uninstall = true;
        else if (_wcsicmp(a, L"/PURGE") == 0) o.purge = true;
        else if (_wcsicmp(a, L"/CONFIRMED") == 0) o.confirmed = true;
        else if (_wcsicmp(a, L"/MACHINE") == 0) { o.machine = true; o.scopeExplicit = true; }
        else if (_wcsicmp(a, L"/USER") == 0) { o.machine = false; o.scopeExplicit = true; }
        else if (_wcsicmp(a, L"/STL") == 0) { o.stl = true; o.stlExplicit = true; }
        else if (_wcsicmp(a, L"/NOSTL") == 0) { o.stl = false; o.stlExplicit = true; }
        else if (_wcsicmp(a, L"/NOMENU") == 0) { o.noMenu = true; o.menuExplicit = true; }
        else if (_wcsicmp(a, L"/NOAUTOSTART") == 0) { o.noAutostart = true; o.autostartExplicit = true; }
        else if (_wcsicmp(a, L"/RESTARTEXPLORER") == 0) o.restartExplorer = true;
        else if (_wcsicmp(a, L"/CLEANUPOLD") == 0) cleanupOnly = true;
    }
    LocalFree(argv);

    if (cleanupOnly) {
        const auto active = ReadInstalled(false);
        if (!active.present) return 0;
        std::wstring report;
        return CleanPreviousVersions(false, active.dir, report) ? 0 : 1;
    }

    const InstalledState user = ReadInstalled(false);
    const InstalledState machine = ReadInstalled(true);
    const InstalledState* previous = nullptr;
    const InstalledState* duplicate = nullptr;
    if (user.present && machine.present) {
        if (o.uninstall && o.scopeExplicit) previous = o.machine ? &machine : &user;
        else {
            previous = &machine;
            if (!o.uninstall) duplicate = &user;
        }
    } else if (user.present || machine.present) {
        previous = user.present ? &user : &machine;
        if (o.scopeExplicit && o.machine != previous->machine) {
            if (!o.silent) ShowResult(false, L"CadThumb уже установлен в другой области",
                                      L"Обновление выполняется поверх существующей установки. Сначала удалите её, если нужно сменить область установки.", L"");
            return 2;
        }
    }

    if (o.uninstall) {
        if (previous) o.machine = previous->machine;
        if (!o.silent && !o.confirmed && !AskUninstall(o)) return 0;
        if (o.machine && !IsElevated()) return RelaunchElevated(FlagsFor(o));
        std::wstring report;
        bool ok = DoUninstall(o, previous ? *previous : InstalledState{}, report);
        if (!ok) {
            if (!o.silent) ShowResult(false, L"Удаление не удалось", nullptr, report);
            return 1;
        }
        bool restart = o.restartExplorer;
        if (!o.silent) restart = AskRestartAfterUninstall(report);
        if (restart) {
            RestartExplorer();
            RemoveOldVersions(InstallRoot(o.machine), DirOf(ModulePath(nullptr)));
        }
        return ok ? 0 : 1;
    }

    if (previous && CompareVersions(previous->version, L"" CADTHUMB_VERSION) > 0) {
        if (!o.silent) ShowResult(false, L"Установлена более новая версия CadThumb",
                                  L"Этот установщик не будет заменять её старой версией. Для отката сначала удалите текущую установку.", L"");
        return 2;
    }
    if (previous) KeepPreviousChoices(o, *previous);
    if (!o.silent && !o.confirmed && !AskInstall(o, previous ? *previous : InstalledState{})) return 0;
    if (o.machine && !IsElevated()) return RelaunchElevated(FlagsFor(o));

    std::wstring report;
    UpdateSession updateSession;
    if (!CloseInstalledApplications(o.silent, updateSession, report)) {
        if (!o.silent) ShowResult(false, L"Обновление не выполнено", report.c_str(), L"");
        return 1;
    }
    bool ok = DoInstall(o, previous ? *previous : InstalledState{}, report, duplicate);
    updateSession.Finish();
    if (ok && !o.noAutostart) {
        std::wstring startupReport;
        RunCaptured(ReadInstalled(o.machine).dir + L"\\CadThumb.exe", L"--start-host", startupReport);
    }
    if (!ok) {
        if (!o.silent) ShowResult(false, L"Установка не удалась", L"Подробности ниже.", report);
        return 1;
    }
    // A running Explorer (the desktop especially) keeps its old per-extension handler info until restart.
    bool restart = o.restartExplorer;
    if (!o.silent) {
        if (updateSession.stopped)
            ShowResult(true, L"CadThumb обновлён", L"Старые копии удалены. Программа готова к работе.", report);
        else
            restart = AskRestartExplorer(report, previous != nullptr);
    }
    if (restart) {
        RestartExplorer();
        const std::wstring root = InstallRoot(o.machine);
        std::wstring active = ReadInstalled(o.machine).dir;
        if (!active.empty()) CleanPreviousVersions(o.machine, active, report);
    }
    return 0;
}
