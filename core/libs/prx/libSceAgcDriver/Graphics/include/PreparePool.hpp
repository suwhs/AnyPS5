#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PREPAREPOOL_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PREPAREPOOL_HPP

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace AgcDriver::Graphics {

class ShaderResources;

struct PreparedStageA {
    bool staging = false;
    std::shared_ptr<ShaderResources> resources;
    void Wait();
    std::mutex waitMutex;
    std::condition_variable waitDone;
    std::atomic<bool> done{false};
    std::exception_ptr failure;
};

std::size_t PrepareWorkers();
bool PreparePoolEnabled();

class PreparePool {
public:
    using Job = std::function<void()>;
    static PreparePool& Get();
    void Enqueue(Job job);
    std::size_t Busy();
    std::size_t Queued();
    std::size_t InFlight();
    void Drain();

private:
    PreparePool() = default;
    void RunWorker();
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable idle;
    std::deque<Job> jobs;
    std::vector<std::exception_ptr> failures;
    std::size_t workers = 0;
    std::size_t busy = 0;
    bool stopped = false;
};

}

#endif
