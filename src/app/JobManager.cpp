#include "app/JobManager.h"

#include "app/App.h"
#include "common/Cache.h"
#include "common/Log.h"
#include "common/Settings.h"

namespace ct {

static constexpr int kMaxWorkers = 16;

JobManager& JobManager::Instance() {
    static JobManager m;
    return m;
}

void JobManager::Start() {
    std::lock_guard<std::mutex> lock(mtx_);
    stop_ = false;
    for (int i = 0; i < kMaxWorkers; ++i) workers_.emplace_back(&JobManager::WorkerLoop, this);
}

void JobManager::Stop() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        stop_ = true;
    }
    queueCv_.notify_all();
    doneCv_.notify_all();
    for (auto& t : workers_) t.join();
    workers_.clear();
}

JobManager::ReserveResult JobManager::Reserve(const std::string& key, FileType type, int bucket,
                                              const std::wstring& name, JobPtr& job) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = active_.find(key);
    if (it != active_.end()) {
        job = it->second;
        return ReserveResult::Existing;
    }
    // re-check under the lock: a job for this key may have finished a moment ago
    switch (QueryCache(key, Settings::Get().failRetryHours)) {
    case CacheState::Ready: return ReserveResult::Cached;
    case CacheState::Failed: return ReserveResult::KnownFailure;
    default: break;
    }
    job = std::make_shared<Job>();
    job->key = key;
    job->type = type;
    job->bucket = bucket;
    job->name = name;
    active_[key] = job;
    return ReserveResult::New;
}

void JobManager::Enqueue(const JobPtr& job, const std::wstring& input, bool ownsInput, bool highPriority) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        job->input = input;
        job->ownsInput = ownsInput;
        job->state = Job::Queued;
        // Explorer requests: newest first (what the user is looking at right now).
        if (highPriority) queue_.push_front(job);
        else queue_.push_back(job);
    }
    queueCv_.notify_all();
}

void JobManager::Abandon(const JobPtr& job) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        job->state = Job::Failed;
        job->error = L"abandoned";
        auto it = active_.find(job->key);
        if (it != active_.end() && it->second == job) active_.erase(it);
    }
    doneCv_.notify_all();
}

bool JobManager::Wait(const JobPtr& job, DWORD ms) {
    std::unique_lock<std::mutex> lock(mtx_);
    return doneCv_.wait_for(lock, std::chrono::milliseconds(ms), [&] {
        return stop_ || job->state == Job::Done || job->state == Job::Failed;
    }) && (job->state == Job::Done || job->state == Job::Failed);
}

JobManager::Stats JobManager::GetStats() {
    std::lock_guard<std::mutex> lock(mtx_);
    return {int(queue_.size()), running_, done_, failed_};
}

void JobManager::WorkerLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        JobPtr job;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            // Parallelism limit is re-read periodically, so settings changes apply without restart.
            queueCv_.wait_for(lock, std::chrono::seconds(3), [&] {
                return stop_ || (!queue_.empty() && running_ < Settings::Get().maxParallel);
            });
            if (stop_) break;
            if (queue_.empty() || running_ >= Settings::Get().maxParallel) continue;
            job = queue_.front();
            queue_.pop_front();
            job->state = Job::Running;
            ++running_;
        }

        std::wstring error;
        bool ok = RenderToCache(job->input, job->type, job->bucket, job->key, Settings::Get(), &error, job->name);
        if (job->ownsInput) DeleteFileW(job->input.c_str());

        {
            std::lock_guard<std::mutex> lock(mtx_);
            --running_;
            job->state = ok ? Job::Done : Job::Failed;
            job->error = error;
            ok ? ++done_ : ++failed_;
            auto it = active_.find(job->key);
            if (it != active_.end() && it->second == job) active_.erase(it);
        }
        doneCv_.notify_all();
        queueCv_.notify_all();
    }
    CoUninitialize();
}

} // namespace ct
