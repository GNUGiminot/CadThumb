#pragma once
#include "common/FileType.h"

#include <windows.h>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ct {

// Render queue of the host process. Deduplicates requests by cache key, limits parallelism
// (MaxParallelRenders, re-read on the fly) and serves Explorer requests before bulk pre-generation.
class JobManager {
public:
    struct Job {
        enum State { Receiving, Queued, Running, Done, Failed };
        std::string key;
        FileType type = FileType::Unknown;
        int bucket = 256;
        std::wstring name;
        std::wstring input;
        bool ownsInput = false; // temp copy received over the pipe -> delete after rendering
        std::atomic<State> state{Receiving};
        std::wstring error;
    };
    using JobPtr = std::shared_ptr<Job>;

    enum class ReserveResult { New, Existing, Cached, KnownFailure };

    struct Stats {
        int queued = 0, running = 0, done = 0, failed = 0;
    };

    static JobManager& Instance();

    void Start();
    void Stop();

    ReserveResult Reserve(const std::string& key, FileType type, int bucket, const std::wstring& name, JobPtr& job);
    void Enqueue(const JobPtr& job, const std::wstring& input, bool ownsInput, bool highPriority);
    void Abandon(const JobPtr& job);
    // true when the job finished (Done or Failed) within the timeout
    bool Wait(const JobPtr& job, DWORD ms);
    Stats GetStats();

private:
    void WorkerLoop();

    std::mutex mtx_;
    std::condition_variable queueCv_, doneCv_;
    std::deque<JobPtr> queue_;
    std::map<std::string, JobPtr> active_;
    std::vector<std::thread> workers_;
    int running_ = 0, done_ = 0, failed_ = 0;
    bool stop_ = false;
};

} // namespace ct
