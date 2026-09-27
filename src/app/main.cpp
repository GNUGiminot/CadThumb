#include "app/App.h"

#include "common/Cache.h"
#include "common/ExplorerRestart.h"
#include "common/Log.h"
#include "common/Paths.h"
#include "common/PipeClient.h"
#include "common/Registration.h"

#include <exdisp.h>
#include <shellapi.h>
#include <shldisp.h>
#include <shlguid.h>
#include <shobjidl.h>

using namespace ct;

static const wchar_t kUsage[] =
    L"CadThumb " CADTHUMB_VERSION L" — эскизы STEP / 3MF / STL в Проводнике Windows\r\n"
    L"\r\n"
    L"  CadThumb.exe                     запустить фоновый сервис (значок в трее)\r\n"
    L"  CadThumb.exe --register [--stl] [--machine] [--no-menu] [--no-autostart]\r\n"
    L"                                   зарегистрировать обработчик эскизов\r\n"
    L"                                   --stl      также для .stl (по умолчанию нет)\r\n"
    L"                                   --machine  для всех пользователей (нужен админ)\r\n"
    L"  CadThumb.exe --unregister [--machine]\r\n"
    L"  CadThumb.exe --refresh <файл> [...]   перестроить эскиз файла(ов)\r\n"
    L"  CadThumb.exe --prewarm <папка>        построить эскизы для всех моделей в папке\r\n"
    L"  CadThumb.exe --view [файл]            3D-просмотрщик (вращение, масштаб, рёбра)\r\n"
    L"  CadThumb.exe --render <вход> <выход.png> [--size N] [--type step|3mf|stl]\r\n"
    L"  CadThumb.exe --status | --stop | --clear-cache | --restart-explorer\r\n";

static bool HasFlag(int argc, wchar_t** argv, const wchar_t* flag) {
    for (int i = 1; i < argc; ++i)
        if (_wcsicmp(argv[i], flag) == 0) return true;
    return false;
}

static const wchar_t* FlagValue(int argc, wchar_t** argv, const wchar_t* flag) {
    for (int i = 1; i + 1 < argc; ++i)
        if (_wcsicmp(argv[i], flag) == 0) return argv[i + 1];
    return nullptr;
}

// Asks Explorer to start the host (IShellDispatch2::ShellExecute on the desktop folder view). The host then
// is not our descendant — installers and `Start-Process -Wait` wait for every process in their job — and
// runs with the user's normal (non-elevated) token even when the installer itself is elevated.
static bool LaunchViaExplorer(const std::wstring& exe, const std::wstring& args) {
    bool ok = false;
    IShellWindows* windows = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows))))
        return false;
    VARIANT empty{};
    long hwnd = 0;
    IDispatch* disp = nullptr;
    if (SUCCEEDED(windows->FindWindowSW(&empty, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &disp)) && disp) {
        IServiceProvider* sp = nullptr;
        if (SUCCEEDED(disp->QueryInterface(IID_PPV_ARGS(&sp)))) {
            IShellBrowser* browser = nullptr;
            if (SUCCEEDED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser)))) {
                IShellView* view = nullptr;
                if (SUCCEEDED(browser->QueryActiveShellView(&view))) {
                    IDispatch* bg = nullptr;
                    if (SUCCEEDED(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&bg)))) {
                        IShellFolderViewDual* fv = nullptr;
                        if (SUCCEEDED(bg->QueryInterface(IID_PPV_ARGS(&fv)))) {
                            IDispatch* app = nullptr;
                            if (SUCCEEDED(fv->get_Application(&app))) {
                                IShellDispatch2* shell = nullptr;
                                if (SUCCEEDED(app->QueryInterface(IID_PPV_ARGS(&shell)))) {
                                    BSTR file = SysAllocString(exe.c_str());
                                    VARIANT vArgs{}, vDir{}, vOp{}, vShow{};
                                    vArgs.vt = VT_BSTR;
                                    vArgs.bstrVal = SysAllocString(args.c_str());
                                    vDir.vt = VT_BSTR;
                                    vDir.bstrVal = SysAllocString(DirOf(exe).c_str());
                                    vOp.vt = VT_BSTR;
                                    vOp.bstrVal = SysAllocString(L"open");
                                    vShow.vt = VT_I4;
                                    vShow.lVal = SW_HIDE;
                                    ok = SUCCEEDED(shell->ShellExecute(file, vArgs, vDir, vOp, vShow));
                                    SysFreeString(file);
                                    VariantClear(&vArgs);
                                    VariantClear(&vDir);
                                    VariantClear(&vOp);
                                    shell->Release();
                                }
                                app->Release();
                            }
                            fv->Release();
                        }
                        bg->Release();
                    }
                    view->Release();
                }
                browser->Release();
            }
            sp->Release();
        }
        disp->Release();
    }
    windows->Release();
    return ok;
}

static void LaunchHostDetached() {
    std::wstring exe = SelfExePath();
    if (LaunchViaExplorer(exe, L"--host")) return;
    // No Explorer shell (e.g. Server Core) — start it ourselves.
    std::wstring cmd = L"\"" + exe + L"\" --host";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                             DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB, nullptr, DirOf(exe).c_str(), &si, &pi);
    if (!ok)
        ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr,
                            DirOf(exe).c_str(), &si, &pi);
    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

