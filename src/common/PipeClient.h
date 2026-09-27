#pragma once
#include "common/Protocol.h"

#include <objidl.h>
#include <windows.h>
#include <string>

namespace ct {

enum class RenderRequestResult { Done, Failed, Timeout, NoHost };

// Sends the file (from `data`) to the host and waits up to `waitMs` for the render.
// hostExe: path to CadThumb.exe, started automatically if the host is not running.
RenderRequestResult RequestRender(const PipeRequest& req, IStream* data, DWORD waitMs, const std::wstring& hostExe);

bool IsHostRunning();
bool StartHost(const std::wstring& hostExe, DWORD waitMs);

} // namespace ct
