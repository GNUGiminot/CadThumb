#include "common/PipeClient.h"
#include "common/Log.h"
#include "common/Paths.h"

#include <algorithm>
#include <vector>

namespace ct {

namespace {

class Deadline {
public:
    explicit Deadline(DWORD ms) : end_(GetTickCount64() + ms) {}
    DWORD Remaining() const {
        ULONGLONG now = GetTickCount64();
        return now >= end_ ? 0 : (DWORD)(end_ - now);
    }

private:
    ULONGLONG end_;
};

// Overlapped I/O with timeout on a pipe opened with FILE_FLAG_OVERLAPPED.
bool IoWithTimeout(HANDLE pipe, bool write, void* buf, DWORD len, const Deadline& dl, bool* timedOut) {
    char* p = static_cast<char*>(buf);
    DWORD done = 0;
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ev) return false;
    bool ok = true;
    while (done < len) {
        OVERLAPPED ov{};
        ov.hEvent = ev;
        ResetEvent(ev);
        DWORD n = 0;
        BOOL r = write ? WriteFile(pipe, p + done, len - done, &n, &ov) : ReadFile(pipe, p + done, len - done, &n, &ov);
        if (!r) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                ok = false;
                break;
            }
            DWORD w = WaitForSingleObject(ev, dl.Remaining());
            if (w != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &ov);
                GetOverlappedResult(pipe, &ov, &n, TRUE);
                if (timedOut) *timedOut = true;
                ok = false;
                break;
            }
            if (!GetOverlappedResult(pipe, &ov, &n, FALSE)) {
                ok = false;
                break;
            }
        }
        if (n == 0) {
            ok = false;
            break;
        }
        done += n;
    }
    CloseHandle(ev);
    return ok;
}

HANDLE ConnectPipe(DWORD waitMs) {
    std::wstring name = PipeName();
    Deadline dl(waitMs);
    for (;;) {
        HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
        DWORD err = GetLastError();
        if (err == ERROR_PIPE_BUSY) {
            if (!WaitNamedPipeW(name.c_str(), std::max<DWORD>(1, std::min<DWORD>(dl.Remaining(), 2000)))) {
                if (dl.Remaining() == 0) return INVALID_HANDLE_VALUE;
            }
            continue;
        }
        if (dl.Remaining() == 0) return INVALID_HANDLE_VALUE;
        Sleep(std::min<DWORD>(100, dl.Remaining()));
    }
}

} // namespace

bool IsHostRunning() {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, HostMutexName().c_str());
    if (m) {
        CloseHandle(m);
        return true;
    }
    return false;
}

bool StartHost(const std::wstring& hostExe, DWORD waitMs) {
    // Serialize concurrent starts from several thumbnail threads.
    HANDLE startLock = CreateMutexW(nullptr, FALSE, (L"Local\\CadThumb.Start." + CurrentUserSid()).c_str());
    DWORD lockResult = startLock ? WaitForSingleObject(startLock, 5000) : WAIT_FAILED;
    if (startLock && lockResult != WAIT_OBJECT_0 && lockResult != WAIT_ABANDONED) {
        CloseHandle(startLock);
        return false;
    }

    bool ok = IsHostRunning();
    if (!ok && FileExists(hostExe)) {
        std::wstring cmd = L"\"" + hostExe + L"\" --host";
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        // Break away from the thumbnail surrogate's job object (if allowed), so the host survives it.
        DWORD flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB;
        BOOL created = CreateProcessW(hostExe.c_str(), cmd.data(), nullptr, nullptr, FALSE, flags, nullptr,
                                      DirOf(hostExe).c_str(), &si, &pi);
        if (!created) {
            flags &= ~CREATE_BREAKAWAY_FROM_JOB;
            created = CreateProcessW(hostExe.c_str(), cmd.data(), nullptr, nullptr, FALSE, flags, nullptr,
                                     DirOf(hostExe).c_str(), &si, &pi);
        }
        if (created) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            Log(L"started host %s", hostExe.c_str());
        } else {
            Log(L"failed to start host %s (err %lu)", hostExe.c_str(), GetLastError());
        }
    }
    if (startLock) {
        ReleaseMutex(startLock);
        CloseHandle(startLock);
    }
    // wait for the pipe to appear
    Deadline dl(waitMs);
    while (dl.Remaining() > 0) {
        if (WaitNamedPipeW(PipeName().c_str(), 50)) return true;
        if (GetLastError() == ERROR_SEM_TIMEOUT) return true; // exists but busy
        Sleep(100);
    }
    return ok;
}

RenderRequestResult RequestRender(const PipeRequest& reqIn, IStream* data, DWORD waitMs, const std::wstring& hostExe) {
    Deadline dl(waitMs);
    HANDLE pipe = ConnectPipe(300);
    if (pipe == INVALID_HANDLE_VALUE) {
        if (!StartHost(hostExe, std::min<DWORD>(dl.Remaining(), 5000))) return RenderRequestResult::NoHost;
        pipe = ConnectPipe(std::min<DWORD>(dl.Remaining(), 3000));
        if (pipe == INVALID_HANDLE_VALUE) return RenderRequestResult::NoHost;
    }

    PipeRequest req = reqIn;
    bool timedOut = false;
    RenderRequestResult result = RenderRequestResult::Failed;
    PipeResponse resp{};
    do {
        if (!IoWithTimeout(pipe, true, &req, sizeof(req), dl, &timedOut)) break;
        if (!IoWithTimeout(pipe, false, &resp, sizeof(resp), dl, &timedOut) || resp.magic != kPipeMagic) break;

        if (resp.status == RespCached) {
            result = RenderRequestResult::Done;
            break;
        }
        if (resp.status == RespFailed || resp.status == RespBusy || resp.status == RespBadRequest) break;

        if (resp.status == RespSendData) {
            LARGE_INTEGER zero{};
            if (FAILED(data->Seek(zero, STREAM_SEEK_SET, nullptr))) break;
            std::vector<char> buf(1 << 20);
            uint64_t left = req.dataSize;
            bool sendOk = true;
            while (left > 0) {
                ULONG want = (ULONG)std::min<uint64_t>(left, buf.size());
                ULONG got = 0;
                HRESULT hr = data->Read(buf.data(), want, &got);
                if (FAILED(hr) || got == 0) {
                    sendOk = false;
                    break;
                }
                if (!IoWithTimeout(pipe, true, buf.data(), got, dl, &timedOut)) {
                    sendOk = false;
                    break;
                }
                left -= got;
            }
            if (!sendOk) break;
        }
        // Inflight or data sent: wait for completion
        if (!IoWithTimeout(pipe, false, &resp, sizeof(resp), dl, &timedOut) || resp.magic != kPipeMagic) break;
        result = resp.status == RespDone ? RenderRequestResult::Done : RenderRequestResult::Failed;
    } while (false);

    CloseHandle(pipe);
    if (timedOut) return RenderRequestResult::Timeout;
    return result;
}

} // namespace ct
