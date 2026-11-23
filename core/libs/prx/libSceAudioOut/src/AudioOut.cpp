#include "prx/libSceAudioOut/include/AudioOut.hpp"
#include "prx/libSceAudioOut/include/OutputDevice.hpp"
#include <algorithm>
#include <cstdlib>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include <prx/libc/include/General.hpp>

#include "SDL.h"
#include "SceTypes.hpp"
#include "prx/libc/include/PreciseWait.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include "prx/libkernel/Time/include/Time.hpp"
#include "prx/libSceAudioOut/include/AudioOutSdl.hpp"
#include "prx/libSceAudioOut/include/HostGain.hpp"
#include "prx/libScePad/include/HostVolumeControl.hpp"
#include "AudioOutShutdown.hpp"
#include "AudioOut2PadMix.hpp"
#include "prx/libScePad/include/PadState.hpp"
#include "prx/libScePad/include/PadRumbleIntensity.hpp"

static constexpr int PORT_TYPE_MAIN = 0;
static constexpr int PORT_TYPE_BGM = 1;
static constexpr int PORT_TYPE_VOICE = 2;
static constexpr int PORT_TYPE_PERSONAL = 3;
static constexpr int PORT_TYPE_PADSPK = 4;
static constexpr int PORT_TYPE_EX = 6;
static constexpr int PORT_TYPE_SYS = 7;
static constexpr int PORT_TYPE_VIBRATION = 10;
static constexpr int PORT_TYPE_AUDIO3D = 126;
static constexpr int PORT_TYPE_AUX = 127;

static constexpr int PORTS_MAX = 32;
static constexpr int DEFAULT_VOLUME = 32768;
static constexpr int DEFAULT_PADSPK_MIX_LEVEL = 11626;
static constexpr std::uint32_t FORMAT_MASK = 0xFFu;
static constexpr std::uint64_t TARGET_LATENCY_US = 40000;
static constexpr std::uint64_t DRAIN_TIMEOUT_US = 200000;
static constexpr std::uint64_t DRAIN_SLEEP_US = 1000;
static constexpr std::uint64_t DEVICE_RETRY_US = 2000000;
static constexpr int MONITOR_TYPE_HDMI = 1;
static constexpr std::uint32_t HDMI_MONITOR_INFO_SIZE = 0x180;
static constexpr std::uint32_t EX_MONITOR_INFO_SIZE = 0x400;
static constexpr std::uint32_t EX_OPEN_MODE_MAX = 10;
static constexpr std::uint32_t EX_OPEN_MODES = 0x61F;
static constexpr std::uint32_t EX_CONFIGURE_MODE_DEFAULT = 0xFF;
static constexpr std::uint32_t EX_CONFIGURE_TARGET_DEFAULT = 0xFF;
static constexpr int ERROR_EX_INVALID_MODE = -2144993259;
static constexpr int ERROR_EX_INVALID_CONTROL = -2144960503;
static constexpr std::uint32_t SYS_OPEN_MODE_TRUEHD = 5;
static constexpr std::uint32_t SYS_CONFIGURE_TYPE_HDMI = 1;
static constexpr std::uint32_t SYS_CONFIGURE_TARGET_HDMI = 1;
static constexpr std::uint32_t SYS_PORT_SAMPLES = 1024;
static constexpr std::uint32_t SYS_PORT_FRAME_RATE = 192000;

enum class Format {
    Unknown,
    S16Mono,
    S16Stereo,
    S16_8Ch,
    F32Mono,
    F32Stereo,
    F32_8Ch,
    S16_8ChStd,
    F32_8ChStd,
};

static bool formatIsFloat(Format f) {
    return f == Format::F32Mono || f == Format::F32Stereo ||
           f == Format::F32_8Ch || f == Format::F32_8ChStd;
}

static bool formatIsStd(Format f) {
    return f == Format::S16_8ChStd || f == Format::F32_8ChStd;
}

static int channelsForFormat(Format f) {
    switch (f) {
        case Format::S16Mono:
        case Format::F32Mono:
            return 1;
        case Format::S16Stereo:
        case Format::F32Stereo:
            return 2;
        case Format::S16_8Ch:
        case Format::F32_8Ch:
        case Format::S16_8ChStd:
        case Format::F32_8ChStd:
            return 8;
        default:
            throw std::runtime_error("channelsForFormat: unknown format");
    }
}

static SDL_AudioFormat sdlFormat(Format f) {
    return formatIsFloat(f) ? AUDIO_F32SYS : AUDIO_S16SYS;
}

static std::uint32_t bytesPerSample(Format f) {
    return formatIsFloat(f) ? sizeof(float) : sizeof(std::int16_t);
}

static std::uint32_t checkedBufferSize(std::uint32_t frames, std::uint32_t channels, std::uint32_t bps) {
    const std::uint64_t size = static_cast<std::uint64_t>(frames) * channels * bps;
    if (size > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::runtime_error("checkedBufferSize: audio buffer size overflows");
    }
    return static_cast<std::uint32_t>(size);
}

struct Port {
    bool used = false;
    int type = 0;
    std::uint32_t samplesNum = 0;
    std::uint32_t freq = 0;
    Format format = Format::Unknown;
    int channels = 0;
    int volume[8] = {};
    int mixLevel = DEFAULT_VOLUME;
    std::uint64_t lastDataOutputTime = 0;
    std::uint64_t virtualQueueEnd = 0;
    std::uint64_t nextDeviceRetry = 0;
    AudioOut2PadLayout padLayout;
    SDL_AudioDeviceID device = 0;
    SDL_AudioSpec spec = {};
    HostGainRamp hostGain;
};

static std::mutex g_mutex;
static std::array<std::mutex, PORTS_MAX> g_outputMutexes;
static Port g_ports[PORTS_MAX];
static bool g_sdlInitialized = false;

static bool ensureSdlAudio() {
    if (g_sdlInitialized) {
        return true;
    }
    if (InitializeSdlAudio_nid_no_patch() < 0) {
        return false;
    }
    g_sdlInitialized = true;
    return true;
}

static const char* selectedOutputDevice(const char* requested) {
    if (requested == nullptr || *requested == '\0') return nullptr;
    const int count = SDL_GetNumAudioDevices(0);
    std::vector<std::string> names;
    for (int index = 0; index < count; ++index) {
        const char* name = SDL_GetAudioDeviceName(index, 0);
        if (name != nullptr) names.emplace_back(name);
    }
    if (FindOutputDevice(names, requested)) return requested;
    std::string listed;
    for (const auto& name : names) listed += "\n  " + name;
    APS5_LOG_OUT("AudioOut: ANYPS5_AUDIO_DEVICE '%s' is not an output device; using the default. Output devices:%s\n", requested, listed.c_str());
    return nullptr;
}

