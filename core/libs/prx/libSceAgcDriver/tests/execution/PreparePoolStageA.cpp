#include "prx/libSceAgcDriver/Graphics/include/PreparePool.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using AgcDriver::Graphics::PreparePool;
using AgcDriver::Graphics::PreparePoolEnabled;
using AgcDriver::Graphics::PrepareWorkers;
using AgcDriver::Graphics::PreparedStageA;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void EveryJobRunsAndConcurrencyStaysAtTheCap() {
    auto& pool = PreparePool::Get();
    const std::size_t workers = PrepareWorkers();
    Require(workers != 0, "the prepare pool ran with no workers configured");
    std::atomic<int> ran{0};
    std::atomic<int> inside{0};
    std::atomic<int> peak{0};
    std::vector<std::shared_ptr<std::atomic<bool>>> done(8 * workers);
    for (auto& flag : done) flag = std::make_shared<std::atomic<bool>>(false);
    for (auto& flag : done) {
        pool.Enqueue([&ran, &inside, &peak, flag] {
            const int now = ++inside;
            int peakSeen = peak.load();
            while (now > peakSeen && !peak.compare_exchange_weak(peakSeen, now)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            --inside;
            ++ran;
            flag->store(true);
        });
    }
    for (auto& flag : done) {
        const auto start = std::chrono::steady_clock::now();
        while (!flag->load() && std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) std::this_thread::yield();
        Require(flag->load(), "a prepare pool job never ran");
    }
    Require(ran.load() == static_cast<int>(done.size()), "the prepare pool ran a different number of jobs than enqueued");
    Require(peak.load() <= static_cast<int>(workers), "prepare pool concurrency exceeded the configured worker cap");
    pool.Drain();
    Require(pool.InFlight() == 0, "the prepare pool reported work after every job completed");
}

void SlotCarriesWaitAndFailure() {
    auto& pool = PreparePool::Get();
    {
        auto slot = std::make_shared<PreparedStageA>();
        slot->staging = true;
        pool.Enqueue([slot] {
            {
                std::lock_guard lock(slot->waitMutex);
                slot->done.store(true);
            }
            slot->waitDone.notify_all();
        });
        slot->Wait();
        Require(slot->done.load() && slot->resources == nullptr && slot->failure == nullptr, "an empty prepare slot resolved wrong");
    }
    {
        auto slot = std::make_shared<PreparedStageA>();
        pool.Enqueue([slot] {
            try {
                throw std::runtime_error("stage A failure");
            } catch (...) {
                slot->failure = std::current_exception();
            }
            {
                std::lock_guard lock(slot->waitMutex);
                slot->done.store(true);
            }
            slot->waitDone.notify_all();
        });
        slot->Wait();
        Require(slot->failure != nullptr, "a failed prepare slot reported no failure");
        bool rethrown = false;
        try {
            std::rethrow_exception(slot->failure);
        } catch (const std::runtime_error&) {
            rethrown = true;
        }
        Require(rethrown, "a prepare slot's failure did not rethrow");
    }
}

void HeadroomGateBoundsInFlight() {
    auto& pool = PreparePool::Get();
    const std::size_t workers = PrepareWorkers();
    std::atomic<bool> release{false};
    std::vector<std::shared_ptr<std::atomic<bool>>> queued(workers + 3);
    for (auto& flag : queued) flag = std::make_shared<std::atomic<bool>>(false);
    for (auto& flag : queued) {
        pool.Enqueue([&release, flag] {
            while (!release.load()) std::this_thread::yield();
            flag->store(true);
        });
    }
    const auto start = std::chrono::steady_clock::now();
    while (pool.Busy() < workers && std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) std::this_thread::yield();
    Require(pool.Busy() <= workers, "prepare pool concurrency exceeded the configured cap");
    Require(pool.InFlight() == queued.size(), "the prepare pool lost jobs between the queue and the running workers");
    release.store(true);
    for (auto& flag : queued) {
        const auto started = std::chrono::steady_clock::now();
        while (!flag->load() && std::chrono::steady_clock::now() - started < std::chrono::seconds(10)) std::this_thread::yield();
        Require(flag->load(), "a released prepare pool job never ran");
    }
    pool.Drain();
}

}

int main() {
#ifdef _WIN32
    _putenv_s("APS5_PREPARE_WORKERS", "2");
#else
    setenv("APS5_PREPARE_WORKERS", "2", 1);
#endif
    try {
        Require(PreparePoolEnabled(), "the prepare pool did not enable with APS5_PREPARE_WORKERS set");
        EveryJobRunsAndConcurrencyStaysAtTheCap();
        SlotCarriesWaitAndFailure();
        HeadroomGateBoundsInFlight();
        std::cout << "prepare pool stage A tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
