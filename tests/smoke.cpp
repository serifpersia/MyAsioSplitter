#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../src/asio_abi.h"
#include "../src/shm_ring.h"

static const CLSID CLSID_MyAsioSplitterTap =
    {0x9a4f2d18, 0x6b3c, 0x4e57, {0xa1, 0xd0, 0x2c, 0x8e, 0x7f, 0x4b, 0x9a, 0x61}};

static std::atomic<int> g_callbacks{0};
static std::atomic<int> g_wrongCallbacks{0};
static std::atomic<int> g_timeInfoErrors{0};
static std::atomic<bool> g_sawLeft{false};
static std::atomic<bool> g_sawRight{false};
static std::atomic<bool> g_checkSilence{false};
static std::atomic<int> g_silenceErrors{0};

static float* g_left[2] = {};
static float* g_right[2] = {};
static long g_watchFrames = 0;
static double g_expectedRate = 48000.0;
static uint64_t g_expectedPos = 0;

static void WatchBuffers(long index) {
    if (!g_left[index] || !g_right[index] || g_watchFrames <= 0) return;

    if (g_checkSilence.load(std::memory_order_acquire)) {
        for (long i = 0; i < g_watchFrames; ++i) {
            if (std::fabs(g_left[index][i]) > 1e-6f || std::fabs(g_right[index][i]) > 1e-6f) {
                g_silenceErrors.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        return;
    }

    for (long i = 0; i < g_watchFrames; ++i) {
        if (std::fabs(g_left[index][i] - 0.25f) < 1e-4f) {
            g_sawLeft.store(true, std::memory_order_relaxed);
        }
        if (std::fabs(g_right[index][i] + 0.50f) < 1e-4f) {
            g_sawRight.store(true, std::memory_order_relaxed);
        }
    }
}

static long OnAsioMessage(long selector, long, void*, double*) {
    if (selector == kAsioSupportsTimeInfo) return 1;
    if (selector == kAsioSelectorSupported) return 1;
    return 0;
}

static void OnBufferSwitch(long, ASIOBool) {
    g_wrongCallbacks.fetch_add(1, std::memory_order_relaxed);
}

static ASIOTime* OnBufferSwitchTimeInfo(ASIOTime* time, long index, ASIOBool) {
    if (!time) {
        g_timeInfoErrors.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    const unsigned long expectedFlags =
        kSystemTimeValid | kSamplePositionValid | kSampleRateValid | kSpeedValid;
    const uint64_t pos = (static_cast<uint64_t>(time->timeInfo.samplePosition.hi) << 32) |
                         time->timeInfo.samplePosition.lo;
    if (time->timeInfo.flags != expectedFlags ||
        std::fabs(time->timeInfo.sampleRate - g_expectedRate) > 0.5 ||
        pos != g_expectedPos) {
        g_timeInfoErrors.fetch_add(1, std::memory_order_relaxed);
    }
    g_expectedPos += static_cast<uint64_t>(g_watchFrames);

    g_callbacks.fetch_add(1, std::memory_order_relaxed);
    WatchBuffers(index);
    return time;
}

static void Produce(ShmBlock* block, long chunk, double sampleRate, int millis) {
    LARGE_INTEGER freq = {};
    QueryPerformanceFrequency(&freq);
    const double periodTicks =
        (static_cast<double>(chunk) / sampleRate) * static_cast<double>(freq.QuadPart);

    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    double nextTick = static_cast<double>(now.QuadPart);
    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(millis);

    while (GetTickCount64() < end) {
        QueryPerformanceCounter(&now);
        if (static_cast<double>(now.QuadPart) < nextTick) {
            SwitchToThread();
            continue;
        }
        nextTick += periodTicks;
        if (static_cast<double>(now.QuadPart) - nextTick > periodTicks * 8.0) {
            nextTick = static_cast<double>(now.QuadPart);
        }

        const uint64_t write = block->writeIdx.load(std::memory_order_relaxed);
        const uint64_t read = block->readIdx.load(std::memory_order_acquire);
        if (write - read + static_cast<uint64_t>(chunk) > kRingFrames) continue;

        for (long i = 0; i < chunk; ++i) {
            float* sample = &block->samples[((write + static_cast<uint64_t>(i)) & kRingMask) * 2];
            sample[0] = 0.25f;
            sample[1] = -0.50f;
        }
        block->writeIdx.store(write + static_cast<uint64_t>(chunk), std::memory_order_release);
    }
}

static int failures = 0;

#define CHECK(expr)                                                       \
    do {                                                                  \
        if (!(expr)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr);   \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

int main() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) {
        std::printf("CoInitializeEx failed: 0x%08lX\n", static_cast<unsigned long>(com));
        return 1;
    }

    HMODULE module = LoadLibraryW(L"MyAsioSplitter.dll");
    if (!module) {
        std::printf("LoadLibraryW failed: %lu\n", GetLastError());
        CoUninitialize();
        return 1;
    }

    using GetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    using CanUnloadNowFn = HRESULT(STDAPICALLTYPE*)();
    auto getClassObject =
        reinterpret_cast<GetClassObjectFn>(GetProcAddress(module, "DllGetClassObject"));
    auto canUnloadNow =
        reinterpret_cast<CanUnloadNowFn>(GetProcAddress(module, "DllCanUnloadNow"));

    auto createTap = [&]() -> IASIO* {
        IClassFactory* factory = nullptr;
        HRESULT hr = getClassObject
                         ? getClassObject(CLSID_MyAsioSplitterTap, IID_IClassFactory,
                                          reinterpret_cast<void**>(&factory))
                         : E_FAIL;
        IASIO* drv = nullptr;
        if (factory) {
            hr = factory->CreateInstance(nullptr, CLSID_MyAsioSplitterTap,
                                         reinterpret_cast<void**>(&drv));
            factory->Release();
        }
        return SUCCEEDED(hr) ? drv : nullptr;
    };

    ShmMapping shm;
    CHECK(shm.Attach());
    ShmBlock* block = shm.block();
    CHECK(block != nullptr);
    if (!block) {
        FreeLibrary(module);
        CoUninitialize();
        return 1;
    }

    const uint32_t existing = block->masterPid.load(std::memory_order_acquire);
    if (existing != 0 && existing != GetCurrentProcessId() && ProcessAlive(existing)) {
        std::printf("Close Pianoteq/OBS before running smoke_test.\n");
        shm.Detach();
        FreeLibrary(module);
        CoUninitialize();
        return 1;
    }

    std::memset(block->samples, 0, sizeof(block->samples));
    block->masterPid.store(GetCurrentProcessId(), std::memory_order_release);
    block->generation.fetch_add(1, std::memory_order_acq_rel);
    block->masterStreaming.store(0, std::memory_order_relaxed);
    block->outputChannels.store(2, std::memory_order_relaxed);
    block->bufferSize.store(16, std::memory_order_relaxed);
    block->sampleRateBits.store(SampleRateToBits(96000.0), std::memory_order_release);
    block->writeIdx.store(0, std::memory_order_relaxed);
    block->readIdx.store(0, std::memory_order_relaxed);

    {
        IASIO* driver = createTap();
        CHECK(driver != nullptr);
        if (!driver) {
            block->masterPid.store(0, std::memory_order_release);
            shm.Detach();
            FreeLibrary(module);
            CoUninitialize();
            return 1;
        }

        CHECK(driver->init(nullptr) == ASIOTrue);
        long minSize = 0, maxSize = 0, preferred = 0, granularity = 0;
        CHECK(driver->getBufferSize(&minSize, &maxSize, &preferred, &granularity) == ASE_OK);
        CHECK(minSize == 256 && preferred == 512 && granularity == -1);
        CHECK(driver->future(kAsioCanTimeInfo, nullptr) == ASE_SUCCESS);
        CHECK(driver->future(kAsioEngineVersion, nullptr) == 2);
        CHECK(driver->future(kAsioSelectorSupported,
                             reinterpret_cast<void*>(static_cast<intptr_t>(kAsioEngineVersion))) ==
              ASIOTrue);
        CHECK(driver->future(kAsioSelectorSupported,
                             reinterpret_cast<void*>(static_cast<intptr_t>(kAsioSupportsTimeInfo))) ==
              ASIOTrue);
        CHECK(driver->future(kAsioSelectorSupported,
                             reinterpret_cast<void*>(static_cast<intptr_t>(kAsioCanTimeCode))) ==
              ASIOFalse);
        driver->Release();
        CHECK(canUnloadNow && canUnloadNow() == S_OK);
    }

    block->bufferSize.store(16, std::memory_order_relaxed);
    block->sampleRateBits.store(SampleRateToBits(48000.0), std::memory_order_release);
    block->generation.fetch_add(1, std::memory_order_acq_rel);

    {
        IASIO* driver = createTap();
        CHECK(driver != nullptr);
        if (!driver) {
            block->masterPid.store(0, std::memory_order_release);
            shm.Detach();
            FreeLibrary(module);
            CoUninitialize();
            return 1;
        }

        char name[64] = {};
        driver->getDriverName(name);
        CHECK(std::strcmp(name, "MyASIO Splitter (Tap)") == 0);
        CHECK(driver->init(nullptr) == ASIOTrue);

        long inputs = -1;
        long outputs = -1;
        CHECK(driver->getChannels(&inputs, &outputs) == ASE_OK);
        CHECK(inputs == 2);
        CHECK(outputs == 0);

        ASIOChannelInfo channel = {};
        channel.isInput = ASIOTrue;
        channel.channel = 0;
        CHECK(driver->getChannelInfo(&channel) == ASE_OK);
        CHECK(channel.isActive == ASIOTrue);
        CHECK(channel.type == ASIOSTFloat32LSB);
        CHECK(channel.channelGroup == 0);
        channel.channel = 1;
        CHECK(driver->getChannelInfo(&channel) == ASE_OK);
        CHECK(channel.isActive == ASIOTrue);
        CHECK(channel.type == ASIOSTFloat32LSB);
        CHECK(channel.channelGroup == 0);

        long minSize = 0, maxSize = 0, preferred = 0, granularity = 0;
        CHECK(driver->getBufferSize(&minSize, &maxSize, &preferred, &granularity) == ASE_OK);
        CHECK(minSize == 128 && preferred == 256 && maxSize == 4096 && granularity == -1);
        const long tapBufferSize = preferred;

        ASIOSampleRate rate = 0.0;
        CHECK(driver->getSampleRate(&rate) == ASE_OK);
        CHECK(std::fabs(rate - 48000.0) < 0.5);
        CHECK(driver->canSampleRate(48000.0) == ASE_OK);
        CHECK(driver->canSampleRate(12345.0) == ASE_NoClock);

        ASIOClockSource clock = {};
        long numSources = 0;
        CHECK(driver->getClockSources(&clock, &numSources) == ASE_OK);
        CHECK(numSources == 1);
        CHECK(clock.isCurrentSource == ASIOTrue);

        ASIOBufferInfo infos[2] = {};
        infos[0].isInput = ASIOTrue;
        infos[0].channelNum = 1;
        infos[1].isInput = ASIOTrue;
        infos[1].channelNum = 0;

        ASIOCallbacks callbacks = {};
        callbacks.bufferSwitch = &OnBufferSwitch;
        callbacks.asioMessage = &OnAsioMessage;
        callbacks.bufferSwitchTimeInfo = &OnBufferSwitchTimeInfo;

        CHECK(driver->createBuffers(infos, 2, tapBufferSize, &callbacks) == ASE_OK);
        CHECK(infos[0].buffers[0] != nullptr && infos[0].buffers[1] != nullptr);
        CHECK(infos[1].buffers[0] != nullptr && infos[1].buffers[1] != nullptr);
        CHECK(infos[0].buffers[0] != infos[0].buffers[1]);

        g_left[0] = static_cast<float*>(infos[1].buffers[0]);
        g_left[1] = static_cast<float*>(infos[1].buffers[1]);
        g_right[0] = static_cast<float*>(infos[0].buffers[0]);
        g_right[1] = static_cast<float*>(infos[0].buffers[1]);
        g_watchFrames = tapBufferSize;
        g_expectedRate = 48000.0;
        g_expectedPos = 0;
        g_callbacks.store(0);
        g_wrongCallbacks.store(0);
        g_timeInfoErrors.store(0);
        g_sawLeft.store(false);
        g_sawRight.store(false);
        g_checkSilence.store(false);
        g_silenceErrors.store(0);

        CHECK(driver->start() == ASE_OK);
        block->masterStreaming.store(1, std::memory_order_release);
        Produce(block, 16, 48000.0, 500);

        block->masterStreaming.store(0, std::memory_order_release);
        Sleep(80);
        g_checkSilence.store(true, std::memory_order_release);
        Sleep(100);
        g_checkSilence.store(false, std::memory_order_release);

        CHECK(driver->stop() == ASE_OK);

        CHECK(g_callbacks.load(std::memory_order_relaxed) > 50);
        CHECK(g_wrongCallbacks.load(std::memory_order_relaxed) == 0);
        CHECK(g_timeInfoErrors.load(std::memory_order_relaxed) == 0);
        CHECK(g_sawLeft.load(std::memory_order_relaxed));
        CHECK(g_sawRight.load(std::memory_order_relaxed));
        CHECK(g_silenceErrors.load(std::memory_order_relaxed) == 0);

        ASIOSamples position = {};
        ASIOTimeStamp stamp = {};
        CHECK(driver->getSamplePosition(&position, &stamp) == ASE_OK);
        CHECK((position.hi != 0) || (position.lo != 0));

        CHECK(driver->disposeBuffers() == ASE_OK);
        driver->Release();
        CHECK(canUnloadNow && canUnloadNow() == S_OK);
    }

    {
        IASIO* driver = createTap();
        CHECK(driver != nullptr && driver->init(nullptr) == ASIOTrue);
        if (driver) {
            ASIOSamples position = {};
            ASIOTimeStamp stamp = {};
            CHECK(driver->getSamplePosition(&position, &stamp) == ASE_OK);
            CHECK(position.hi == 0 && position.lo == 0);
            driver->Release();
        }
        CHECK(canUnloadNow && canUnloadNow() == S_OK);
    }

    block->masterPid.store(0, std::memory_order_release);
    block->masterStreaming.store(0, std::memory_order_release);

    shm.Detach();
    FreeLibrary(module);
    CoUninitialize();

    if (failures == 0) {
        std::printf("PASS: COM, IASIO vtable, 16-sample decoupling, and tap ring/worker path are correct.\n");
        return 0;
    }
    std::printf("%d check(s) failed.\n", failures);
    return 1;
}