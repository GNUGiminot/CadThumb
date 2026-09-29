// Host mode: tray icon + named-pipe render server. Started at logon (HKCU\...\Run) or on demand by the
// shell extension.
#include "app/App.h"
#include "app/JobManager.h"
#include "app/PipeServer.h"

#include "common/Cache.h"
#include "common/Log.h"
#include "common/Paths.h"
#include "common/Registration.h"

#include <shellapi.h>
#include <shobjidl.h>
#include <cstdio>
#include <thread>

namespace ct {

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT_PTR TIMER_TIP = 1, TIMER_PRUNE = 2;
enum MenuId : UINT {
    ID_STATUS = 100,
    ID_CACHEINFO,
    ID_PREWARM,
    ID_VIEWER,
    ID_SETTINGS,
    ID_LOG,
    ID_OPEN_CACHE,
    ID_CLEAR_CACHE,
    ID_HANDLERS,
    ID_AUTOSTART,
    ID_EXIT,
};

HWND g_wnd = nullptr;
HICON g_icon = nullptr;
UINT g_taskbarCreated = 0;
PrewarmState g_prewarm;
std::thread g_prewarmThread;
bool g_prewarmNotified = true;

void AddTrayIcon() {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = g_wnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_icon;
    wcscpy_s(nid.szTip, L"CadThumb — эскизы STEP / 3MF");
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void RemoveTrayIcon() {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = g_wnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void Balloon(const std::wstring& title, const std::wstring& text) {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = g_wnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

std::wstring StatusLine() {
    auto st = JobManager::Instance().GetStats();
    wchar_t buf[200];
    swprintf_s(buf, L"В очереди: %d, в работе: %d, готово: %d, ошибок: %d", st.queued, st.running, st.done, st.failed);
    return buf;
}

void UpdateTip() {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = g_wnd;
    nid.uID = 1;
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    std::wstring tip = L"CadThumb\n" + StatusLine();
    if (g_prewarm.running)
        tip += L"\nПодготовка: " + std::to_wstring(g_prewarm.done.load()) + L"/" + std::to_wstring(g_prewarm.total.load());
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);

    if (!g_prewarm.running && !g_prewarmNotified) {
        g_prewarmNotified = true;
        Balloon(L"CadThumb: эскизы подготовлены",
                L"Файлов: " + std::to_wstring(g_prewarm.total.load()) + L", построено: " +
                    std::to_wstring(g_prewarm.rendered.load()) + L", ошибок: " + std::to_wstring(g_prewarm.failed.load()));
    }
}

std::wstring PickFolder() {
    std::wstring result;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return result;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Папка с моделями STEP / 3MF — подготовить эскизы");
    if (SUCCEEDED(dlg->Show(g_wnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                result = p;
                CoTaskMemFree(p);
            }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

void StartPrewarm() {
    if (g_prewarm.running) {
        g_prewarm.cancel = true;
        return;
    }
    std::wstring folder = PickFolder();
    if (folder.empty()) return;
    if (g_prewarmThread.joinable()) g_prewarmThread.join();
    g_prewarm.running = true;
    g_prewarmNotified = false;
    g_prewarmThread = std::thread([folder] { PrewarmFolder(folder, g_prewarm); });
}

void ShowMenu() {
    HMENU m = CreatePopupMenu();
    auto cs = GetCacheStats();
    wchar_t cacheLine[128];
    swprintf_s(cacheLine, L"Кэш: %llu эскизов, %.1f МБ", cs.files, cs.bytes / 1048576.0);

    AppendMenuW(m, MF_STRING | MF_GRAYED, ID_STATUS, StatusLine().c_str());
    AppendMenuW(m, MF_STRING | MF_GRAYED, ID_CACHEINFO, cacheLine);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    if (g_prewarm.running) {
        std::wstring s = L"Остановить подготовку (" + std::to_wstring(g_prewarm.done.load()) + L"/" +
                         std::to_wstring(g_prewarm.total.load()) + L")";
        AppendMenuW(m, MF_STRING, ID_PREWARM, s.c_str());
    } else {
        AppendMenuW(m, MF_STRING, ID_PREWARM, L"Подготовить эскизы для папки…");
    }
    AppendMenuW(m, MF_STRING, ID_VIEWER, L"Открыть 3D-модель…");
    AppendMenuW(m, MF_STRING, ID_SETTINGS, L"Настройки…");
    AppendMenuW(m, MF_STRING, ID_LOG, L"Открыть журнал");
    AppendMenuW(m, MF_STRING, ID_OPEN_CACHE, L"Открыть папку кэша");
    AppendMenuW(m, MF_STRING, ID_CLEAR_CACHE, L"Очистить кэш эскизов");
    AppendMenuW(m, MF_STRING, ID_HANDLERS, L"Диагностика (эскизы не появляются?)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : 0), ID_AUTOSTART, L"Запускать при входе в Windows");
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Выход");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_wnd, nullptr);
    DestroyMenu(m);
    PostMessageW(g_wnd, WM_NULL, 0, 0);

    switch (cmd) {
    case ID_PREWARM: StartPrewarm(); break;
    case ID_VIEWER: LaunchViewer(L""); break; // the viewer shows its own open dialog (Ctrl+O / drag & drop)
    case ID_SETTINGS:
        Settings::WriteDefaultsIfMissing();
        ShellExecuteW(nullptr, nullptr, L"notepad.exe", (L"\"" + SettingsPath() + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        break;
    case ID_LOG: ShellExecuteW(nullptr, nullptr, L"notepad.exe", (L"\"" + LogPath() + L"\"").c_str(), nullptr, SW_SHOWNORMAL); break;
    case ID_OPEN_CACHE: ShellExecuteW(nullptr, L"open", CacheDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case ID_CLEAR_CACHE:
        if (MessageBoxW(g_wnd,
                        L"Удалить все эскизы из кэша CadThumb?\n\nПроводник хранит свою копию эскизов отдельно. "
                        L"Чтобы перестроить конкретный файл, используйте пункт контекстного меню «Обновить эскиз (CadThumb)», "
                        L"для всех — «Очистка диска» → «Эскизы».",
                        L"CadThumb", MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
            ClearCache();
            Log(L"cache cleared by user");
        }
        break;
    case ID_HANDLERS:
        // separate process: the check loads the thumbnail DLL like Explorer does, keep that out of the host
        ShellExecuteW(nullptr, nullptr, SelfExePath().c_str(), L"--diagnose", nullptr, SW_SHOWNORMAL);
        break;
    case ID_AUTOSTART: SetAutostart(!IsAutostartEnabled(), SelfExePath()); break;
    case ID_EXIT: DestroyWindow(g_wnd); break;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && g_taskbarCreated) {
        AddTrayIcon(); // Explorer restarted
        return 0;
    }
    switch (msg) {
    case WM_TRAY:
        switch (LOWORD(lp)) {
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP:
        case NIN_SELECT:
        case NIN_KEYSELECT: ShowMenu(); break;
        }
        return 0;
    case WM_TIMER:
        if (wp == TIMER_TIP) UpdateTip();
        if (wp == TIMER_PRUNE) {
            Settings s = Settings::Get();
            PruneCache(s.cacheMaxMB, s.failRetryHours);
            CleanupTempDir(24);
        }
        return 0;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_ENDSESSION:
        if (wp) RemoveTrayIcon();
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

bool StopRunningHost(DWORD waitMs) {
    HWND w = FindWindowW(kHostWindowClass, nullptr);
    if (!w) return true;
    PostMessageW(w, WM_CLOSE, 0, 0);
    ULONGLONG end = GetTickCount64() + waitMs;
    while (GetTickCount64() < end) {
        HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, HostMutexName().c_str());
        if (!m) return true;
        CloseHandle(m);
        Sleep(100);
    }
    return false;
}

int RunHost(HINSTANCE instance) {
    SetLogTag(L"host");
    HANDLE mutex = CreateMutexW(nullptr, TRUE, HostMutexName().c_str());
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) return 0; // already running

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // Inherited by render processes: no WER / critical-error dialogs.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

    Settings::WriteDefaultsIfMissing();
    {
        Settings s = Settings::Get();
        CleanupTempDir(24); // A separate --refresh/--prewarm process may still be rendering.
        PruneCache(s.cacheMaxMB, s.failRetryHours);
    }

    JobManager::Instance().Start();
    PipeServer server;
    if (!server.Start()) Log(L"pipe server failed to start");
    Log(L"host started (v%s), pipe %s", CADTHUMB_VERSION L"", PipeName().c_str());

    g_icon = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                               GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!g_icon) g_icon = LoadIconW(nullptr, IDI_APPLICATION);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kHostWindowClass;
    wc.hIcon = g_icon;
    RegisterClassExW(&wc);
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, kHostWindowClass, L"CadThumb", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                            instance, nullptr);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    AddTrayIcon();
    SetTimer(g_wnd, TIMER_TIP, 2000, nullptr);
    SetTimer(g_wnd, TIMER_PRUNE, 6 * 3600 * 1000, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Log(L"host exiting");
    RemoveTrayIcon();
    g_prewarm.cancel = true;
    // Render processes live in job objects with KILL_ON_JOB_CLOSE: they die with us.
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    ExitProcess(0);
}

} // namespace ct