static bool openDevice(Port& port) {
    if (!ensureSdlAudio() || SdlAudioReportsNoOutputEndpoint(SDL_GetCurrentAudioDriver(), SDL_GetNumAudioDevices(0))) {
        return false;
    }
    SDL_AudioSpec desired{};
    desired.freq = static_cast<int>(port.freq);
    desired.format = sdlFormat(port.format);
    desired.channels = static_cast<Uint8>(port.channels);
    desired.samples = static_cast<Uint16>(port.samplesNum);
    desired.callback = nullptr;
    SDL_AudioSpec obtained{};
    port.device = SDL_OpenAudioDevice(selectedOutputDevice(std::getenv("ANYPS5_AUDIO_DEVICE")), 0, &desired, &obtained, 0);
    if (port.device == 0) {
        return false;
    }
    port.spec = obtained;
    SDL_PauseAudioDevice(port.device, 0);
    return true;
}

static bool openVibrationDevice(Port& port) {
    if (!ensureSdlAudio()) throw std::runtime_error(std::string("AudioOut: could not initialize controller audio: ") + SDL_GetError());
    const int count = SDL_GetNumAudioDevices(0);
    if (count < 0) throw std::runtime_error(std::string("AudioOut: could not enumerate controller sound cards: ") + SDL_GetError());
    for (int index = 0; index < count; ++index) {
        const char* name = SDL_GetAudioDeviceName(index, 0);
        if (!AudioOut2IsPadAudioDevice(name)) continue;
        const std::string controllerName(name);
        SDL_AudioSpec native{};
        if (SDL_GetAudioDeviceSpec(index, 0, &native) == 0 && native.channels != 0 && native.channels != AUDIO_OUT2_PAD_CHANNELS) continue;
        port.padLayout = AudioOut2PadLayoutForDriver(SDL_GetCurrentAudioDriver());
        SDL_AudioSpec desired{};
        desired.freq = static_cast<int>(port.freq);
        desired.format = AUDIO_F32SYS;
        desired.channels = static_cast<Uint8>(port.padLayout.channels);
        desired.samples = static_cast<Uint16>(port.samplesNum);
        port.device = SDL_OpenAudioDevice(controllerName.c_str(), 0, &desired, &port.spec, 0);
        if (port.device == 0) throw std::runtime_error(std::string("AudioOut: could not open the controller sound card: ") + SDL_GetError());
        SDL_PauseAudioDevice(port.device, 0);
        return true;
    }
    return false;
}

static void closeDevice(Port& port) {
    if (port.device != 0 && SDL_WasInit(SDL_INIT_AUDIO) != 0) {
        SDL_ClearQueuedAudio(port.device);
        SDL_CloseAudioDevice(port.device);
    }
    port.device = 0;
    port.spec = {};
}

static void reopenStoppedDevice(Port& port) {
    if (port.device == 0) {
        if (port.nextDeviceRetry == 0 || sceKernelGetProcessTime() < port.nextDeviceRetry) return;
    } else {
        if (SDL_GetAudioDeviceStatus(port.device) != SDL_AUDIO_STOPPED) return;
        closeDevice(port);
    }
    if (port.type == PORT_TYPE_VIBRATION) {
        try {
            openVibrationDevice(port);
        } catch (const std::runtime_error&) {
            port.device = 0;
            port.spec = {};
        }
    } else {
        openDevice(port);
        port.nextDeviceRetry = port.device == 0 ? sceKernelGetProcessTime() + DEVICE_RETRY_US : 0;
    }
}

static constexpr std::uint32_t STD_8CH_MAP[8] = {0, 1, 2, 3, 6, 7, 4, 5};

static const void* prepareBuffer(const Port& port, const void* data, std::vector<std::uint8_t>& buf) {
    const auto frames = port.samplesNum;
    const auto ch = static_cast<std::uint32_t>(port.channels);
    const auto bps = bytesPerSample(port.format);
    const auto size = checkedBufferSize(frames, ch, bps);

    bool volumeChanged = port.mixLevel != DEFAULT_VOLUME;
    for (std::uint32_t i = 0; i < ch; i++) {
        if (port.volume[i] != DEFAULT_VOLUME) {
            volumeChanged = true;
            break;
        }
    }

    if (!volumeChanged && !formatIsStd(port.format)) {
        return data;
    }

    buf.resize(size);
    const bool isStd = formatIsStd(port.format) && ch == 8;

    if (formatIsFloat(port.format)) {
        auto* dst = reinterpret_cast<float*>(buf.data());
        const auto* src = static_cast<const float*>(data);
        const float mixLevel = static_cast<float>(port.mixLevel) / static_cast<float>(DEFAULT_VOLUME);
        for (std::uint32_t fr = 0; fr < frames; fr++) {
            for (std::uint32_t c = 0; c < ch; c++) {
                const auto srcCh = isStd ? STD_8CH_MAP[c] : c;
                dst[fr * ch + c] = src[fr * ch + srcCh] *
                    (static_cast<float>(port.volume[c]) / static_cast<float>(DEFAULT_VOLUME)) * mixLevel;
            }
        }
    } else {
        auto* dst = reinterpret_cast<std::int16_t*>(buf.data());
        const auto* src = static_cast<const std::int16_t*>(data);
        for (std::uint32_t fr = 0; fr < frames; fr++) {
            for (std::uint32_t c = 0; c < ch; c++) {
                const auto srcCh = isStd ? STD_8CH_MAP[c] : c;
                std::int64_t s = static_cast<std::int64_t>(src[fr * ch + srcCh]) *
                    port.volume[c] * port.mixLevel / (static_cast<std::int64_t>(DEFAULT_VOLUME) * DEFAULT_VOLUME);
                s = std::clamp(s,
                    static_cast<std::int64_t>(std::numeric_limits<std::int16_t>::min()),
                    static_cast<std::int64_t>(std::numeric_limits<std::int16_t>::max()));
                dst[fr * ch + c] = static_cast<std::int16_t>(s);
            }
        }
    }
    return buf.data();
}