static int CmdRegister(int argc, wchar_t** argv) {
    RegisterOptions opt;
    opt.machine = HasFlag(argc, argv, L"--machine");
    opt.includeStl = HasFlag(argc, argv, L"--stl");
    opt.contextMenu = !HasFlag(argc, argv, L"--no-menu");
    opt.autostart = !HasFlag(argc, argv, L"--no-autostart");
    opt.exePath = SelfExePath();
    opt.dllPath = DirOf(opt.exePath) + L"\\" + kDllName;
    if (!FileExists(opt.dllPath)) {
        Out(L"Не найден " + opt.dllPath + L"\r\n");
        return 1;
    }
    std::wstring report;
    bool ok = RegisterShellExtension(opt, report);
    Out(std::wstring(ok ? L"Регистрация выполнена" : L"Ошибка регистрации") +
        (opt.machine ? L" (все пользователи):\r\n" : L" (текущий пользователь):\r\n") + report);
    Settings::WriteDefaultsIfMissing();
    if (ok && opt.autostart) {
        StopRunningHost(5000); // restart to pick up a new version
        LaunchHostDetached();
    }
    Out(L"\r\nТекущие обработчики эскизов:\r\n" + DescribeThumbnailHandlers());
    return ok ? 0 : 1;
}

static int CmdStatus() {
    Out(std::wstring(L"CadThumb ") + CADTHUMB_VERSION L"\r\n");
    Out(L"Фоновый сервис: " + std::wstring(IsHostRunning() ? L"запущен" : L"не запущен") + L"\r\n");
    Out(L"Автозапуск: " + std::wstring(IsAutostartEnabled() ? L"включён" : L"выключен") + L"\r\n");
    auto cs = GetCacheStats();
    Out(L"Кэш: " + std::to_wstring(cs.files) + L" эскизов, " + std::to_wstring(cs.bytes / 1024) + L" КБ, " +
        std::to_wstring(cs.failures) + L" ошибок  (" + CacheDir() + L")\r\n");
    Out(L"Настройки: " + SettingsPath() + L"\r\n\r\nОбработчики эскизов:\r\n" + DescribeThumbnailHandlers());
    return 0;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    SetLogTag(L"cli");

    if (argc < 2 || HasFlag(argc, argv, L"--host")) return RunHost(instance);
    if (_wcsicmp(argv[1], L"--view") == 0) {
        const wchar_t* snap = FlagValue(argc, argv, L"--snapshot");
        if (snap) AttachParentConsole();
        return RunViewer(instance, argc > 2 && wcsncmp(argv[2], L"--", 2) != 0 ? argv[2] : L"", snap ? snap : L"");
    }

    const std::wstring cmd = argv[1];
    if (cmd == L"--render") {
        AttachParentConsole();
        if (argc < 4) {
            Out(kUsage);
            return 3;
        }
        const wchar_t* size = FlagValue(argc, argv, L"--size");
        const wchar_t* type = FlagValue(argc, argv, L"--type");
        return RenderCommand(argv[2], argv[3], type ? FileTypeFromName(type) : FileType::Unknown,
                             size ? _wtoi(size) : 256);
    }

    AttachParentConsole();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int rc = 0;
    bool showBox = true;
    if (cmd == L"--via-explorer" && argc >= 3) {
        // Diagnostics: run a program in Explorer's context (outside app containers / job objects).
        std::wstring args;
        for (int i = 3; i < argc; ++i) args += (i > 3 ? L" \"" : L"\"") + std::wstring(argv[i]) + L"\"";
        rc = LaunchViaExplorer(argv[2], args) ? 0 : 1;
        LocalFree(argv);
        return rc;
    }
    if (cmd == L"--register") {
        rc = CmdRegister(argc, argv);
    } else if (cmd == L"--unregister") {
        StopRunningHost(5000);
        std::wstring report;
        bool ok = UnregisterShellExtension(HasFlag(argc, argv, L"--machine"), report);
        Out(std::wstring(ok ? L"Регистрация снята:\r\n" : L"Ошибка:\r\n") + report);
        rc = ok ? 0 : 1;
    } else if (cmd == L"--refresh") {
        std::vector<std::wstring> files;
        for (int i = 2; i < argc; ++i) files.push_back(argv[i]);
        rc = RefreshFiles(files);
        showBox = false; // RefreshFiles reports errors itself
    } else if (cmd == L"--prewarm" && argc >= 3) {
        // one-shot, without the host: render every model in the folder
        const Settings s = Settings::Get();
        auto files = CollectModelFiles(argv[2], true);
        int ok = 0, fail = 0;
        for (const auto& f : files) {
            FileType type = FileTypeFromExtension(ExtOf(f));
            if (!FileTypeEnabled(type, s)) continue;
            std::string key;
            if (!CacheKeyFromFile(f, type, 256, s.renderSignature, key)) continue;
            if (QueryCache(key, s.failRetryHours) != CacheState::Missing) continue;
            std::wstring err;
            if (RenderToCache(f, type, 256, key, s, &err)) {
                ++ok;
                UpdateExplorerThumbnail(f);
            } else {
                ++fail;
                Out(FileNameOf(f) + L": " + err + L"\r\n");
            }
        }
        Out(L"Готово: построено " + std::to_wstring(ok) + L", ошибок " + std::to_wstring(fail) + L"\r\n");
    } else if (cmd == L"--status") {
        rc = CmdStatus();
    } else if (cmd == L"--stop") {
        rc = StopRunningHost(10000) ? 0 : 1;
        Out(rc ? L"Не удалось остановить сервис\r\n" : L"Сервис остановлен\r\n");
    } else if (cmd == L"--restart-explorer") {
        rc = RestartExplorer() ? 0 : 1;
        Out(rc ? L"Не удалось перезапустить Проводник\r\n" : L"Проводник перезапущен\r\n");
    } else if (cmd == L"--clear-cache") {
        ClearCache();
        Out(L"Кэш очищен\r\n");
    } else {
        Out(kUsage);
        rc = cmd == L"--help" || cmd == L"-h" || cmd == L"/?" ? 0 : 3;
    }
    if (showBox) FlushOutAsMessageBox(L"CadThumb", rc != 0);
    LocalFree(argv);
    return rc;
}
