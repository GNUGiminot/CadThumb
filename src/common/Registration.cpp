#include "common/Registration.h"
#include "common/Paths.h"

#include <windows.h>
#include <shlobj.h>

namespace ct {

namespace {

const wchar_t kStateKey[] = L"Software\\CadThumb";
const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t kApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved";
const wchar_t kVerbName[] = L"CadThumb.Refresh";
const wchar_t kViewVerbName[] = L"CadThumb.View";
const wchar_t kViewerProgId[] = L"CadThumb.Model3D";

bool SetStr(HKEY root, const std::wstring& sub, const wchar_t* name, const std::wstring& value) {
    return RegSetKeyValueW(root, sub.c_str(), name, REG_SZ, value.c_str(),
                           DWORD((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool GetStr(HKEY root, const std::wstring& sub, const wchar_t* name, std::wstring& out) {
    wchar_t buf[2048];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, sub.c_str(), name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, buf,
                     &size) != ERROR_SUCCESS)
        return false;
    out = buf;
    return true;
}

bool SetMulti(HKEY root, const std::wstring& sub, const wchar_t* name, const std::vector<std::wstring>& items) {
    std::wstring data;
    for (auto& s : items) {
        data += s;
        data.push_back(L'\0');
    }
    data.push_back(L'\0');
    return RegSetKeyValueW(root, sub.c_str(), name, REG_MULTI_SZ, data.data(),
                           DWORD(data.size() * sizeof(wchar_t))) == ERROR_SUCCESS;
}

std::vector<std::wstring> GetMulti(HKEY root, const std::wstring& sub, const wchar_t* name) {
    std::vector<std::wstring> out;
    DWORD size = 0;
    if (RegGetValueW(root, sub.c_str(), name, RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS)
        return out;
    std::wstring buf(size / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(root, sub.c_str(), name, RRF_RT_REG_MULTI_SZ, nullptr, buf.data(), &size) != ERROR_SUCCESS)
        return out;
    for (const wchar_t* p = buf.c_str(); *p; p += wcslen(p) + 1) out.emplace_back(p);
    return out;
}

void DeleteValue(HKEY root, const std::wstring& sub, const wchar_t* name) {
    HKEY k;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, name);
        RegCloseKey(k);
    }
}

// Deletes Classes\<path>\ShellEx\{thumb} only when it points to our CLSID.
bool DeleteOurHandler(HKEY root, const std::wstring& classesPath) {
    std::wstring key = L"Software\\Classes\\" + classesPath + L"\\ShellEx\\" + kThumbnailHandlerKey;
    std::wstring v;
    if (GetStr(root, key, nullptr, v) && _wcsicmp(v.c_str(), kClsidString) == 0) {
        LSTATUS result = RegDeleteTreeW(root, key.c_str());
        return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
    }
    return true;
}

std::wstring HandlerKey(const std::wstring& classesPath) {
    return L"Software\\Classes\\" + classesPath + L"\\ShellEx\\" + kThumbnailHandlerKey;
}

bool RestoreHandler(HKEY root, const std::wstring& path) {
    std::wstring current, backup;
    if (GetStr(root, HandlerKey(path), nullptr, current) && _wcsicmp(current.c_str(), kClsidString) == 0) {
        bool changed = GetStr(root, kStateKey, (L"Backup." + path).c_str(), backup)
            ? SetStr(root, HandlerKey(path), nullptr, backup)
            : DeleteOurHandler(root, path);
        if (!changed) return false;
    }
    DeleteValue(root, kStateKey, (L"Backup." + path).c_str());
    return true;
}

std::vector<std::wstring> ProgIdsFor(const std::wstring& ext) {
    std::vector<std::wstring> ids;
    std::wstring v;
    if (GetStr(HKEY_CLASSES_ROOT, ext, nullptr, v) && !v.empty()) ids.push_back(v);
    if (GetStr(HKEY_CURRENT_USER,
               L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" + ext + L"\\UserChoice",
               L"ProgId", v) &&
        !v.empty()) {
        bool dup = false;
        for (auto& i : ids) dup |= _wcsicmp(i.c_str(), v.c_str()) == 0;
        if (!dup) ids.push_back(v);
    }
    return ids;
}

} // namespace

std::vector<std::wstring> HandledExtensions(bool includeStl) {
    std::vector<std::wstring> e = {L".step", L".stp", L".p21", L".3mf"};
    if (includeStl) e.push_back(L".stl");
    return e;
}

bool RegisterShellExtension(const RegisterOptions& opt, std::wstring& report) {
    HKEY root = opt.machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    const std::wstring clsidKey = std::wstring(L"Software\\Classes\\CLSID\\") + kClsidString;
    bool ok = true;

    ok &= SetStr(root, clsidKey, nullptr, L"CadThumb STEP/3MF/STL Thumbnail Provider");
    ok &= SetStr(root, clsidKey + L"\\InprocServer32", nullptr, opt.dllPath);
    ok &= SetStr(root, clsidKey + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
    // Machine-wide: Windows hosts the handler in its isolated thumbnail surrogate (dllhost).
    // Per-user: that surrogate does not see HKCU class registrations (REGDB_E_CLASSNOTREG), so the
    // handler must run inside Explorer. That is safe enough here: the DLL only talks to the pipe and
    // reads 3MF thumbnails; all model parsing happens in the separate CadThumb.exe render process.
    {
        HKEY k;
        if (RegCreateKeyExW(root, clsidKey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
            if (opt.machine) {
                RegDeleteValueW(k, L"DisableProcessIsolation");
            } else {
                DWORD one = 1;
                RegSetValueExW(k, L"DisableProcessIsolation", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
            }
            RegCloseKey(k);
        }
    }
    if (!ok) {
        report += L"Не удалось записать ключ CLSID (нужны права администратора для --machine?)\r\n";
        return false;
    }
    SetStr(root, kApprovedKey, kClsidString, L"CadThumb Thumbnail Provider");

    std::vector<std::wstring> exts = HandledExtensions(opt.includeStl);
    std::vector<std::wstring> overridden = GetMulti(root, kStateKey, L"OverriddenProgIds");
    std::vector<std::wstring> desired;
    for (const auto& ext : exts) {
        desired.push_back(ext);
        desired.push_back(L"SystemFileAssociations\\" + ext);
        for (const auto& id : ProgIdsFor(ext)) desired.push_back(id);
    }
    for (auto it = overridden.begin(); it != overridden.end();) {
        bool keep = false;
        for (const auto& path : desired) keep |= _wcsicmp(path.c_str(), it->c_str()) == 0;
        if (keep) { ++it; continue; }
        if (!RestoreHandler(root, *it)) {
            ok = false;
            ++it;
        } else {
            it = overridden.erase(it);
        }
    }
    auto installHandler = [&](const std::wstring& path) {
        std::wstring own;
        if (GetStr(root, HandlerKey(path), nullptr, own) && _wcsicmp(own.c_str(), kClsidString) != 0)
            ok &= SetStr(root, kStateKey, (L"Backup." + path).c_str(), own);
        ok &= SetStr(root, HandlerKey(path), nullptr, kClsidString);
        bool known = false;
        for (const auto& saved : overridden) known |= _wcsicmp(saved.c_str(), path.c_str()) == 0;
        if (!known) overridden.push_back(path);
    };

    for (const auto& ext : exts) {
        installHandler(ext);
        installHandler(L"SystemFileAssociations\\" + ext);
        report += L"  " + ext + L": зарегистрирован\r\n";

        // Some ProgIDs carry their own thumbnail handler that would win over the extension key.
        for (const auto& progId : ProgIdsFor(ext)) {
            std::wstring cur;
            std::wstring hk = std::wstring(progId) + L"\\ShellEx\\" + kThumbnailHandlerKey;
            if (!GetStr(HKEY_CLASSES_ROOT, hk, nullptr, cur) || _wcsicmp(cur.c_str(), kClsidString) == 0) continue;
            installHandler(progId);
            report += L"    перекрыт обработчик ProgID " + progId + L" (" + cur + L")\r\n";
        }

        if (opt.contextMenu) {
            std::wstring verb = L"Software\\Classes\\SystemFileAssociations\\" + ext + L"\\shell\\" + kVerbName;
            SetStr(root, verb, nullptr, L"Обновить эскиз (CadThumb)");
            SetStr(root, verb, L"Icon", L"\"" + opt.exePath + L"\",0");
            SetStr(root, verb, L"MultiSelectModel", L"Player");
            SetStr(root, verb + L"\\command", nullptr, L"\"" + opt.exePath + L"\" --refresh \"%1\"");

            std::wstring view = L"Software\\Classes\\SystemFileAssociations\\" + ext + L"\\shell\\" + kViewVerbName;
            SetStr(root, view, nullptr, L"Просмотр 3D (CadThumb)");
            SetStr(root, view, L"Icon", L"\"" + opt.exePath + L"\",0");
            SetStr(root, view + L"\\command", nullptr, L"\"" + opt.exePath + L"\" --view \"%1\"");
        } else {
            const auto shell = L"Software\\Classes\\SystemFileAssociations\\" + ext + L"\\shell\\";
            RegDeleteTreeW(root, (shell + kVerbName).c_str());
            RegDeleteTreeW(root, (shell + kViewVerbName).c_str());
        }
        // "Open with" list; the default program (CAD, slicer) stays untouched.
        HKEY k;
        if (RegCreateKeyExW(root, (L"Software\\Classes\\" + ext + L"\\OpenWithProgids").c_str(), 0, nullptr, 0,
                            KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
            RegSetValueExW(k, kViewerProgId, 0, REG_NONE, nullptr, 0);
            RegCloseKey(k);
        }
        SetStr(root, std::wstring(L"Software\\Classes\\Applications\\") + kExeName + L"\\SupportedTypes", ext.c_str(), L"");
    }

    const std::wstring progId = std::wstring(L"Software\\Classes\\") + kViewerProgId;
    SetStr(root, progId, nullptr, L"3D-модель");
    SetStr(root, progId + L"\\DefaultIcon", nullptr, L"\"" + opt.exePath + L"\",0");
    SetStr(root, progId + L"\\shell\\open", L"FriendlyAppName", L"CadThumb — просмотр 3D");
    SetStr(root, progId + L"\\shell\\open\\command", nullptr, L"\"" + opt.exePath + L"\" --view \"%1\"");
    const std::wstring app = std::wstring(L"Software\\Classes\\Applications\\") + kExeName;
    SetStr(root, app, L"FriendlyAppName", L"CadThumb — просмотр 3D");
    SetStr(root, app + L"\\shell\\open\\command", nullptr, L"\"" + opt.exePath + L"\" --view \"%1\"");

    // Drop registrations of extensions that are no longer requested (e.g. STL turned off).
    for (const auto& old : GetMulti(root, kStateKey, L"Extensions")) {
        bool still = false;
        for (auto& e : exts) still |= _wcsicmp(e.c_str(), old.c_str()) == 0;
        if (still) continue;
        DeleteOurHandler(root, old);
        DeleteOurHandler(root, L"SystemFileAssociations\\" + old);
        RegDeleteTreeW(root, (L"Software\\Classes\\SystemFileAssociations\\" + old + L"\\shell\\" + kVerbName).c_str());
        RegDeleteTreeW(root, (L"Software\\Classes\\SystemFileAssociations\\" + old + L"\\shell\\" + kViewVerbName).c_str());
        DeleteValue(root, L"Software\\Classes\\" + old + L"\\OpenWithProgids", kViewerProgId);
        DeleteValue(root, std::wstring(L"Software\\Classes\\Applications\\") + kExeName + L"\\SupportedTypes", old.c_str());
        report += L"  " + old + L": регистрация снята\r\n";
    }

    ok &= SetMulti(root, kStateKey, L"Extensions", exts);
    ok &= SetMulti(root, kStateKey, L"OverriddenProgIds", overridden);
    SetStr(root, kStateKey, L"InstallDir", DirOf(opt.exePath));

    ok &= SetAutostart(opt.autostart, opt.exePath);

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok;
}

bool UnregisterShellExtension(bool machine, std::wstring& report) {
    HKEY root = machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    std::vector<std::wstring> exts = GetMulti(root, kStateKey, L"Extensions");
    if (exts.empty()) exts = HandledExtensions(true);
    std::vector<std::wstring> overridden = GetMulti(root, kStateKey, L"OverriddenProgIds");
    bool ok = true;

    for (const auto& ext : exts) {
        ok &= RestoreHandler(root, ext);
        ok &= RestoreHandler(root, L"SystemFileAssociations\\" + ext);
        RegDeleteTreeW(root, (L"Software\\Classes\\SystemFileAssociations\\" + ext + L"\\shell\\" + kVerbName).c_str());
        RegDeleteTreeW(root, (L"Software\\Classes\\SystemFileAssociations\\" + ext + L"\\shell\\" + kViewVerbName).c_str());
        DeleteValue(root, L"Software\\Classes\\" + ext + L"\\OpenWithProgids", kViewerProgId);
        report += L"  " + ext + L": регистрация снята\r\n";
    }
    RegDeleteTreeW(root, (std::wstring(L"Software\\Classes\\") + kViewerProgId).c_str());
    RegDeleteTreeW(root, (std::wstring(L"Software\\Classes\\Applications\\") + kExeName).c_str());
    for (const auto& progId : overridden) {
        ok &= RestoreHandler(root, progId);
        report += L"  восстановлен обработчик ProgID " + progId + L"\r\n";
    }

    // A successful exit must not leave Explorer pointing at an uninstalled DLL.
    for (const auto& path : overridden) {
        std::wstring value;
        if (GetStr(root, HandlerKey(path), nullptr, value) && _wcsicmp(value.c_str(), kClsidString) == 0)
            ok = false;
    }
    if (ok) {
        const auto deleted = RegDeleteTreeW(root, (std::wstring(L"Software\\Classes\\CLSID\\") + kClsidString).c_str());
        ok &= deleted == ERROR_SUCCESS || deleted == ERROR_FILE_NOT_FOUND;
    }
    if (ok) {
        DeleteValue(root, kApprovedKey, kClsidString);
        SetAutostart(false, L"");
        const auto stateDeleted = RegDeleteTreeW(root, kStateKey);
        ok &= stateDeleted == ERROR_SUCCESS || stateDeleted == ERROR_FILE_NOT_FOUND;
    }
    if (!ok) report += L"Не удалось полностью снять регистрацию CadThumb. Проверьте права доступа к реестру.\r\n";

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok;
}

bool IsAutostartEnabled() {
    std::wstring v;
    return GetStr(HKEY_CURRENT_USER, kRunKey, L"CadThumb", v) && !v.empty();
}

bool SetAutostart(bool enable, const std::wstring& exePath) {
    if (!enable) {
        DeleteValue(HKEY_CURRENT_USER, kRunKey, L"CadThumb");
        return true;
    }
    return SetStr(HKEY_CURRENT_USER, kRunKey, L"CadThumb", L"\"" + exePath + L"\" --host");
}

std::wstring DescribeThumbnailHandlers() {
    std::wstring out;
    for (const auto& ext : HandledExtensions(true)) {
        std::wstring clsid;
        std::wstring where;
        if (GetStr(HKEY_CLASSES_ROOT, ext + L"\\ShellEx\\" + kThumbnailHandlerKey, nullptr, clsid)) where = ext;
        for (const auto& p : ProgIdsFor(ext)) {
            std::wstring c;
            if (GetStr(HKEY_CLASSES_ROOT, p + L"\\ShellEx\\" + kThumbnailHandlerKey, nullptr, c)) {
                clsid = c;
                where = p;
                break;
            }
        }
        if (clsid.empty() && GetStr(HKEY_CLASSES_ROOT,
                                    L"SystemFileAssociations\\" + ext + L"\\ShellEx\\" + kThumbnailHandlerKey,
                                    nullptr, clsid))
            where = L"SystemFileAssociations\\" + ext;
        std::wstring name;
        if (!clsid.empty()) GetStr(HKEY_CLASSES_ROOT, L"CLSID\\" + clsid, nullptr, name);
        bool ours = _wcsicmp(clsid.c_str(), kClsidString) == 0;
        out += ext + L": " + (clsid.empty() ? L"(нет)" : (ours ? L"CadThumb" : name + L" " + clsid)) +
               (where.empty() ? L"" : L"  [" + where + L"]") + L"\r\n";
    }
    return out;
}

} // namespace ct