static std::vector<float> prepareVibration(const Port& port, const void* data) {
    checkedBufferSize(port.samplesNum, AUDIO_OUT2_PAD_CHANNELS, sizeof(float));
    std::vector<std::uint8_t> prepared;
    const auto* samples = prepareBuffer(port, data, prepared);
    std::vector<float> pad(static_cast<std::size_t>(port.samplesNum) * AUDIO_OUT2_PAD_CHANNELS, 0.0f);
    constexpr float unity[2] = {1.0f, 1.0f};
    for (std::uint32_t frame = 0; frame < port.samplesNum; ++frame) {
        float input[2]{};
        for (int channel = 0; channel < port.channels; ++channel) {
            const auto index = static_cast<std::size_t>(frame) * port.channels + channel;
            input[channel] = formatIsFloat(port.format) ? static_cast<const float*>(samples)[index] : static_cast<const std::int16_t*>(samples)[index] / 32768.0f;
        }
        AudioOut2AccumulatePadFrame(AudioOut2Route::PadVibration, input, port.channels, unity, pad.data() + static_cast<std::size_t>(frame) * AUDIO_OUT2_PAD_CHANNELS);
    }
    AudioOut2FinishPadMix(pad.data(), port.samplesNum);
    return pad;
}

enum class AudioOutApi {
    Open,
    Close,
    Output,
    Outputs,
    SetVolume,
    GetLastOutputTime,
    GetPortState,
    GetSystemState,
    SetMainOutput,
    SysOpen,
    SysClose,
    ExOpen,
    ExClose,
    Other,
    Count,
};

const char* AudioOutApiName(AudioOutApi api) {
    switch (api) {
        case AudioOutApi::Open: return "open";
        case AudioOutApi::Close: return "close";
        case AudioOutApi::Output: return "output";
        case AudioOutApi::Outputs: return "outputs";
        case AudioOutApi::SetVolume: return "setVolume";
        case AudioOutApi::GetLastOutputTime: return "getLastOutputTime";
        case AudioOutApi::GetPortState: return "getPortState";
        case AudioOutApi::GetSystemState: return "getSystemState";
        case AudioOutApi::SetMainOutput: return "setMainOutput";
        case AudioOutApi::SysOpen: return "sysOpen";
        case AudioOutApi::SysClose: return "sysClose";
        case AudioOutApi::ExOpen: return "exOpen";
        case AudioOutApi::ExClose: return "exClose";
        default: return "other";
    }
}

struct AudioOutTraceState {
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> virtualCalls{0};
    std::atomic<std::uint64_t> totalUs{0};
    std::atomic<std::uint64_t> longestUs{0};
    std::atomic<std::uint64_t> slowCalls{0};
    std::atomic<std::uint64_t> sleeps{0};
    std::atomic<std::uint64_t> sleepTotalUs{0};
    std::atomic<std::uint64_t> sleepLongestUs{0};
    std::atomic<std::uint64_t> lastReportUs{0};
    std::array<std::atomic<int>, 8> peakIn{};
    std::array<std::atomic<int>, 8> peakOut{};
    std::atomic<std::uint64_t> loudestIn{0};
    std::atomic<std::uint64_t> loudestOut{0};
    std::atomic<std::uint64_t> silentBuffers{0};
    std::atomic<std::uint64_t> loudBuffers{0};
    std::array<char, 65> pattern{};
    std::uint32_t patternCount = 0;
    const void* watchedBuffer = nullptr;
    std::uint64_t watchedChecksum = 0;
    std::uint64_t watchedAtUs = 0;
    std::atomic<std::uint64_t> lateFills{0};
    std::atomic<std::uint64_t> lateFillWorstUs{0};
    std::atomic<std::uint64_t> prefixOnlyBuffers{0};
    std::atomic<std::uint64_t> suffixOnlyBuffers{0};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(AudioOutApi::Count)> apiCalls{};
};


constexpr std::size_t peakChannels = 8;

static bool AudioOutTraceEnabled() {
    static const bool enabled = std::getenv("APS5_TRACE_AUDIOOUT") != nullptr;
    return enabled;
}

static AudioOutTraceState& AudioOutTrace() {
    static AudioOutTraceState state;
    return state;
}
static void CountAudioOutApi(AudioOutApi api) {
    if (!AudioOutTraceEnabled()) return;
    AudioOutTrace().apiCalls[static_cast<std::size_t>(api)].fetch_add(1, std::memory_order_relaxed);
}

