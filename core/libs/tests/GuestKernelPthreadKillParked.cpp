#include "prx/libkernel/include/Api.hpp"
#include "prx/libkernel/Pthread/include/Cond.hpp"
#include "prx/libkernel/Pthread/include/Mutex.hpp"
#include "SceTypes.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

static constexpr int SceOk = 0;
static constexpr int GuestSignalSimple = 30;
static constexpr int GuestSignalSiginfo = 16;
static constexpr int SaSiginfo = 0x40;

static void Require(bool value, const char* what) {
    if (value) return;
    std::fprintf(stderr, "pthread_kill on a parked thread: %s\n", what);
    std::abort();
}

struct Parked {
    PthreadMutex mutex = nullptr;
    PthreadCond cond = nullptr;
    std::atomic<bool> waiting{false};
    std::atomic<bool> ran{false};
    std::atomic<std::int64_t> handlerThread{0};
    std::atomic<std::int64_t> parkedThread{0};
    int siginfoSeen = -1;
};

static Parked* gParked = nullptr;

static void APS5_VABI SimpleHandler(int signum) {
    Require(signum == GuestSignalSimple, "a simple handler saw another signal");
    gParked->handlerThread.store(scePthreadGetthreadid(), std::memory_order_release);
    gParked->ran.store(true, std::memory_order_release);
}

static void APS5_VABI SiginfoHandler(int signum, void*, void*) {
    Require(signum == GuestSignalSiginfo, "an SA_SIGINFO handler saw another signal");
    gParked->siginfoSeen = signum;
    gParked->handlerThread.store(scePthreadGetthreadid(), std::memory_order_release);
    gParked->ran.store(true, std::memory_order_release);
}

static void* APS5_VABI WaitForSignal(void* opaque) {
    auto& parked = *static_cast<Parked*>(opaque);
    Require(scePthreadMutexLock(&parked.mutex) == SceOk, "the parked thread cannot lock");
    parked.parkedThread.store(scePthreadGetthreadid(), std::memory_order_release);
    parked.waiting.store(true, std::memory_order_release);
    Require(scePthreadCondWait(&parked.cond, &parked.mutex) == SceOk, "the parked thread's wait failed");
    Require(scePthreadMutexUnlock(&parked.mutex) == SceOk, "the parked thread cannot unlock");
    return nullptr;
}

static void Round(Parked& parked, int guest, bool siginfo) {
    Require(scePthreadMutexInit(&parked.mutex, nullptr, nullptr) == SceOk, "mutex init");
    Require(scePthreadCondInit(&parked.cond, nullptr, nullptr) == SceOk, "cond init");
    gParked = &parked;
    if (siginfo) {
        GuestSigaction action{};
        action.handler = reinterpret_cast<std::uintptr_t>(&SiginfoHandler);
        action.flags = SaSiginfo;
        Require(sigaction_nid_postfix(guest, &action, nullptr) == 0, "sigaction");
    } else {
        Require(signal_nid_postfix(guest, SimpleHandler) != reinterpret_cast<GuestHandler>(-1), "signal");
    }
    Pthread thread = nullptr;
    Require(scePthreadCreate(&thread, nullptr, WaitForSignal, &parked, "parked") == SceOk, "thread create");
    while (!parked.waiting.load(std::memory_order_acquire)) std::this_thread::yield();
    Require(pthread_kill_nid_postfix(thread, guest) == 0, "pthread_kill");
    for (int waited = 0; waited < 2000 && !parked.ran.load(std::memory_order_acquire); ++waited)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Require(parked.ran.load(std::memory_order_acquire), "the handler did not run while the thread was parked");
    Require(parked.handlerThread.load(std::memory_order_acquire) == parked.parkedThread.load(std::memory_order_acquire), "the handler did not run on the target thread");
    Require(scePthreadMutexLock(&parked.mutex) == SceOk, "wake lock");
    Require(scePthreadCondSignal(&parked.cond) == SceOk, "wake signal");
    Require(scePthreadMutexUnlock(&parked.mutex) == SceOk, "wake unlock");
    Require(scePthreadJoin(thread, nullptr) == SceOk, "join");
    if (siginfo)
        Require(parked.siginfoSeen == guest, "the SA_SIGINFO handler saw no signal");
    else
        Require(signal_nid_postfix(guest, nullptr) != nullptr, "clear the simple handler");
    Require(scePthreadCondDestroy(&parked.cond) == SceOk, "cond destroy");
    Require(scePthreadMutexDestroy(&parked.mutex) == SceOk, "mutex destroy");
    gParked = nullptr;
}

int main() {
    Parked simple;
    Round(simple, GuestSignalSimple, false);
    Parked siginfo;
    Round(siginfo, GuestSignalSiginfo, true);
    std::printf("pthread_kill runs a parked thread's handler on the target thread\n");
    return 0;
}
