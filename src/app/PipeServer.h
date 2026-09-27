#pragma once
#include <windows.h>
#include <atomic>
#include <thread>

namespace ct {

class PipeServer {
public:
    bool Start();
    void Stop();

private:
    void AcceptLoop();

    std::thread thread_;
    std::atomic<bool> stop_{false};
    SECURITY_ATTRIBUTES sa_{};
    PSECURITY_DESCRIPTOR sd_ = nullptr;
};

} // namespace ct