static void RecordLongest(std::atomic<std::uint64_t>& target, std::uint64_t value) {
    std::uint64_t previous = target.load(std::memory_order_relaxed);
    while (previous < value && !target.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {
    }
}

static void RecordAudioOutSleep(std::uint64_t us) {
    if (!AudioOutTraceEnabled()) return;
    auto& state = AudioOutTrace();
    state.sleeps.fetch_add(1, std::memory_order_relaxed);
    state.sleepTotalUs.fetch_add(us, std::memory_order_relaxed);
    RecordLongest(state.sleepLongestUs, us);
}

static void RecordAudioOutCall(std::uint64_t startUs, bool virtualPort) {
    if (!AudioOutTraceEnabled()) return;
    auto& state = AudioOutTrace();
    const std::uint64_t now = sceKernelGetProcessTime();
    const std::uint64_t waited = now > startUs ? now - startUs : 0;
    state.calls.fetch_add(1, std::memory_order_relaxed);
    state.totalUs.fetch_add(waited, std::memory_order_relaxed);
    if (virtualPort) state.virtualCalls.fetch_add(1, std::memory_order_relaxed);
    if (waited >= 50000) state.slowCalls.fetch_add(1, std::memory_order_relaxed);
    RecordLongest(state.longestUs, waited);
    std::uint64_t last = state.lastReportUs.load(std::memory_order_relaxed);
    if (last == 0) {
        state.lastReportUs.store(now, std::memory_order_relaxed);
        return;
    }
    if (now - last < 5000000) return;
    if (!state.lastReportUs.compare_exchange_strong(last, now, std::memory_order_relaxed)) return;
    const std::uint64_t calls = state.calls.exchange(0, std::memory_order_relaxed);
    const std::uint64_t virtualCalls = state.virtualCalls.exchange(0, std::memory_order_relaxed);
    const std::uint64_t totalUs = state.totalUs.exchange(0, std::memory_order_relaxed);
    const std::uint64_t longestUs = state.longestUs.exchange(0, std::memory_order_relaxed);
    const std::uint64_t slowCalls = state.slowCalls.exchange(0, std::memory_order_relaxed);
    const std::uint64_t sleeps = state.sleeps.exchange(0, std::memory_order_relaxed);
    const std::uint64_t sleepTotalUs = state.sleepTotalUs.exchange(0, std::memory_order_relaxed);
    const std::uint64_t sleepLongestUs = state.sleepLongestUs.exchange(0, std::memory_order_relaxed);
    for (std::size_t index = 0; index < static_cast<std::size_t>(AudioOutApi::Count); ++index) {
        const auto calls = AudioOutTrace().apiCalls[index].exchange(0, std::memory_order_relaxed);
        if (calls != 0) std::fprintf(stderr, "[audioout] api %s x%llu\n", AudioOutApiName(static_cast<AudioOutApi>(index)), static_cast<unsigned long long>(calls));
    }
    std::fprintf(stderr, "[audioout] levels in");
    for (std::size_t channel = 0; channel < peakChannels; ++channel) std::fprintf(stderr, " %d", state.peakIn[channel].exchange(0, std::memory_order_relaxed));
    std::fprintf(stderr, " (loudest %llu); out", static_cast<unsigned long long>(state.loudestIn.exchange(0, std::memory_order_relaxed)));
    for (std::size_t channel = 0; channel < peakChannels; ++channel) std::fprintf(stderr, " %d", state.peakOut[channel].exchange(0, std::memory_order_relaxed));
    std::fprintf(stderr, " (loudest %llu); buffers silent %llu / loud %llu\n", static_cast<unsigned long long>(state.loudestOut.exchange(0, std::memory_order_relaxed)),
        static_cast<unsigned long long>(state.silentBuffers.exchange(0, std::memory_order_relaxed)),
        static_cast<unsigned long long>(state.loudBuffers.exchange(0, std::memory_order_relaxed)));
    state.pattern[state.patternCount] = 0;
    std::fprintf(stderr, "[audioout] halves: prefix-only %llu, suffix-only %llu\n",
        static_cast<unsigned long long>(state.prefixOnlyBuffers.exchange(0, std::memory_order_relaxed)),
        static_cast<unsigned long long>(state.suffixOnlyBuffers.exchange(0, std::memory_order_relaxed)));
    std::fprintf(stderr, "[audioout] pattern %s; late fills %llu (worst %llu us)\n", state.pattern.data(),
        static_cast<unsigned long long>(state.lateFills.exchange(0, std::memory_order_relaxed)),
        static_cast<unsigned long long>(state.lateFillWorstUs.exchange(0, std::memory_order_relaxed)));
    state.patternCount = 0;
    std::fprintf(stderr,
        "[audioout] %llu calls in %.1f s (virtual %llu): inside %.3f s, longest %.1f ms, over 50 ms %llu; our sleeps %llu, longest asked %.1f ms, total %.3f s\n",
        static_cast<unsigned long long>(calls), static_cast<double>(now - last) / 1e6,
        static_cast<unsigned long long>(virtualCalls), static_cast<double>(totalUs) / 1e6,
        static_cast<double>(longestUs) / 1e3, static_cast<unsigned long long>(slowCalls),
        static_cast<unsigned long long>(sleeps), static_cast<double>(sleepLongestUs) / 1e3,
        static_cast<double>(sleepTotalUs) / 1e6);
}

static void RecordAudioLevels(const void* data, const Port& port, bool outgoing) {
    if (!AudioOutTraceEnabled() || data == nullptr || port.channels == 0 || port.samplesNum == 0) return;
    auto& state = AudioOutTrace();
    const auto channels = std::min<std::uint32_t>(port.channels, peakChannels);
    const bool isFloat = formatIsFloat(port.format);
    std::array<int, peakChannels> peak{};
    int loudest = 0;
    for (std::uint32_t frame = 0; frame < port.samplesNum; ++frame) {
        for (std::uint32_t channel = 0; channel < channels; ++channel) {
            const auto index = static_cast<std::size_t>(frame) * port.channels + channel;
            const int value = isFloat
                ? static_cast<int>(std::fabs(static_cast<const float*>(data)[index]) * 32768.0f)
                : std::abs(static_cast<int>(static_cast<const std::int16_t*>(data)[index]));
            if (value > peak[channel]) peak[channel] = value;
            if (value > loudest) loudest = value;
        }
    }
    auto& target = outgoing ? state.peakOut : state.peakIn;
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
        int previous = target[channel].load(std::memory_order_relaxed);
        while (previous < peak[channel] && !target[channel].compare_exchange_weak(previous, peak[channel], std::memory_order_relaxed)) {
        }
    }
    if (!outgoing && port.samplesNum >= 4) {
        const bool isFloat = formatIsFloat(port.format);
        const auto half = port.samplesNum / 2;
        bool firstLoud = false;
        bool secondLoud = false;
        for (std::uint32_t frame = 0; frame < port.samplesNum; ++frame) {
            for (std::uint32_t channel = 0; channel < channels; ++channel) {
                const auto index = static_cast<std::size_t>(frame) * port.channels + channel;
                const int value = isFloat
                    ? (std::fabs(static_cast<const float*>(data)[index]) > 0.0f ? 1 : 0)
                    : (static_cast<const std::int16_t*>(data)[index] != 0 ? 1 : 0);
                if (value == 0) continue;
                if (frame < half) firstLoud = true;
                else secondLoud = true;
            }
        }
        if (firstLoud && !secondLoud) state.prefixOnlyBuffers.fetch_add(1, std::memory_order_relaxed);
        if (!firstLoud && secondLoud) state.suffixOnlyBuffers.fetch_add(1, std::memory_order_relaxed);
    }
    std::uint64_t checksum = 0;
    for (std::uint32_t frame = 0; frame < port.samplesNum; frame += 16) {
        for (std::uint32_t channel = 0; channel < channels; ++channel) {
            const auto index = static_cast<std::size_t>(frame) * port.channels + channel;
            const auto* bytes = static_cast<const std::uint8_t*>(data) + index * bytesPerSample(port.format);
            checksum = checksum * 1000003ULL + bytes[0] + (bytes[1] << 8);
        }
    }
    if (!outgoing) {
        (loudest == 0 ? state.silentBuffers : state.loudBuffers).fetch_add(1, std::memory_order_relaxed);
        if (state.patternCount < state.pattern.size() - 1) state.pattern[state.patternCount++] = loudest == 0 ? 'S' : 'L';
        if (state.watchedBuffer == data && checksum != state.watchedChecksum) {
            const auto now = sceKernelGetProcessTime();
            const auto late = now > state.watchedAtUs ? now - state.watchedAtUs : 0;
            state.lateFills.fetch_add(1, std::memory_order_relaxed);
            RecordLongest(state.lateFillWorstUs, late);
        }
        state.watchedBuffer = data;
        state.watchedChecksum = checksum;
        state.watchedAtUs = sceKernelGetProcessTime();
    }
    auto& loudestTarget = outgoing ? state.loudestOut : state.loudestIn;
    std::uint64_t previousLoudest = loudestTarget.load(std::memory_order_relaxed);
    while (previousLoudest < static_cast<std::uint64_t>(loudest) && !loudestTarget.compare_exchange_weak(previousLoudest, static_cast<std::uint64_t>(loudest), std::memory_order_relaxed)) {
    }
}

