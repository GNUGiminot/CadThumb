#pragma once
#include <cstdint>

namespace ct {

// Named-pipe protocol between the shell extension (client, runs inside Explorer's thumbnail
// surrogate) and CadThumb.exe --host (server).
//
//   client -> PipeRequest
//   server -> PipeResponse{Cached | Failed | Inflight | SendData | Busy}
//   if SendData: client -> dataSize raw bytes of the file
//   if Inflight/SendData: server -> PipeResponse{Done | Failed} when the render finishes
//
// If the client gives up waiting, it simply disconnects; the render continues and lands in the cache.

inline constexpr uint32_t kPipeMagic = 0x31485443; // "CTH1"
inline constexpr uint32_t kPipeVersion = 1;

enum PipeRequestKind : uint32_t { ReqRender = 1, ReqPing = 2 };

enum PipeStatus : uint32_t {
    RespCached = 0,
    RespSendData = 1,
    RespInflight = 2,
    RespFailed = 3,
    RespBusy = 4,
    RespDone = 5,
    RespPong = 6,
    RespBadRequest = 7,
};

#pragma pack(push, 1)
struct PipeRequest {
    uint32_t magic = kPipeMagic;
    uint32_t version = kPipeVersion;
    uint32_t kind = ReqRender;
    uint32_t fileType = 0;
    uint32_t bucket = 256;
    uint64_t dataSize = 0;
    char key[40] = {};
    wchar_t name[260] = {}; // file name for logs only
};

struct PipeResponse {
    uint32_t magic = kPipeMagic;
    uint32_t status = RespFailed;
};
#pragma pack(pop)

} // namespace ct
