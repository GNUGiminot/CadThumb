// `CadThumb.exe --diagnose` / tray "Диагностика": checks the whole chain the way Explorer uses it and
// writes a report to the desktop (CadThumb-diagnostics.txt). Meant for "thumbnails don't show up on
// this PC" reports: registration, the handler Windows actually resolves, the host, a real render and
// a real shell thumbnail request, and whether Explorer predates the installation.
#include "app/App.h"

#include "common/Cache.h"
#include "common/ExplorerRestart.h"
#include "common/ImageIO.h"
#include "common/Paths.h"
#include "common/PipeClient.h"
#include "common/Registration.h"
#include "common/Settings.h"
#include "render/RenderFile.h"

#include <miniz.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <cstdio>
#include <string>
#include <vector>

namespace ct {

namespace {

constexpr wchar_t kThumbIid[] = L"{e357fccd-a995-4576-b01f-234630154e96}";

std::wstring Hex(HRESULT hr) {
    wchar_t b[16];
    swprintf_s(b, L"0x%08lX", (unsigned long)hr);
    return b;
}

std::wstring FormatTime(const FILETIME& ft) {
    FILETIME local;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&ft, &local);
    FileTimeToSystemTime(&local, &st);
    wchar_t b[64];
    swprintf_s(b, L"%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}

std::wstring RegStr(HKEY root, const std::wstring& key, const wchar_t* value) {
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key.c_str(), value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, buf,
                     &size) != ERROR_SUCCESS)
        return {};
    return buf;
}

// Minimal 3MF (a 20 mm cube, no embedded picture -> goes through the real render path). A timestamp
// comment makes the content unique so the check never hits an old cache entry.
bool WriteTest3mf(const std::wstring& path) {
    wchar_t stamp[32];
    swprintf_s(stamp, L"%llu", (unsigned long long)GetTickCount64());
    const std::string model =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!-- cadthumb diagnostics " + Utf8(stamp) +
        " -->\n<model unit=\"millimeter\" xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\">"
        "<resources><object id=\"1\" type=\"model\"><mesh><vertices>"
        "<vertex x=\"0\" y=\"0\" z=\"0\"/><vertex x=\"20\" y=\"0\" z=\"0\"/><vertex x=\"20\" y=\"20\" z=\"0\"/>"
        "<vertex x=\"0\" y=\"20\" z=\"0\"/><vertex x=\"0\" y=\"0\" z=\"20\"/><vertex x=\"20\" y=\"0\" z=\"20\"/>"
        "<vertex x=\"20\" y=\"20\" z=\"20\"/><vertex x=\"0\" y=\"20\" z=\"20\"/></vertices><triangles>"
        "<triangle v1=\"0\" v2=\"2\" v3=\"1\"/><triangle v1=\"0\" v2=\"3\" v3=\"2\"/>"
        "<triangle v1=\"4\" v2=\"5\" v3=\"6\"/><triangle v1=\"4\" v2=\"6\" v3=\"7\"/>"
        "<triangle v1=\"0\" v2=\"1\" v3=\"5\"/><triangle v1=\"0\" v2=\"5\" v3=\"4\"/>"
        "<triangle v1=\"1\" v2=\"2\" v3=\"6\"/><triangle v1=\"1\" v2=\"6\" v3=\"5\"/>"
        "<triangle v1=\"2\" v2=\"3\" v3=\"7\"/><triangle v1=\"2\" v2=\"7\" v3=\"6\"/>"
        "<triangle v1=\"3\" v2=\"0\" v3=\"4\"/><triangle v1=\"3\" v2=\"4\" v3=\"7\"/>"
        "</triangles></mesh></object></resources><build><item objectid=\"1\"/></build></model>\n";
    const std::string types =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/"
        "content-types\"><Default Extension=\"rels\" ContentType=\"application/"
        "vnd.openxmlformats-package.relationships+xml\"/><Default Extension=\"model\" ContentType=\"application/"
        "vnd.ms-package.3dmanufacturing-3dmodel+xml\"/></Types>\n";
    const std::string rels =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/"
        "package/2006/relationships\"><Relationship Target=\"/3D/3dmodel.model\" Id=\"rel0\" "
        "Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/></Relationships>\n";

    mz_zip_archive zip{};
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) return false;
    bool ok = mz_zip_writer_add_mem(&zip, "[Content_Types].xml", types.data(), types.size(), MZ_DEFAULT_COMPRESSION) &&
              mz_zip_writer_add_mem(&zip, "_rels/.rels", rels.data(), rels.size(), MZ_DEFAULT_COMPRESSION) &&
              mz_zip_writer_add_mem(&zip, "3D/3dmodel.model", model.data(), model.size(), MZ_DEFAULT_COMPRESSION);
    void* buf = nullptr;
    size_t size = 0;
    ok = ok && mz_zip_writer_finalize_heap_archive(&zip, &buf, &size);
    if (ok) {
        FILE* f = _wfopen(path.c_str(), L"wb");
        ok = f && fwrite(buf, 1, size, f) == size;
        if (f) fclose(f);
    }
    mz_zip_writer_end(&zip); // frees buf
    return ok;
}

} // namespace