static void DumpQueuedAudio(const void* data, std::uint32_t bytes) {
    static const char* path = std::getenv("APS5_AUDIO_DUMP");
    if (path == nullptr) return;
    static FILE* file = std::fopen(path, "wb");
    static std::uint64_t written = 0;
    constexpr std::uint64_t limit = 96ULL * 1024ULL * 1024ULL;
    if (file == nullptr || written >= limit) return;
    const auto count = std::min<std::uint64_t>(bytes, limit - written);
    written += std::fwrite(data, 1, static_cast<std::size_t>(count), file);
    std::fflush(file);
}

static void sleepUs(std::unique_lock<std::mutex>& lock, std::uint64_t us) {
    RecordAudioOutSleep(us);
    lock.unlock();
    PreciseSleepUs(us);
    lock.lock();
}

static std::uint64_t AudioQueueTargetUs() {
    const char* text = std::getenv("APS5_AUDIO_QUEUE_MS");
    if (text == nullptr) return TARGET_LATENCY_US;
    const long value = std::strtol(text, nullptr, 10);
    if (value <= 0 || value > 5000) return TARGET_LATENCY_US;
    return static_cast<std::uint64_t>(value) * 1000ULL;
}

static void queueAudio(Port& port, const void* data, std::unique_lock<std::mutex>& lock) {
    if (data == nullptr) {
        const std::uint64_t waitStart = sceKernelGetProcessTime();
        while (SDL_GetQueuedAudioSize(port.device) > 0) {
            if (sceKernelGetProcessTime() - waitStart > DRAIN_TIMEOUT_US) {
                SDL_ClearQueuedAudio(port.device);
                break;
            }
            sleepUs(lock, DRAIN_SLEEP_US);
        }
        return;
    }

    std::vector<std::uint8_t> prepareBuf;
    std::vector<float> padFrames;
    const void* queueData = nullptr;
    std::uint32_t queueSize = 0;
    if (port.type == PORT_TYPE_VIBRATION) {
        queueSize = checkedBufferSize(port.samplesNum, port.padLayout.channels, sizeof(float));
        const auto pad = prepareVibration(port, data);
        padFrames.resize(static_cast<std::size_t>(port.samplesNum) * port.padLayout.channels);
        AudioOut2WritePadFrames(pad.data(), port.padLayout, padFrames.data(), port.samplesNum, Pad::RumblePercent());
        queueData = padFrames.data();
    } else {
        queueData = prepareBuffer(port, data, prepareBuf);
        RecordAudioLevels(data, port, false);
        queueData = HostGainFilter(port.hostGain, HostVolumeGain_nid_no_patch(), queueData, formatIsFloat(port.format), port.samplesNum, static_cast<unsigned>(port.channels), prepareBuf);
        RecordAudioLevels(queueData, port, true);
        queueSize = checkedBufferSize(port.samplesNum, static_cast<std::uint32_t>(port.channels), bytesPerSample(port.format));
    }

    const std::uint64_t bufferUs = port.freq != 0
        ? (1000000ULL * port.samplesNum) / port.freq
        : 0;
    const auto targetUs = AudioQueueTargetUs();
    const std::uint32_t buffers = bufferUs != 0
        ? static_cast<std::uint32_t>((targetUs + bufferUs - 1) / bufferUs)
        : 2u;
    const std::uint32_t minQueued = queueSize * std::clamp(buffers, 2u, 8192u);
    const std::uint64_t drainTimeoutUs = targetUs == TARGET_LATENCY_US ? DRAIN_TIMEOUT_US : DRAIN_TIMEOUT_US + targetUs;
    const std::uint64_t waitStart = sceKernelGetProcessTime();

    while (SDL_GetQueuedAudioSize(port.device) > minQueued) {
        if (sceKernelGetProcessTime() - waitStart > drainTimeoutUs) {
            SDL_ClearQueuedAudio(port.device);
            break;
        }
        sleepUs(lock, DRAIN_SLEEP_US);
    }

    DumpQueuedAudio(queueData, queueSize);
    if (SDL_QueueAudio(port.device, queueData, queueSize) < 0) {
        throw std::runtime_error(std::string("SDL_QueueAudio: ") + SDL_GetError());
    }
    // APS5_LOG_OUT("device=%u type=%d bytes=%u queued=%u", port.device, port.type, queueSize, SDL_GetQueuedAudioSize(port.device));
}

static void paceVirtualPort(Port& port, bool hasData, std::unique_lock<std::mutex>& lock) {
    const std::uint64_t now = sceKernelGetProcessTime();
    const auto queued = static_cast<std::int64_t>(port.virtualQueueEnd - now);
    if (queued <= 0) port.virtualQueueEnd = now;
    if (!hasData) {
        if (queued > 0) sleepUs(lock, static_cast<std::uint64_t>(queued));
        return;
    }
    if (queued > static_cast<std::int64_t>(TARGET_LATENCY_US)) sleepUs(lock, static_cast<std::uint64_t>(queued) - TARGET_LATENCY_US);
    port.virtualQueueEnd += (1000000ULL * port.samplesNum) / port.freq;
}

static void outputVirtualPort(Port& port, const void* data, std::unique_lock<std::mutex>& lock) {
    if (data != nullptr) {
        checkedBufferSize(port.samplesNum, static_cast<std::uint32_t>(port.channels), bytesPerSample(port.format));
    }
    const auto pad = port.type == PORT_TYPE_VIBRATION && data != nullptr ? prepareVibration(port, data) : std::vector<float>{};
    paceVirtualPort(port, data != nullptr, lock);
    const auto source = static_cast<int>(&port - g_ports) + 1;
    if (port.type == PORT_TYPE_VIBRATION && data == nullptr) PadClearLegacyAudioHaptics_nid_postfix(source);
    if (!pad.empty() && PadHapticStreamOpen_nid_postfix()) {
        PadSubmitHapticFrames_nid_postfix(pad.data() + AUDIO_OUT2_PAD_VIBRATION_LEFT, pad.data() + AUDIO_OUT2_PAD_VIBRATION_RIGHT, AUDIO_OUT2_PAD_CHANNELS, port.samplesNum, port.freq);
    } else if (!pad.empty()) {
        constexpr std::uint32_t grainFrames = 256;
        const auto begin = port.virtualQueueEnd - (1000000ULL * port.samplesNum) / port.freq;
        for (std::uint32_t frame = 0; frame < port.samplesNum;) {
            const auto count = std::min(grainFrames, port.samplesNum - frame);
            const auto start = begin + (1000000ULL * frame) / port.freq;
            const auto end = begin + (1000000ULL * (frame + count)) / port.freq;
            const auto rumble = AudioOut2RumbleFromPadFrames(pad.data() + static_cast<std::size_t>(frame) * AUDIO_OUT2_PAD_CHANNELS, count);
            PadQueueLegacyAudioHaptics_nid_postfix(source, start, end, rumble[0], rumble[1]);
            frame += count;
        }
    }
}

