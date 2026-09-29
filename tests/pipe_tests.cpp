#include "app/App.h"
#include "app/JobManager.h"
#include "app/PipeServer.h"
#include "common/Cache.h"
#include "common/Paths.h"
#include "common/Protocol.h"
#include "common/Settings.h"

#include <sddl.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

namespace ct {

Settings Settings::Get() {
    Settings s;
    s.maxParallel = 1;
    s.renderTimeoutSec = 5;
    return s;
}

CacheState QueryCache(const std::string& key, int) {
    return key[0] == 'a' ? CacheState::Ready : CacheState::Missing;
}

std::wstring TempDir() { return CADTHUMB_TEST_DIR; }
std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
const wchar_t* FileTypeExt(FileType) { return L".stl"; }

std::wstring CurrentUserSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::string buf(size, '\0');
    LPWSTR sid = nullptr;
    if (GetTokenInformation(token, TokenUser, buf.data(), size, &size))
        ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &sid);
    std::wstring result = sid ? sid : L"";
    if (sid) LocalFree(sid);
    CloseHandle(token);
    return result;
}

std::wstring PipeName() {
    return L"\\\\.\\pipe\\CadThumb.Tests." + CurrentUserSid() + L"." + std::to_wstring(GetCurrentProcessId());
}

void Log(const wchar_t*, ...) {}
void LogVerbose(const wchar_t*, ...) {}

bool RenderToCache(const std::wstring&, FileType, int, const std::string&, const Settings&,
                   std::wstring*, const std::wstring&) {
    Sleep(500); // keep the first request active while a second client joins it
    return true;
}

} // namespace ct

namespace {

int failures = 0;
void Check(bool condition, const char* label) {
    if (!condition) {
        ++failures;
        fprintf(stderr, "FAIL: %s\n", label);
    }
}

HANDLE Connect() {
    for (int i = 0; i < 100; ++i) {
        HANDLE h = CreateFileW(ct::PipeName().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
        Sleep(10);
    }
    return INVALID_HANDLE_VALUE;
}

ct::PipeRequest Request(char key) {
    ct::PipeRequest r;
    r.kind = ct::ReqRender;
    r.fileType = uint32_t(ct::FileType::Stl);
    r.dataSize = 1;
    memset(r.key, key, 32);
    return r;
}

bool Write(HANDLE h, const void* buf, DWORD size) {
    DWORD n = 0;
    return WriteFile(h, buf, size, &n, nullptr) && n == size;
}

int ReadStatus(HANDLE h) {
    ct::PipeResponse r{};
    DWORD n = 0;
    if (!ReadFile(h, &r, sizeof(r), &n, nullptr) || n != sizeof(r) || r.magic != ct::kPipeMagic)
        return -1;
    return int(r.status);
}

} // namespace

int main() {
    std::filesystem::create_directories(ct::TempDir());
    ct::JobManager::Instance().Start();
    ct::PipeServer server;
    Check(server.Start(), "pipe server starts");

    HANDLE cached = Connect();
    Check(cached != INVALID_HANDLE_VALUE, "cached client connects");
    if (cached != INVALID_HANDLE_VALUE) {
        auto req = Request('a');
        Check(Write(cached, &req, sizeof(req)) && ReadStatus(cached) == ct::RespCached,
              "cached response is terminal");
        CloseHandle(cached);
    }

    HANDLE first = Connect();
    Check(first != INVALID_HANDLE_VALUE, "new client connects after cached request");
    if (first != INVALID_HANDLE_VALUE) {
        auto req = Request('b');
        Check(Write(first, &req, sizeof(req)) && ReadStatus(first) == ct::RespSendData,
              "new request asks for data");
        char data = 'x';
        Check(Write(first, &data, 1), "data transfer completes");

        HANDLE second = Connect();
        Check(second != INVALID_HANDLE_VALUE, "second client connects");
        if (second != INVALID_HANDLE_VALUE) {
            Check(Write(second, &req, sizeof(req)) && ReadStatus(second) == ct::RespInflight,
                  "duplicate request joins in-flight render");
            Check(ReadStatus(second) == ct::RespDone, "duplicate receives completion");
            CloseHandle(second);
        }
        Check(ReadStatus(first) == ct::RespDone, "owner receives completion");
        CloseHandle(first);
    }

    server.Stop();
    ct::JobManager::Instance().Stop();
    if (!failures) puts("pipe protocol checks passed");
    return failures ? 1 : 0;
}
