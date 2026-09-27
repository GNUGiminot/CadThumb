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
void DeleteOurHandler(HKEY root, const std::wstring& classesPath) {
    std::wstring key = L"Software\\Classes\\" + classesPath + L"\\ShellEx\\" + kThumbnailHandlerKey;
    std::wstring v;
    if (GetStr(root, key, nullptr, v) && _wcsicmp(v.c_str(), kClsidString) == 0) RegDeleteTreeW(root, key.c_str());
}

std::wstring HandlerKey(const std::wstring& classesPath) {
    return L"Software\\Classes\\" + classesPath + L"\\ShellEx\\" + kThumbnailHandlerKey;
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
    std::vector<std::wstring> e = {L".step", L".stp", L".3mf"};
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

    for (const auto& ext : exts) {
        SetStr(root, HandlerKey(ext), nullptr, kClsidString);
        SetStr(root, HandlerKey(L"SystemFileAssociations\\" + ext), nullptr, kClsidString);
        report += L"  " + ext + L": зарегистрирован\r\n";

        // Some ProgIDs carry their own thumbnail handler that would win over the extension key.
        for (const auto& progId : ProgIdsFor(ext)) {
            std::wstring cur;
            std::wstring hk = std::wstring(progId) + L"\\ShellEx\\" + kThumbnailHandlerKey;
            if (!GetStr(HKEY_CLASSES_ROOT, hk, nullptr, cur) || _wcsicmp(cur.c_str(), kClsidString) == 0) continue;
            std::wstring own;
            if (GetStr(root, HandlerKey(progId), nullptr, own))
                SetStr(root, kStateKey, (L"Backup." + progId).c_str(), own);
            SetStr(root, HandlerKey(progId), nullptr, kClsidString);
            bool known = false;
            for (auto& o : overridden) known |= _wcsicmp(o.c_str(), progId.c_str()) == 0;
            if (!known) overridden.push_back(progId);
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
        report += L"  " + old + L": регистрация снята\r\n";
    }

    SetMulti(root, kStateKey, L"Extensions", exts);
    SetMulti(root, kStateKey, L"OverriddenProgIds", overridden);
    SetStr(root, kStateKey, L"InstallDir", DirOf(opt.exePath));

    if (opt.autostart) SetAutostart(true, opt.exePath);

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}

bool UnregisterShellExtension(bool machine, std::wstring& report) {
    HKEY root = machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    std::vector<std::wstring> exts = GetMulti(root, kStateKey, L"Extensions");
    if (exts.empty()) exts = HandledExtensions(true);

    for (const auto& ext : exts) {
        DeleteOurHandler(root, ext);
        DeleteOurHandler(root, L"SystemFileAssociations\\" + ext);
        RegDeleteTreeW(root, (L"Software\\Classes\\SystemFileAssociations\\" + ext + L"\\shell\\" + kVerbName).c_str());
        RegDeleteTreeW(root, (L"Software\\Classes\\SystemFileAssociations\\" + ext + L"\\shell\\" + kViewVerbName).c_str());
        DeleteValue(root, L"Software\\Classes\\" + ext + L"\\OpenWithProgids", kViewerProgId);
        report += L"  " + ext + L": регистрация снята\r\n";
    }
    RegDeleteTreeW(root, (std::wstring(L"Software\\Classes\\") + kViewerProgId).c_str());
    RegDeleteTreeW(root, (std::wstring(L"Software\\Classes\\Applications\\") + kExeName).c_str());
    for (const auto& progId : GetMulti(root, kStateKey, L"OverriddenProgIds")) {
        std::wstring backup;
        if (GetStr(root, kStateKey, (L"Backup." + progId).c_str(), backup))
            SetStr(root, HandlerKey(progId), nullptr, backup);
        else
            DeleteOurHandler(root, progId);
        report += L"  восстановлен обработчик ProgID " + progId + L"\r\n";
    }

    RegDeleteTreeW(root, (std::wstring(L"Software\\Classes\\CLSID\\") + kClsidString).c_str());
    DeleteValue(root, kApprovedKey, kClsidString);
    RegDeleteTreeW(root, kStateKey);
    SetAutostart(false, L"");

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
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
