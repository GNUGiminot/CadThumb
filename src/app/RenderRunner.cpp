#include "app/App.h"

#include "common/Cache.h"
#include "common/ImageIO.h"
#include "common/Log.h"
#include "common/Paths.h"
#include "render/RenderFile.h"

#include <cstdio>

namespace ct {

static std::wstring ReadTextFile(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return {};
    std::string s(4096, '\0');
    s.resize(fread(s.data(), 1, s.size(), f));
    fclose(f);
    return Wide(s);
}

static void WriteTextFile(const std::wstring& path, const std::string& text) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
}

int RenderCommand(const std::wstring& input, const std::wstring& output, FileType type, int size) {
    SetLogTag(L"render");
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const std::wstring errFile = output + L".err";
    DeleteFileW(errFile.c_str());

    if (type == FileType::Unknown) type = FileTypeFromExtension(ExtOf(input));
    Settings s = Settings::Get();
    Image img;
    std::string error, details;
    const ULONGLONG t0 = GetTickCount64();
    bool ok = RenderFileToImage(input, type, size, s, img, error, &details);
    if (ok && !SavePng(output, img)) {
        ok = false;
        error = "cannot write PNG";
    }
    if (!ok) {
        WriteTextFile(errFile, error);
        Out(L"ERROR: " + Wide(error) + L"\r\n");
        return 2;
    }
    Out(L"OK " + std::to_wstring(GetTickCount64() - t0) + L" ms: " + Wide(details) + L"\r\n");
    return 0;
}

bool RenderToCache(const std::wstring& input, FileType type, int bucket, const std::string& key, const Settings& s,
                   std::wstring* errorOut, const std::wstring& displayName) {
    const std::wstring name = displayName.empty() ? FileNameOf(input) : displayName;
    const std::wstring outPng = TempDir() + L"\\" + Wide(key) + L".out.png";
    const std::wstring errFile = outPng + L".err";
    DeleteFileW(outPng.c_str());
    DeleteFileW(errFile.c_str());

    const std::wstring exe = SelfExePath();
    std::wstring cmd = L"\"" + exe + L"\" --render \"" + input + L"\" \"" + outPng + L"\" --size " +
                       std::to_wstring(bucket) + L" --type " + FileTypeName(type);

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
                                           JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    lim.ProcessMemoryLimit = SIZE_T(s.memoryLimitMB) * 1024 * 1024;
    if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof(lim));

    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    const DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS;
    const ULONGLONG t0 = GetTickCount64();
    std::wstring error;
    bool ok = false;

    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, DirOf(exe).c_str(), &si,
                        &pi)) {
        error = L"cannot start render process (" + std::to_wstring(GetLastError()) + L")";
    } else {
        if (job) AssignProcessToJobObject(job, pi.hProcess);
        ResumeThread(pi.hThread);
        DWORD w = WaitForSingleObject(pi.hProcess, DWORD(s.renderTimeoutSec) * 1000);
        DWORD code = 1;
        if (w == WAIT_TIMEOUT) {
            if (job) TerminateJobObject(job, 1);
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            error = L"timeout after " + std::to_wstring(s.renderTimeoutSec) + L" s";
        } else {
            GetExitCodeProcess(pi.hProcess, &code);
            if (code == 0 && FileExists(outPng)) {
                ok = true;
            } else {
                error = ReadTextFile(errFile);
                if (error.empty()) {
                    wchar_t buf[64];
                    swprintf_s(buf, L"render process crashed (exit 0x%08lX)", code);
                    error = buf;
                }
            }
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    if (job) CloseHandle(job);
    DeleteFileW(errFile.c_str());

    const ULONGLONG ms = GetTickCount64() - t0;
    if (ok && CommitSuccess(key, outPng)) {
        Log(L"rendered %s (%s, %d px) in %llu ms", name.c_str(), FileTypeName(type), bucket, ms);
        return true;
    }
    DeleteFileW(outPng.c_str());
    if (error.empty()) error = L"cannot store result in cache";
    Log(L"FAILED %s (%s): %s", name.c_str(), FileTypeName(type), error.c_str());
    CommitFailure(key, error);
    if (errorOut) *errorOut = error;
    return false;
}

} // namespace ct
