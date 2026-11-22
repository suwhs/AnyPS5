#include <cerrno>
#include <cstdint>
#include <ctime>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "SceTypes.hpp"

namespace {

constexpr int SceKernelErrorEINVAL = -2147418107;
constexpr int SceKernelErrorETIMEDOUT = -2147418106;

int Futex(std::uint32_t* address, int operation, std::uint32_t value, const struct timespec* timeout) {
    return static_cast<int>(::syscall(SYS_futex, address, operation, value, timeout, nullptr, 0));
}

}

extern "C" {

// The guest uses this pair as a lightweight futex: Wait sleeps while the word
// still holds the expected value, Wake releases the waiters. Returning
// immediately (as a stub would) breaks the title's synchronisation, so the port
// must really block and wake.
int APS5_VABI sceKernelSyncOnAddressWait(void* address, std::uint32_t expected, std::uint64_t timeout) {
    if (address == nullptr) return SceKernelErrorEINVAL;
    struct timespec spec{};
    const struct timespec* timeoutSpec = nullptr;
    if (timeout != 0) {
        spec.tv_sec = static_cast<time_t>(timeout / 1000000ull);
        spec.tv_nsec = static_cast<long>((timeout % 1000000ull) * 1000ull);
        timeoutSpec = &spec;
    }
    const int result = Futex(static_cast<std::uint32_t*>(address), FUTEX_WAIT_PRIVATE, expected, timeoutSpec);
    if (result == 0) return 0;
    if (errno == EAGAIN) return 0;      // the word changed before sleeping
    if (errno == ETIMEDOUT) return SceKernelErrorETIMEDOUT;
    if (errno == EINTR) return 0;
    return SceKernelErrorEINVAL;
}

int APS5_VABI sceKernelSyncOnAddressWake(void* address, int count) {
    if (address == nullptr) return SceKernelErrorEINVAL;
    const int waiters = count > 0 ? count : 1;
    const int result = Futex(static_cast<std::uint32_t*>(address), FUTEX_WAKE_PRIVATE, static_cast<std::uint32_t>(waiters), nullptr);
    if (result < 0) return SceKernelErrorEINVAL;
    return 0;
}

}