static bool portTypeValid(int type) {
    return (type >= PORT_TYPE_MAIN && type <= PORT_TYPE_PADSPK) ||
           type == PORT_TYPE_VIBRATION ||
           type == PORT_TYPE_AUDIO3D ||
           type == PORT_TYPE_AUX;
}

static Port* getPort(int handle) {
    if (handle <= 0 || handle > PORTS_MAX) return nullptr;
    const int idx = handle - 1;
    if (!g_ports[idx].used) {
        return nullptr;
    }
    return &g_ports[idx];
}

static int allocatePort(int type, std::uint32_t len, std::uint32_t freq, Format format) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int i = 0; i < PORTS_MAX; i++) {
        if (!g_ports[i].used) {
            Port& port = g_ports[i];
            port.used = true;
            port.type = type;
            port.samplesNum = len;
            port.freq = freq;
            port.format = format;
            port.channels = channelsForFormat(format);
            port.lastDataOutputTime = 0;
            port.hostGain = HostGainRamp(freq / 100);
            port.virtualQueueEnd = sceKernelGetProcessTime();
            port.mixLevel = type == PORT_TYPE_PADSPK ? DEFAULT_PADSPK_MIX_LEVEL : DEFAULT_VOLUME;
            if (type == PORT_TYPE_VIBRATION) {
                try {
                    openVibrationDevice(port);
                } catch (...) {
                    port = Port{};
                    throw;
                }
            }
            for (int c = 0; c < port.channels; c++) {
                port.volume[c] = DEFAULT_VOLUME;
            }
            if (type != PORT_TYPE_VIBRATION && type != PORT_TYPE_EX && type != PORT_TYPE_SYS) {
                openDevice(port);
            }
            static const bool trace = std::getenv("APS5_TRACE_AUDIOOUT") != nullptr;
            if (trace) std::fprintf(stderr, "[audioout] port %d: type %d, %u samples at %u Hz, format %d, device %u (device gives %d Hz, format 0x%x, %u channels, %u samples)\n", i + 1, type, len, freq, static_cast<int>(format), static_cast<unsigned>(port.device), port.spec.freq, static_cast<unsigned>(port.spec.format), static_cast<unsigned>(port.spec.channels), static_cast<unsigned>(port.spec.samples));
            return i + 1;
        }
    }
    return -2144993275;
}

static std::atomic<bool> g_audioOut2Initialized{false};
static std::atomic<bool> g_audioOutInitialized{false};
static constexpr int SCE_AUDIO_OUT2_ERROR_ALREADY_INITIALIZED = static_cast<int>(0x80268004);
static constexpr int SCE_AUDIO_OUT_ERROR_ALREADY_INITIALIZED = static_cast<int>(0x8026000e);
static constexpr int SCE_AUDIO_OUT_ERROR_INVALID_FORMAT = static_cast<int>(0x80260007);

bool AudioOut2IsInitialized_nid_no_patch() {
    return g_audioOut2Initialized.load();
}

void AudioOut2MarkInitialized_nid_no_patch() {
    g_audioOut2Initialized.store(true);
}

bool AudioOut2TryInitialize_nid_no_patch() {
    return !g_audioOut2Initialized.exchange(true);
}

