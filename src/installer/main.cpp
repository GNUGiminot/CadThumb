// CadThumbSetup.exe — single-file installer/uninstaller. CadThumb.exe and CadThumbShell.dll are embedded
// as RCDATA resources; registration itself is delegated to `CadThumb.exe --register/--unregister`.
//
//   CadThumbSetup.exe                         dialog: for me / for all users, optional STL
//   CadThumbSetup.exe /S [/STL] [/MACHINE] [/NOMENU] [/NOAUTOSTART] [/RESTARTEXPLORER]   silent install
//   CadThumbSetup.exe /uninstall [/S] [/PURGE]                           uninstall (/PURGE: settings + cache too)
#include "common/ExplorerRestart.h"
#include "common/Paths.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <filesystem>
#include <string>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace ct;

namespace {

constexpr WORD kResExe = 101;
constexpr WORD kResDll = 102;
constexpr wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\CadThumb";
constexpr wchar_t kTitle[] = L"CadThumb";

struct Options {
    bool silent = false, uninstall = false, purge = false, confirmed = false;
    bool machine = false, stl = false, noMenu = false, noAutostart = false, restartExplorer = false;
};

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

void SetRegStr(HKEY k, const wchar_t* name, const std::wstring& v) {
    RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), DWORD((v.size() + 1) * sizeof(wchar_t)));
}

void SetRegDword(HKEY k, const wchar_t* name, DWORD v) {
    RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
}

// Relaunches this exe elevated with the given arguments (the UAC prompt is shown by Windows).
bool RelaunchElevated(const std::wstring& args) {
    std::wstring self = ModulePath(nullptr);
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.lpVerb = L"runas";
    sei.lpFile = self.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != FALSE;
}

std::wstring FlagsFor(const Options& o) {
    std::wstring a = L"/CONFIRMED";
    if (o.silent) a += L" /S";
    if (o.uninstall) a += L" /uninstall";
    if (o.purge) a += L" /PURGE";
    if (o.machine) a += L" /MACHINE";
    if (o.stl) a += L" /STL";
    if (o.noMenu) a += L" /NOMENU";
    if (o.noAutostart) a += L" /NOAUTOSTART";
    if (o.restartExplorer) a += L" /RESTARTEXPLORER";
    return a;
}

void RemoveOldVersions(const std::wstring& root, const std::wstring& keep) {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
        if (!e.is_directory(ec)) continue;
        std::wstring name = e.path().filename().wstring();
        if (name.rfind(L"app-", 0) != 0 || _wcsicmp(e.path().wstring().c_str(), keep.c_str()) == 0) continue;
        std::filesystem::remove_all(e.path(), ec); // a version still loaded by Explorer stays until next time
    }
}

bool DoInstall(const Options& o, std::wstring& report) {
    const std::wstring root = InstallRoot(o.machine);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamp[40];
    swprintf_s(stamp, L"\\app-%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
               st.wSecond);
    // A new folder per install: the previous DLL may still be loaded by Explorer and cannot be overwritten.
    const std::wstring dir = root + stamp;
    if (!EnsureDir(dir)) {
        report = L"Не удалось создать папку " + dir;
        return false;
    }
    const std::wstring exe = dir + L"\\CadThumb.exe";
    const std::wstring dll = dir + L"\\CadThumbShell.dll";
    const std::wstring setupCopy = dir + L"\\CadThumbSetup.exe";
    if (!ExtractResource(kResExe, exe) || !ExtractResource(kResDll, dll)) {
        report = L"Не удалось распаковать файлы программы в " + dir;
        return false;
    }
    CopyFileW(ModulePath(nullptr).c_str(), setupCopy.c_str(), FALSE); // used later as the uninstaller

    std::wstring args = L"--register";
    if (o.stl) args += L" --stl";
    if (o.machine) args += L" --machine";
    if (o.noMenu) args += L" --no-menu";
    if (o.noAutostart) args += L" --no-autostart";
    int rc = RunCaptured(exe, args, report);
    if (rc != 0) {
        if (report.empty()) report = L"CadThumb.exe --register завершился с кодом " + std::to_wstring(rc);
        return false;
    }

    // "Installed apps" entry
    HKEY k;
    if (RegCreateKeyExW(o.machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        SetRegStr(k, L"DisplayName", L"CadThumb — эскизы STEP/3MF/STL");
        SetRegStr(k, L"DisplayVersion", L"" CADTHUMB_VERSION);
        SetRegStr(k, L"Publisher", L"CadThumb");
        SetRegStr(k, L"DisplayIcon", exe + L",0");
        SetRegStr(k, L"InstallLocation", root);
        SetRegStr(k, L"UninstallString", L"\"" + setupCopy + L"\" /uninstall");
        SetRegStr(k, L"QuietUninstallString", L"\"" + setupCopy + L"\" /uninstall /S");
        SetRegDword(k, L"NoModify", 1);
        SetRegDword(k, L"NoRepair", 1);
        WIN32_FILE_ATTRIBUTE_DATA a{}, b{};
        GetFileAttributesExW(exe.c_str(), GetFileExInfoStandard, &a);
        GetFileAttributesExW(dll.c_str(), GetFileExInfoStandard, &b);
        SetRegDword(k, L"EstimatedSize", (a.nFileSizeLow + b.nFileSizeLow) / 1024 * 2);
        RegCloseKey(k);
    }
    RemoveOldVersions(root, dir);
    return true;
}

bool DoUninstall(const Options& o, std::wstring& report) {
    const std::wstring self = ModulePath(nullptr);
    const bool machine = StartsWithI(self, InstallRoot(true) + L"\\");
    std::wstring root = InstallRoot(machine);

    // Unregister with the CadThumb.exe next to us (or any installed version).
    std::wstring exe = DirOf(self) + L"\\CadThumb.exe";
    if (!FileExists(exe)) {
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(root, ec))
            if (FileExists(e.path().wstring() + L"\\CadThumb.exe")) exe = e.path().wstring() + L"\\CadThumb.exe";
    }
    if (FileExists(exe)) {
        RunCaptured(exe, machine ? L"--unregister --machine" : L"--unregister", report);
    } else {
        report = L"CadThumb.exe не найден — регистрация не снята\r\n";
    }
    RegDeleteTreeW(machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, kUninstallKey);

    if (o.purge) {
        std::error_code ec;
        std::filesystem::remove_all(AppDataDir(), ec);
        report += L"Настройки и кэш удалены\r\n";
    }

    // Everything except the running uninstaller is removed now; the folder with it — right after exit.
    std::error_code ec;
    const std::wstring selfDir = DirOf(self);
    const bool runningFromInstall = StartsWithI(self, root + L"\\");
    for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
        if (runningFromInstall && _wcsicmp(e.path().wstring().c_str(), selfDir.c_str()) == 0) continue;
        std::filesystem::remove_all(e.path(), ec);
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
        }
    } else {
        RemoveDirectoryW(root.c_str());
    }
    return true;
}

