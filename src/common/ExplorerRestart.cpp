#include "common/ExplorerRestart.h"

#include <windows.h>
#include <restartmanager.h>
#include <tlhelp32.h>
#include <string>
#include <vector>

#pragma comment(lib, "rstrtmgr.lib")

namespace ct {

bool RestartExplorer() {
    DWORD session = 0;
    WCHAR key[CCH_RM_SESSION_KEY + 1] = {};
    if (RmStartSession(&session, 0, key) != ERROR_SUCCESS) return false;

    DWORD mySession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &mySession);
    std::vector<RM_UNIQUE_PROCESS> procs;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"explorer.exe") != 0) continue;
        DWORD sid = 0;
        if (!ProcessIdToSessionId(pe.th32ProcessID, &sid) || sid != mySession) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        FILETIME created, exited, kernel, user;
        if (GetProcessTimes(h, &created, &exited, &kernel, &user)) procs.push_back({pe.th32ProcessID, created});
        CloseHandle(h);
    }
    CloseHandle(snap);

    bool ok = false;
    if (!procs.empty() &&
        RmRegisterResources(session, 0, nullptr, UINT(procs.size()), procs.data(), 0, nullptr) == ERROR_SUCCESS &&
        RmShutdown(session, RmForceShutdown, nullptr) == ERROR_SUCCESS) {
        ok = RmRestart(session, 0, nullptr) == ERROR_SUCCESS;
    }
    RmEndSession(session);

    // Safety net: if Explorer did not come back (no shell = no taskbar), start it.
    if (!procs.empty()) {
        for (int i = 0; i < 50 && !FindWindowW(L"Shell_TrayWnd", nullptr); ++i) Sleep(100);
        if (!FindWindowW(L"Shell_TrayWnd", nullptr)) {
            wchar_t win[MAX_PATH];
            GetWindowsDirectoryW(win, MAX_PATH);
            std::wstring exe = std::wstring(win) + L"\\explorer.exe";
            STARTUPINFOW si{sizeof(si)};
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(exe.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, win, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                ok = true;
            }
        }
    }
    return ok;
}

} // namespace ct
