#include "app/PipeServer.h"

#include "app/JobManager.h"
#include "common/Cache.h"
#include "common/Log.h"
#include "common/Paths.h"
#include "common/Protocol.h"
#include "common/Settings.h"

#include <sddl.h>
#include <vector>

namespace ct {

namespace {

std::atomic<int> g_clients{0};

bool ReadAll(HANDLE h, void* buf, DWORD len) {
    char* p = static_cast<char*>(buf);
    while (len) {
        DWORD n = 0;
        if (!ReadFile(h, p, len, &n, nullptr) || n == 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

bool WriteAll(HANDLE h, const void* buf, DWORD len) {
    const char* p = static_cast<const char*>(buf);
    while (len) {
        DWORD n = 0;
        if (!WriteFile(h, p, len, &n, nullptr)) return false;
        p += n;
        len -= n;
    }
    return true;
}

bool Send(HANDLE h, uint32_t status) {
    PipeResponse r;
    r.status = status;
    return WriteAll(h, &r, sizeof(r));
}

bool ValidKey(const char* key) {
    size_t n = strnlen(key, sizeof(PipeRequest::key));
    if (n != 32) return false;
    for (size_t i = 0; i < n; ++i)
        if (!isxdigit((unsigned char)key[i])) return false;
    return true;
}

void HandleClient(HANDLE pipe) {
    ++g_clients;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto& jm = JobManager::Instance();
    PipeRequest req;
    do {
        if (!ReadAll(pipe, &req, sizeof(req)) || req.magic != kPipeMagic || req.version != kPipeVersion) break;
        if (req.kind == ReqPing) {
            Send(pipe, RespPong);
            break;
        }
        req.key[sizeof(req.key) - 1] = 0;
        req.name[259] = 0;
        FileType type = FileType(req.fileType);
        if (req.kind != ReqRender || !ValidKey(req.key) || type == FileType::Unknown ||
            FileType(req.fileType) > FileType::Stl) {
            Send(pipe, RespBadRequest);
            break;
        }
        const Settings s = Settings::Get();
        const std::string key = req.key;
        const int bucket = req.bucket <= 256 ? 256 : req.bucket <= 512 ? 512 : 1024;
        const DWORD maxWait = DWORD(s.renderTimeoutSec + 30) * 1000;

        JobManager::JobPtr job;
        switch (jm.Reserve(key, type, bucket, req.name, job)) {
        case JobManager::ReserveResult::Cached:
            Send(pipe, RespCached);
            break;
        case JobManager::ReserveResult::KnownFailure:
            Send(pipe, RespFailed);
            break;
        case JobManager::ReserveResult::Existing:
            if (!Send(pipe, RespInflight)) break;
            jm.Wait(job, maxWait);
            Send(pipe, job->state == JobManager::Job::Done ? RespDone : RespFailed);
            break;
        case JobManager::ReserveResult::New:
            break;
        }

        if (req.dataSize == 0 || req.dataSize > uint64_t(s.maxFileSizeMB) * 1024 * 1024) {
            jm.Abandon(job);
            Send(pipe, RespFailed);
            break;
        }
        if (!Send(pipe, RespSendData)) {
            jm.Abandon(job);
            break;
        }
        // receive the file into a temp copy (the client may be a low-integrity process
        // which cannot write to our folders itself)
        const std::wstring tmp = TempDir() + L"\\" + Wide(key) + FileTypeExt(type);
        HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr);
        bool received = f != INVALID_HANDLE_VALUE;
        if (received) {
            std::vector<char> buf(1 << 20);
            uint64_t left = req.dataSize;
            while (left > 0) {
                DWORD want = (DWORD)std::min<uint64_t>(left, buf.size());
                DWORD n = 0;
                if (!ReadFile(pipe, buf.data(), want, &n, nullptr) || n == 0) {
                    received = false;
                    break;
                }
                DWORD w = 0;
                if (!WriteFile(f, buf.data(), n, &w, nullptr) || w != n) {
                    received = false;
                    break;
                }
                left -= n;
            }
            CloseHandle(f);
        }
        if (!received) {
            DeleteFileW(tmp.c_str());
            jm.Abandon(job);
            LogVerbose(L"%s: client disconnected while sending data", req.name);
            break;
        }
        jm.Enqueue(job, tmp, true, true);
        jm.Wait(job, maxWait);
        Send(pipe, job->state == JobManager::Job::Done ? RespDone : RespFailed);
    } while (false);

    FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    CoUninitialize();
    --g_clients;
}

} // namespace

bool PipeServer::Start() {
    // Current user + SYSTEM only. Mandatory label = Low, so the thumbnail surrogate can still connect
    // (write to the pipe) even if Windows runs it at low integrity.
    std::wstring sddl = L"D:(A;;GA;;;" + CurrentUserSid() + L")(A;;GA;;;SY)S:(ML;;NW;;;LW)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd_, nullptr)) {
        Log(L"cannot build pipe security descriptor (%lu)", GetLastError());
        return false;
    }
    sa_.nLength = sizeof(sa_);
    sa_.lpSecurityDescriptor = sd_;
    sa_.bInheritHandle = FALSE;
    stop_ = false;
    thread_ = std::thread(&PipeServer::AcceptLoop, this);
    return true;
}

void PipeServer::Stop() {
    stop_ = true;
    // unblock ConnectNamedPipe
    HANDLE h = CreateFileW(PipeName().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (thread_.joinable()) thread_.join();
    if (sd_) LocalFree(sd_);
    sd_ = nullptr;
}

void PipeServer::AcceptLoop() {
    const std::wstring name = PipeName();
    while (!stop_) {
        HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                       PIPE_UNLIMITED_INSTANCES, 1 << 20, 1 << 20, 0, &sa_);
        if (pipe == INVALID_HANDLE_VALUE) {
            Log(L"CreateNamedPipe failed (%lu)", GetLastError());
            Sleep(1000);
            continue;
        }
        BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (stop_) {
            CloseHandle(pipe);
            break;
        }
        if (!connected || g_clients > 256) {
            CloseHandle(pipe);
            continue;
        }
        std::thread(HandleClient, pipe).detach();
    }
}

} // namespace ct
