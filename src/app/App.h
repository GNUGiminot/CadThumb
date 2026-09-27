#pragma once
#include "common/FileType.h"
#include "common/Settings.h"

#include <windows.h>
#include <atomic>
#include <string>
#include <vector>

namespace ct {

// ---- RenderRunner.cpp
// Runs `CadThumb.exe --render` in a child process inside a job object (memory limit, hard timeout,
// low priority) and stores the result in the cache under `key`.
bool RenderToCache(const std::wstring& input, FileType type, int bucket, const std::string& key, const Settings& s,
                   std::wstring* error = nullptr, const std::wstring& displayName = {});
// Implementation of `--render` in the child process. Returns the process exit code.
int RenderCommand(const std::wstring& input, const std::wstring& output, FileType type, int size);

// ---- Prewarm.cpp
struct PrewarmState {
    std::atomic<int> total{0}, done{0}, rendered{0}, failed{0}, skipped{0};
    std::atomic<bool> running{false}, cancel{false};
    std::wstring folder;
};
void PrewarmFolder(const std::wstring& folder, PrewarmState& st); // uses the JobManager (host process)
int RefreshFiles(const std::vector<std::wstring>& paths);        // standalone (`--refresh`)
void UpdateExplorerThumbnail(const std::wstring& path);
std::vector<std::wstring> CollectModelFiles(const std::wstring& folder, bool recursive);

// ---- Tray.cpp
int RunHost(HINSTANCE instance);
bool StopRunningHost(DWORD waitMs);

// ---- viewer/Viewer.cpp
// Interactive 3D viewer window. snapshot: test mode — save the first rendered frame as PNG and exit.
int RunViewer(HINSTANCE instance, const std::wstring& path, const std::wstring& snapshot = {});
void LaunchViewer(const std::wstring& path); // starts `CadThumb.exe --view` as a separate process

// ---- Diagnose.cpp
// End-to-end check of registration, Explorer, the service and a real thumbnail request; writes
// CadThumb-diagnostics.txt to the desktop. interactive: show the report (and offer an Explorer restart).
int Diagnose(bool interactive);

// ---- Console.cpp
void AttachParentConsole();
void Out(const std::wstring& text);  // console if attached, otherwise buffered
void FlushOutAsMessageBox(const wchar_t* title, bool error);

std::wstring SelfExePath();

} // namespace ct