extern "C" {

int APS5_VABI sceAudioOut2Initialize(void) {
    return g_audioOut2Initialized.exchange(true) ? SCE_AUDIO_OUT2_ERROR_ALREADY_INITIALIZED : 0;
}

int APS5_VABI sceAudioOutInit() {
    if (g_audioOutInitialized.exchange(true)) return SCE_AUDIO_OUT_ERROR_ALREADY_INITIALIZED;
    AudioOut2MarkInitialized_nid_no_patch();
    return 0;
}

int APS5_VABI sceAudioOutOpen(int userId, int type, int index, std::uint32_t len,
    std::uint32_t freq, std::uint32_t param) {
    CountAudioOutApi(AudioOutApi::Open);
    (void)userId;
    (void)index;
    if (!portTypeValid(type)) {
        return -2144993270;
    }
    if (len == 0) {
        return -2144993274;
    }
    if (freq == 0) {
        return -2144993272;
    }

    Format format = Format::Unknown;
    switch (param & FORMAT_MASK) {
        case 0: format = Format::S16Mono; break;
        case 1: format = Format::S16Stereo; break;
        case 2: format = Format::S16_8Ch; break;
        case 3: format = Format::F32Mono; break;
        case 4: format = Format::F32Stereo; break;
        case 5: format = Format::F32_8Ch; break;
        case 6: format = Format::S16_8ChStd; break;
        case 7: format = Format::F32_8ChStd; break;
        default:
            return SCE_AUDIO_OUT_ERROR_INVALID_FORMAT;
    }

    if (type == PORT_TYPE_VIBRATION && channelsForFormat(format) > 2) throw std::runtime_error("sceAudioOutOpen: vibration layouts above stereo are not implemented");
    return allocatePort(type, len, freq, format);
}

int APS5_VABI sceAudioOutClose(int handle) {
    CountAudioOutApi(AudioOutApi::Close);
    if (handle <= 0 || handle > PORTS_MAX) return -2144993277;
    std::lock_guard<std::mutex> outputLock(g_outputMutexes[handle - 1]);
    std::lock_guard<std::mutex> lock(g_mutex);
    Port* port = getPort(handle);
    if (port == nullptr) {
        return -2144993277;
    }
    closeDevice(*port);
    if (port->type == PORT_TYPE_VIBRATION) PadClearLegacyAudioHaptics_nid_postfix(handle);
    *port = Port{};
    return 0;
}

int APS5_VABI sceAudioOutOutput(int handle, const void* ptr) try {
    CountAudioOutApi(AudioOutApi::Output);
    const AudioOutCall call;
    const std::uint64_t callStart = AudioOutTraceEnabled() ? sceKernelGetProcessTime() : 0;
    if (handle <= 0 || handle > PORTS_MAX) return -2144993277;
    std::lock_guard<std::mutex> outputLock(g_outputMutexes[handle - 1]);
    std::unique_lock<std::mutex> lock(g_mutex);
    Port* port = getPort(handle);
    if (port == nullptr) {
        return -2144993277;
    }

    reopenStoppedDevice(*port);
    if (port->device == 0) {
        outputVirtualPort(*port, ptr, lock);
    } else {
        queueAudio(*port, ptr, lock);
    }
    if (ptr != nullptr) port->lastDataOutputTime = sceKernelGetProcessTime();
    RecordAudioOutCall(callStart, port->device == 0);
    return static_cast<int>(port->samplesNum);
} catch (const ProcessShutdown&) {
    LibcAwaitExit_nid_postfix();
}

int APS5_VABI sceAudioOutOutputs(AudioOutOutputParam* param, std::uint32_t num) try {
    CountAudioOutApi(AudioOutApi::Outputs);
    const AudioOutCall call;
    const std::uint64_t callStart = AudioOutTraceEnabled() ? sceKernelGetProcessTime() : 0;
    if (param == nullptr || num == 0) {
        return -2144993276;
    }

    std::array<bool, PORTS_MAX> selected{};
    for (std::uint32_t i = 0; i < num; ++i) {
        if (param[i].handle <= 0 || param[i].handle > PORTS_MAX) return -2144993277;
        selected[param[i].handle - 1] = true;
    }
    std::array<std::unique_lock<std::mutex>, PORTS_MAX> outputLocks;
    for (int index = 0; index < PORTS_MAX; ++index) {
        if (selected[index]) outputLocks[index] = std::unique_lock<std::mutex>(g_outputMutexes[index]);
    }
    std::unique_lock<std::mutex> lock(g_mutex);

    for (std::uint32_t i = 0; i < num; i++) {
        if (getPort(param[i].handle) == nullptr) {
            return -2144993277;
        }
    }

    Port& first = *getPort(param[0].handle);
    for (std::uint32_t i = 0; i < num; i++) {
        Port& port = *getPort(param[i].handle);
        reopenStoppedDevice(port);
        if (port.device == 0) {
            outputVirtualPort(port, param[i].ptr, lock);
        } else {
            queueAudio(port, param[i].ptr, lock);
        }
    }

    const std::uint64_t done = sceKernelGetProcessTime();
    for (std::uint32_t i = 0; i < num; i++) {
        if (param[i].ptr != nullptr) getPort(param[i].handle)->lastDataOutputTime = done;
    }

    RecordAudioOutCall(callStart, first.device == 0);
    return static_cast<int>(first.samplesNum);
} catch (const ProcessShutdown&) {
    LibcAwaitExit_nid_postfix();
}

int APS5_VABI sceAudioOutSetVolume(int handle, std::uint32_t flag, int* vol) {
    CountAudioOutApi(AudioOutApi::SetVolume);
    if (vol == nullptr) {
        return -2144993276;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    Port* port = getPort(handle);
    if (port == nullptr) {
        return -2144993277;
    }
    const bool isStd = formatIsStd(port->format);
    for (int i = 0; i < port->channels; i++, flag >>= 1u) {
        if ((flag & 1u) == 0) {
            continue;
        }
        int srcIdx = i;
        if (isStd) {
            if (i == 4) srcIdx = 6;
            else if (i == 5) srcIdx = 7;
            else if (i == 6) srcIdx = 4;
            else if (i == 7) srcIdx = 5;
        }
        port->volume[i] = vol[srcIdx];
    }
    return 0;
}

int APS5_VABI sceAudioOutGetLastOutputTime(int handle, std::uint64_t* outputTime) {
    CountAudioOutApi(AudioOutApi::GetLastOutputTime);
    if (outputTime == nullptr) {
        return -2144993276;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    Port* port = getPort(handle);
    if (port == nullptr) {
        return -2144993277;
    }
    const std::uint64_t time = port->lastDataOutputTime;
    const int savedErrno = errno;
    const bool copied = GuestCopyOut_nid_no_patch(outputTime, &time, sizeof(time));
    errno = savedErrno;
    if (!copied) {
        return -2144993276;
    }
    return 0;
}

int APS5_VABI sceAudioOutGetPortState(int handle, AudioOutPortState* state) {
    CountAudioOutApi(AudioOutApi::GetPortState);
    if (state == nullptr) {
        return -2144993276;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    Port* port = getPort(handle);
    if (port == nullptr) {
        return -2144993277;
    }
    AudioOutPortState localState{};
    localState.rerouteCounter = 0;
    localState.volume = 127;
    localState.flag = 0;
    localState.activeState = 0;
    localState.reserved[0] = 0;
    switch (port->type) {
        case PORT_TYPE_MAIN:
        case PORT_TYPE_BGM:
        case PORT_TYPE_AUDIO3D:
            localState.output = 1;
            localState.channel = static_cast<std::uint8_t>(port->channels > 2 ? 2 : port->channels);
            break;
        case PORT_TYPE_VOICE:
        case PORT_TYPE_PERSONAL:
            localState.output = 0x40;
            localState.channel = 1;
            break;
        case PORT_TYPE_PADSPK:
        case PORT_TYPE_VIBRATION:
            localState.output = 4;
            localState.channel = 1;
            break;
        case PORT_TYPE_AUX:
            localState.output = 0x80;
            localState.channel = 0;
            break;
        default:
            throw std::runtime_error("sceAudioOutGetPortState: unknown port type");
    }
    const int savedErrno = errno;
    const bool copied = GuestCopyOut_nid_no_patch(state, &localState, sizeof(localState));
    errno = savedErrno;
    if (!copied) {
        return -2144993276;
    }
    return 0;
}

int APS5_VABI sceAudioOutDeviceIdOpen(std::uint64_t deviceId, std::int32_t type, std::int32_t index, std::uint32_t length, std::int32_t frequency, std::uint32_t parameter) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceAudioOutSysClose(int handle) {
    CountAudioOutApi(AudioOutApi::SysClose);
    if (handle <= 0 || handle > PORTS_MAX) {
        throw std::runtime_error("sceAudioOutSysClose: handle " + std::to_string(handle) + " is not an open Sys port");
    }
    std::lock_guard<std::mutex> outputLock(g_outputMutexes[handle - 1]);
    std::lock_guard<std::mutex> lock(g_mutex);
    Port* port = getPort(handle);
    if (port == nullptr || port->type != PORT_TYPE_SYS) {
        throw std::runtime_error("sceAudioOutSysClose: handle " + std::to_string(handle) + " is not an open Sys port");
    }
    *port = Port{};
    return 0;
}

int APS5_VABI sceAudioOutSysConfigureOutput(int type, std::uint64_t flags, std::uint32_t mode, std::uint32_t target, std::uint64_t option) {
    if (type != SYS_CONFIGURE_TYPE_HDMI) {
        throw std::runtime_error("sceAudioOutSysConfigureOutput: output type " + std::to_string(type) + " not supported");
    }
    if (flags != 0 || option != 0) {
        throw std::runtime_error("sceAudioOutSysConfigureOutput: flags " + std::to_string(flags) + " and option " + std::to_string(option) + " not supported");
    }
    if (mode != SYS_OPEN_MODE_TRUEHD) {
        throw std::runtime_error("sceAudioOutSysConfigureOutput: mode " + std::to_string(mode) + " not supported");
    }
    if (target != SYS_CONFIGURE_TARGET_HDMI) {
        throw std::runtime_error("sceAudioOutSysConfigureOutput: target " + std::to_string(target) + " not supported");
    }
    return 0;
}

int APS5_VABI sceAudioOutSysGetHdmiMonitorInfo(int type, void* info, std::uint32_t size) {
    if (info == nullptr) return static_cast<int>(0x80260004);
    if (size != HDMI_MONITOR_INFO_SIZE) return static_cast<int>(0x80260006);
    if (type != MONITOR_TYPE_HDMI && type != 0x10) return static_cast<int>(0x8026000C);
    if (type == 0x10) throw std::runtime_error("sceAudioOutSysGetHdmiMonitorInfo: monitor type 16 not supported");
    std::memset(info, 0, HDMI_MONITOR_INFO_SIZE);
    return 0;
}

int APS5_VABI sceAudioOutSysOpen(int userId, std::uint32_t mode) {
    CountAudioOutApi(AudioOutApi::SysOpen);
    (void)userId;
    if (mode != SYS_OPEN_MODE_TRUEHD) {
        throw std::runtime_error("sceAudioOutSysOpen: mode " + std::to_string(mode) + " not supported");
    }
    return allocatePort(PORT_TYPE_SYS, SYS_PORT_SAMPLES, SYS_PORT_FRAME_RATE, Format::S16_8Ch);
}

int APS5_VABI sceAudioOutSetMainOutput() {
    return 0;
}

int APS5_VABI sceAudioOutSetMixLevelPadSpk(int handle, std::uint32_t mixLevel) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Port* port = getPort(handle);
    if (port == nullptr) {
        return -2144993277;
    }
    if (mixLevel > DEFAULT_VOLUME) {
        return -2144993260;
    }
    if (port->type != PORT_TYPE_PADSPK) {
        return -2144993270;
    }
    port->mixLevel = mixLevel;
    return 0;
}

int APS5_VABI sceAudioOutExClose(int handle) {
    CountAudioOutApi(AudioOutApi::ExClose);
    if (handle <= 0 || handle > PORTS_MAX) {
        throw std::runtime_error("sceAudioOutExClose: handle " + std::to_string(handle) + " is not an open Ex port");
    }
    std::lock_guard<std::mutex> outputLock(g_outputMutexes[handle - 1]);
    std::lock_guard<std::mutex> lock(g_mutex);
    Port* port = getPort(handle);
    if (port == nullptr || port->type != PORT_TYPE_EX) {
        throw std::runtime_error("sceAudioOutExClose: handle " + std::to_string(handle) + " is not an open Ex port");
    }
    *port = Port{};
    return 0;
}

int APS5_VABI sceAudioOutExConfigureOutput(int control, std::uint64_t flags, std::uint32_t mode, std::uint32_t target, std::uint64_t option) {
    if (control != 0) {
        return ERROR_EX_INVALID_CONTROL;
    }
    if (flags != 0 || option != 0) {
        throw std::runtime_error("sceAudioOutExConfigureOutput: flags " + std::to_string(flags) + " and option " + std::to_string(option) + " not supported");
    }
    if (mode > EX_OPEN_MODE_MAX && mode != EX_CONFIGURE_MODE_DEFAULT) {
        throw std::runtime_error("sceAudioOutExConfigureOutput: mode " + std::to_string(mode) + " not supported");
    }
    if ((target < 1 || target > 3) && target != EX_CONFIGURE_TARGET_DEFAULT) {
        throw std::runtime_error("sceAudioOutExConfigureOutput: target " + std::to_string(target) + " not supported");
    }
    return 0;
}

int APS5_VABI sceAudioOutExGetMonitorInfo(int type, int index, void* info, std::uint32_t size) {
    if (type != 0) return static_cast<int>(0x80260003);
    if (info == nullptr) return static_cast<int>(0x80260004);
    if (size != EX_MONITOR_INFO_SIZE) return static_cast<int>(0x80260006);
    const std::array<std::uint8_t, EX_MONITOR_INFO_SIZE> empty{};
    const int savedErrno = errno;
    const bool copied = GuestCopyOut_nid_no_patch(info, empty.data(), empty.size());
    errno = savedErrno;
    if (!copied) return static_cast<int>(0x80260004);
    return 0;
}

int APS5_VABI sceAudioOutExOpen(int userId, std::uint32_t mode) {
    CountAudioOutApi(AudioOutApi::ExOpen);
    (void)userId;
    if (mode > EX_OPEN_MODE_MAX || ((EX_OPEN_MODES >> mode) & 1) == 0) {
        return ERROR_EX_INVALID_MODE;
    }
    const bool highRate = mode == 3 || mode == 4 || mode == 10;
    return allocatePort(PORT_TYPE_EX, highRate ? 1024 : 256, highRate ? 192000 : 48000, Format::S16Stereo);
}

int APS5_VABI sceAudioOutGetSystemState(AudioOutSystemState* state) {
    if (state == nullptr) {
        return -2144993276;
    }
    AudioOutSystemState localState{};
    localState.loudness = 0.0f;
    const int savedErrno = errno;
    const bool copied = GuestCopyOut_nid_no_patch(state, &localState, sizeof(localState));
    errno = savedErrno;
    if (!copied) {
        return -2144993276;
    }
    return 0;
}

}