// ---- UI (TaskDialog: native look, no dialog templates)

bool AskInstall(Options& o) {
    const TASKDIALOG_BUTTON buttons[] = {
        {1001, L"Установить для меня\nБез прав администратора, только для текущего пользователя"},
        {1002, L"Установить для всех пользователей\nПотребуются права администратора"},
    };
    TASKDIALOGCONFIG cfg{sizeof(cfg)};
    cfg.hInstance = GetModuleHandleW(nullptr);
    cfg.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    cfg.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    cfg.pszWindowTitle = L"Установка CadThumb " CADTHUMB_VERSION;
    cfg.pszMainIcon = MAKEINTRESOURCEW(1);
    cfg.pszMainInstruction = L"Эскизы STEP и 3MF в Проводнике Windows";
    cfg.pszContent = L"Проводник будет показывать миниатюры моделей .step, .stp и .3mf. "
                     L"Файлы по-прежнему открываются в ваших программах (CAD, слайсер).\n\n"
                     L"Фоновый сервис запускается при входе в Windows и живёт в трее.";
    cfg.pszVerificationText = L"Также показывать эскизы для .stl (заменит текущий обработчик STL)";
    cfg.pButtons = buttons;
    cfg.cButtons = ARRAYSIZE(buttons);
    cfg.nDefaultButton = 1001;
    int pressed = 0;
    BOOL stl = FALSE;
    if (FAILED(TaskDialogIndirect(&cfg, &pressed, nullptr, &stl)) || (pressed != 1001 && pressed != 1002)) return false;
    o.machine = pressed == 1002;
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
bool AskRestartExplorer(const std::wstring& details) {
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
    cfg.pszMainInstruction = L"CadThumb установлен";
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

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Options o;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        const wchar_t* a = argv[i];
        if (_wcsicmp(a, L"/S") == 0 || _wcsicmp(a, L"/silent") == 0) o.silent = true;
        else if (_wcsicmp(a, L"/uninstall") == 0 || _wcsicmp(a, L"/U") == 0) o.uninstall = true;
        else if (_wcsicmp(a, L"/PURGE") == 0) o.purge = true;
        else if (_wcsicmp(a, L"/CONFIRMED") == 0) o.confirmed = true;
        else if (_wcsicmp(a, L"/MACHINE") == 0) o.machine = true;
        else if (_wcsicmp(a, L"/STL") == 0) o.stl = true;
        else if (_wcsicmp(a, L"/NOMENU") == 0) o.noMenu = true;
        else if (_wcsicmp(a, L"/NOAUTOSTART") == 0) o.noAutostart = true;
        else if (_wcsicmp(a, L"/RESTARTEXPLORER") == 0) o.restartExplorer = true;
    }
    LocalFree(argv);

    if (o.uninstall) {
        if (!o.silent && !o.confirmed && !AskUninstall(o)) return 0;
        const bool machine = StartsWithI(ModulePath(nullptr), InstallRoot(true) + L"\\");
        if (machine && !IsElevated()) return RelaunchElevated(FlagsFor(o)) ? 0 : 5;
        std::wstring report;
        bool ok = DoUninstall(o, report);
        if (!o.silent) ShowResult(ok, ok ? L"CadThumb удалён" : L"Удаление не удалось", nullptr, report);
        return ok ? 0 : 1;
    }

    if (!o.silent && !o.confirmed && !AskInstall(o)) return 0;
    if (o.machine && !IsElevated()) return RelaunchElevated(FlagsFor(o)) ? 0 : 5;

    std::wstring report;
    bool ok = DoInstall(o, report);
    if (!o.silent)
    if (!ok) {
        if (!o.silent) ShowResult(false, L"Установка не удалась", L"Подробности ниже.", report);
        return 1;
    }
    // A running Explorer (the desktop especially) keeps its old per-extension handler info until restart.
    bool restart = o.restartExplorer;
    if (!o.silent) restart = AskRestartExplorer(report);
    if (restart) RestartExplorer();
    return 0;
}
