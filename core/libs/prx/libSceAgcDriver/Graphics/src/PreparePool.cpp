#include "prx/libSceAgcDriver/Graphics/include/PreparePool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <thread>

namespace AgcDriver::Graphics {

namespace {

constexpr std::size_t PrepareWorkerLimit = 32;

std::size_t ParsePrepareWorkers() {
    const char* text = std::getenv("APS5_PREPARE_WORKERS");
    if (text == nullptr) return 0;
    const auto value = std::strtoull(text, nullptr, 10);
    return value == 0 ? 0 : static_cast<std::size_t>(std::min<unsigned long long>(value, PrepareWorkerLimit));
}

}

std::size_t PrepareWorkers() {
    static const std::size_t workers = ParsePrepareWorkers();
    return workers;
}

bool PreparePoolEnabled() {
    return PrepareWorkers() != 0;
}

void PreparedStageA::Wait() {
    std::unique_lock lock(waitMutex);
    waitDone.wait(lock, [&] { return done.load(std::memory_order_acquire); });
}

PreparePool& PreparePool::Get() {
    static PreparePool* pool = new PreparePool();
    return *pool;
}

void PreparePool::Enqueue(Job job) {
    bool spawn = false;
    {
        std::lock_guard lock(mutex);
        if (stopped) throw std::runtime_error("the prepare pool is stopped");
        jobs.push_back(std::move(job));
        spawn = workers < PrepareWorkers() && workers < jobs.size() + busy;
        if (spawn) ++workers;
    }
    if (spawn) std::thread(&PreparePool::RunWorker, this).detach();
    wake.notify_one();
}

std::size_t PreparePool::Busy() {
    std::lock_guard lock(mutex);
    return busy;
}

std::size_t PreparePool::Queued() {
    std::lock_guard lock(mutex);
    return jobs.size();
}

std::size_t PreparePool::InFlight() {
    std::lock_guard lock(mutex);
    return busy + jobs.size();
}

void PreparePool::Drain() {
    std::exception_ptr failure;
    {
        std::unique_lock lock(mutex);
        idle.wait(lock, [&] { return jobs.empty() && busy == 0; });
        if (!failures.empty()) {
            failure = failures.front();
            failures.clear();
        }
    }
    if (failure != nullptr) std::rethrow_exception(failure);
}

void PreparePool::RunWorker() {
    while (true) {
        Job job;
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return stopped || !jobs.empty(); });
            if (jobs.empty()) return;
            job = std::move(jobs.front());
            jobs.pop_front();
            ++busy;
        }
        try {
            job();
        } catch (...) {
            std::lock_guard lock(mutex);
            failures.push_back(std::current_exception());
        }
        {
            std::lock_guard lock(mutex);
            --busy;
            idle.notify_all();
        }
    }
}

}