int Diagnose(bool interactive) {
    std::wstring r;
    auto line = [&](const std::wstring& s) { r += s + L"\r\n"; };
    int problems = 0;
    auto bad = [&](const std::wstring& s) {
        ++problems;
        line(L"  [!] " + s);
    };
    auto good = [&](const std::wstring& s) { line(L"  [ok] " + s); };

    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t head[128];
    swprintf_s(head, L"CadThumb %s diagnostics, %04u-%02u-%02u %02u:%02u", CADTHUMB_VERSION L"", now.wYear, now.wMonth,
               now.wDay, now.wHour, now.wMinute);
    line(head);
    OSVERSIONINFOEXW os{sizeof(os)};
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
    if (auto fn = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) fn(&os);
    line(L"Windows " + std::to_wstring(os.dwMajorVersion) + L"." + std::to_wstring(os.dwMinorVersion) + L" build " +
         std::to_wstring(os.dwBuildNumber) + L", exe: " + SelfExePath());
    line(L"");

    // 1. registration
    line(L"1. Registration");
    const std::wstring clsidKey = std::wstring(L"CLSID\\") + kClsidString;
    const std::wstring dll = RegStr(HKEY_CLASSES_ROOT, clsidKey + L"\\InprocServer32", nullptr);
    if (dll.empty()) bad(L"thumbnail handler CLSID is not registered -- run the installer again");
    else if (!FileExists(dll)) bad(L"registered DLL does not exist: " + dll);
    else good(L"handler DLL: " + dll);
    DWORD isolation = 0, sz = sizeof(isolation);
    RegGetValueW(HKEY_CLASSES_ROOT, clsidKey.c_str(), L"DisableProcessIsolation", RRF_RT_REG_DWORD, nullptr, &isolation, &sz);
    line(L"  DisableProcessIsolation = " + std::to_wstring(isolation));
    for (const wchar_t* ext : {L".step", L".stp", L".3mf", L".stl"}) {
        wchar_t handler[128] = {};
        DWORD len = ARRAYSIZE(handler);
        HRESULT hr = AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_SHELLEXTENSION, ext, kThumbIid, handler, &len);
        std::wstring h = SUCCEEDED(hr) ? handler : L"(none)";
        bool ours = _wcsicmp(h.c_str(), kClsidString) == 0;
        std::wstring msg = std::wstring(ext) + L": Windows resolves thumbnail handler " + h;
        if (ours) good(msg);
        else if (wcscmp(ext, L".stl") == 0) line(L"  [--] " + msg + L" (STL is optional)");
        else bad(msg + L" -- not CadThumb");
    }
    line(L"");

    // 2. Explorer vs install time
    line(L"2. Explorer");
    FILETIME installed{};
    const std::wstring installDir = RegStr(HKEY_CURRENT_USER, L"Software\\CadThumb", L"InstallDir").empty()
                                        ? RegStr(HKEY_LOCAL_MACHINE, L"Software\\CadThumb", L"InstallDir")
                                        : RegStr(HKEY_CURRENT_USER, L"Software\\CadThumb", L"InstallDir");
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!installDir.empty() && GetFileAttributesExW(installDir.c_str(), GetFileExInfoStandard, &fad)) {
        installed = fad.ftCreationTime;
        line(L"  installed: " + FormatTime(installed) + L"  (" + installDir + L")");
    }
    {
        DWORD v = 0, size = sizeof(v);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                         L"IconsOnly", RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS && v)
            bad(L"\"Always show icons, never thumbnails\" is ON (Explorer > View > Options > View tab) -- "
                L"no thumbnails for any file type");
        for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
            v = 0;
            size = sizeof(v);
            if (RegGetValueW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer",
                             L"DisableThumbnails", RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS && v)
                bad(std::wstring(L"thumbnails are disabled by policy (") +
                    (root == HKEY_CURRENT_USER ? L"HKCU" : L"HKLM") + L"\\...\\Policies\\Explorer\\DisableThumbnails)");
        }
    }
    bool staleExplorer = false;
    DWORD mySession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &mySession);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"explorer.exe") != 0) continue;
        DWORD sid = 0;
        if (!ProcessIdToSessionId(pe.th32ProcessID, &sid) || sid != mySession) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        FILETIME c{}, e{}, k{}, u{};
        if (h && GetProcessTimes(h, &c, &e, &k, &u)) {
            bool older = installed.dwHighDateTime && CompareFileTime(&c, &installed) < 0;
            std::wstring msg = L"explorer.exe pid " + std::to_wstring(pe.th32ProcessID) + L" started " + FormatTime(c);
            if (older) {
                staleExplorer = true;
                bad(msg + L" -- BEFORE installation: it does not know the new handler until restarted");
            } else {
                good(msg);
            }
        }
        if (h) CloseHandle(h);
    }
    CloseHandle(snap);
    line(L"");

    // 3. host
    line(L"3. Background service");
    if (IsHostRunning()) good(L"running");
    else if (StartHost(SelfExePath(), 5000)) good(L"was not running, started now");
    else bad(L"cannot start CadThumb.exe --host");
    const Settings s = Settings::Get();
    line(L"  settings: " + SettingsPath() + L" (STEP " + std::to_wstring(s.enableStep) + L", 3MF " +
         std::to_wstring(s.enable3mf) + L", STL " + std::to_wstring(s.enableStl) + L")");
    line(L"");

    // 4. rendering in this process
    line(L"4. Rendering");
    const std::wstring dir = TempDir() + L"\\diagnostics";
    EnsureDir(dir);
    const std::wstring model = dir + L"\\cadthumb-test.3mf";
    if (!WriteTest3mf(model)) {
        bad(L"cannot write test model to " + dir);
    } else {
        Image img;
        std::string error;
        ULONGLONG t0 = GetTickCount64();
        if (RenderFileToImage(model, FileType::ThreeMf, 256, s, img, error))
            good(L"direct render " + std::to_wstring(GetTickCount64() - t0) + L" ms");
        else
            bad(L"direct render failed: " + Wide(error));
    }
    line(L"");

    // 5. exactly what Explorer does: shell item -> thumbnail handler -> service -> cache
    line(L"5. Windows thumbnail request (same path as Explorer)");
    {
        IShellItemImageFactory* f = nullptr;
        HRESULT hr = SHCreateItemFromParsingName(model.c_str(), nullptr, IID_PPV_ARGS(&f));
        HBITMAP bmp = nullptr;
        ULONGLONG t0 = GetTickCount64();
        if (SUCCEEDED(hr)) {
            hr = f->GetImage({256, 256}, SIIGBF_THUMBNAILONLY, &bmp);
            f->Release();
        }
        if (SUCCEEDED(hr) && bmp) {
            good(L"thumbnail produced in " + std::to_wstring(GetTickCount64() - t0) + L" ms");
            DeleteObject(bmp);
        } else {
            bad(L"no thumbnail, HRESULT " + Hex(hr) + L" after " + std::to_wstring(GetTickCount64() - t0) + L" ms");
        }
    }
    line(L"");

    // 6. log tail
    line(L"6. Last log lines (" + LogPath() + L")");
    if (FILE* f = _wfopen(LogPath().c_str(), L"rb")) {
        std::string all;
        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, n);
        fclose(f);
        std::vector<std::string> lines;
        size_t pos = 0;
        while (pos < all.size()) {
            size_t e = all.find('\n', pos);
            if (e == std::string::npos) e = all.size();
            lines.push_back(all.substr(pos, e - pos));
            pos = e + 1;
        }
        for (size_t i = lines.size() > 15 ? lines.size() - 15 : 0; i < lines.size(); ++i) {
            std::string l = lines[i];
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (l.size() >= 3 && (unsigned char)l[0] == 0xEF) l.erase(0, 3); // BOM
            line(L"  " + Wide(l));
        }
    } else {
        line(L"  (no log yet)");
    }
    line(L"");
    line(problems ? L"Problems found: " + std::to_wstring(problems) : L"No problems found.");

    // report file on the desktop (UTF-8 with BOM for Notepad)
    std::wstring reportPath;
    PWSTR desk = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk)) && desk) {
        reportPath = std::wstring(desk) + L"\\CadThumb-diagnostics.txt";
        CoTaskMemFree(desk);
        if (FILE* f = _wfopen(reportPath.c_str(), L"wb")) {
            std::string u8 = "\xEF\xBB\xBF" + Utf8(r);
            fwrite(u8.data(), 1, u8.size(), f);
            fclose(f);
        }
    }

    Out(r);
    if (!reportPath.empty()) Out(L"\r\nReport saved: " + reportPath + L"\r\n");
    if (interactive) {
        std::wstring text = r + L"\r\nОтчёт сохранён: " + reportPath;
        if (staleExplorer) {
            text += L"\r\n\r\nПроводник запущен до установки CadThumb и поэтому не показывает эскизы. "
                    L"Перезапустить Проводник сейчас?";
            if (MessageBoxW(nullptr, text.c_str(), L"CadThumb — диагностика", MB_YESNO | MB_ICONWARNING) == IDYES)
                RestartExplorer();
        } else {
            MessageBoxW(nullptr, text.c_str(), L"CadThumb — диагностика",
                        MB_OK | (problems ? MB_ICONWARNING : MB_ICONINFORMATION));
        }
    }
    return problems ? 1 : 0;
}

} // namespace ct
