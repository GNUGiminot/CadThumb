// Keep the registry probe and option-merging rules under test without running an installation.
// RegOverridePredefKey redirects both hives inside this process only.
#include "../src/installer/main.cpp"
#include "common/Registration.h"

#include <cstdio>
#include <filesystem>

int main() {
    if (CompareVersions(L"1.0.10", L"1.0.2") <= 0 ||
        CompareVersions(L"1.0.1", L"1.0.2") >= 0 ||
        CompareVersions(L"1.0.2", L"1.0.2") != 0) {
        fprintf(stderr, "version comparison would allow an accidental downgrade\n");
        return 1;
    }
    const std::wstring key = L"Software\\CadThumbSetupTests\\" + std::to_wstring(GetCurrentProcessId());
    HKEY base = nullptr, user = nullptr, machine = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
                        nullptr, &base, nullptr) != ERROR_SUCCESS ||
        RegCreateKeyExW(base, L"User", 0, nullptr, 0, KEY_ALL_ACCESS,
                        nullptr, &user, nullptr) != ERROR_SUCCESS ||
        RegCreateKeyExW(base, L"Machine", 0, nullptr, 0, KEY_ALL_ACCESS,
                        nullptr, &machine, nullptr) != ERROR_SUCCESS) {
        fprintf(stderr, "cannot create isolated registry test keys\n");
        return 1;
    }

    int failed = 0;
    if (RegOverridePredefKey(HKEY_CURRENT_USER, user) != ERROR_SUCCESS ||
        RegOverridePredefKey(HKEY_LOCAL_MACHINE, machine) != ERROR_SUCCESS) {
        fprintf(stderr, "cannot isolate registry hives\n");
        failed = 1;
    } else {
        const wchar_t extensions[] = L".step\0.stl\0";
        RegSetKeyValueW(HKEY_LOCAL_MACHINE, kUninstallKey, L"DisplayName", REG_SZ,
                        L"CadThumb", sizeof(L"CadThumb"));
        RegSetKeyValueW(HKEY_LOCAL_MACHINE, kUninstallKey, L"DisplayVersion", REG_SZ,
                        L"1.0.0", sizeof(L"1.0.0"));
        RegSetKeyValueW(HKEY_LOCAL_MACHINE, L"Software\\CadThumb", L"InstallDir", REG_SZ,
                        L"C:\\Program Files\\CadThumb\\app-old", sizeof(L"C:\\Program Files\\CadThumb\\app-old"));
        RegSetKeyValueW(HKEY_LOCAL_MACHINE, L"Software\\CadThumb", L"Extensions", REG_MULTI_SZ,
                        extensions, sizeof(extensions));
        InstalledState found = ReadInstalled(true);
        if (!found.present || !found.hasEntry || !found.machine || !found.stl || found.version != L"1.0.0") {
            fprintf(stderr, "machine installation was not recognized\n");
            ++failed;
        }
        if (ReadInstalled(false).present) {
            fprintf(stderr, "machine installation leaked into user scope\n");
            ++failed;
        }

        Options update;
        KeepPreviousChoices(update, found);
        if (!update.machine || !update.stl) {
            fprintf(stderr, "update did not preserve scope and STL setting\n");
            ++failed;
        }
        Options optOut;
        optOut.stl = false;
        optOut.stlExplicit = true;
        KeepPreviousChoices(optOut, found);
        if (optOut.stl || !optOut.machine) {
            fprintf(stderr, "explicit STL opt-out was overwritten\n");
            ++failed;
        }

        RegSetKeyValueW(HKEY_CURRENT_USER, kUninstallKey, L"DisplayName", REG_SZ,
                        L"CadThumb", sizeof(L"CadThumb"));
        if (!ReadInstalled(false).present || !ReadInstalled(true).present) {
            fprintf(stderr, "parallel user/machine installations were not detected\n");
            ++failed;
        }

        // Verify that removing CadThumb restores an overwritten third-party handler.
        const wchar_t onlyStep[] = L".step\0";
        const wchar_t overridden[] = L".step\0SystemFileAssociations\\.step\0";
        const wchar_t previousHandler[] = L"{E64164EB-1AE0-4C50-BAEF-A413C2B3A4BC}";
        const std::wstring extHandler = std::wstring(L"Software\\Classes\\.step\\ShellEx\\") + ct::kThumbnailHandlerKey;
        const std::wstring systemHandler = std::wstring(L"Software\\Classes\\SystemFileAssociations\\.step\\ShellEx\\") +
                                           ct::kThumbnailHandlerKey;
        RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\CadThumb", L"Extensions", REG_MULTI_SZ,
                        onlyStep, sizeof(onlyStep));
        RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\CadThumb", L"OverriddenProgIds", REG_MULTI_SZ,
                        overridden, sizeof(overridden));
        RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\CadThumb", L"Backup.SystemFileAssociations\\.step",
                        REG_SZ, previousHandler, sizeof(previousHandler));
        RegSetKeyValueW(HKEY_CURRENT_USER, extHandler.c_str(), nullptr, REG_SZ,
                        ct::kClsidString, sizeof(ct::kClsidString));
        RegSetKeyValueW(HKEY_CURRENT_USER, systemHandler.c_str(), nullptr, REG_SZ,
                        ct::kClsidString, sizeof(ct::kClsidString));
        std::wstring removeReport;
        if (!ct::UnregisterShellExtension(false, removeReport)) {
            fprintf(stderr, "unregistration returned failure\n");
            ++failed;
        }
        wchar_t restored[128]{};
        DWORD restoredSize = sizeof(restored);
        if (RegGetValueW(HKEY_CURRENT_USER, systemHandler.c_str(), nullptr, RRF_RT_REG_SZ,
                         nullptr, restored, &restoredSize) != ERROR_SUCCESS ||
            wcscmp(restored, previousHandler) != 0) {
            fprintf(stderr, "previous thumbnail handler was not restored\n");
            ++failed;
        }
        restoredSize = sizeof(restored);
        if (RegGetValueW(HKEY_CURRENT_USER, extHandler.c_str(), nullptr, RRF_RT_REG_SZ,
                         nullptr, restored, &restoredSize) == ERROR_SUCCESS) {
            fprintf(stderr, "CadThumb thumbnail handler remained after removal\n");
            ++failed;
        }
    }

    RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    RegCloseKey(machine);
    RegCloseKey(user);
    RegCloseKey(base);
    RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());

    std::error_code ec;
    const auto folder = std::filesystem::temp_directory_path() /
                        (L"CadThumbInstallerTest-" + std::to_wstring(GetCurrentProcessId()));
    const auto old = folder / L"app-old";
    const auto keep = folder / L"app-current";
    const auto unrelated = folder / L"user-data";
    std::filesystem::create_directories(old, ec);
    std::filesystem::create_directories(keep, ec);
    std::filesystem::create_directories(unrelated, ec);
    const auto lockedPath = old / L"CadThumbShell.dll";
    HANDLE locked = CreateFileW(lockedPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (locked == INVALID_HANDLE_VALUE) return 1;
    auto pending = RemoveOldVersions(folder.wstring(), keep.wstring());
    if (pending.size() != 1 || pending.front() != old) {
        fprintf(stderr, "locked old version was not reported for deferred deletion\n");
        ++failed;
    }
    CloseHandle(locked);
    if (!RemoveOldVersions(folder.wstring(), keep.wstring()).empty()) {
        fprintf(stderr, "old version was not removed after releasing the DLL\n");
        ++failed;
    }
    if (std::filesystem::exists(old) || !std::filesystem::exists(keep) ||
        !std::filesystem::exists(unrelated)) {
        fprintf(stderr, "upgrade cleanup removed the wrong folder\n");
        ++failed;
    }
    std::filesystem::remove_all(folder, ec);
    return failed ? 1 : 0;
}
