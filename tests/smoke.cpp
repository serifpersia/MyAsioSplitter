// Smoke test: COM wiring, IASIO vtable, and Tap producer -> ring -> consumer path.
// Requires MyAsioSplitter.dll to be registered with regsvr32.
// Run with Pianoteq/OBS closed; this test temporarily claims the shared master slot.
#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../src/asio_abi.h"
#include "../src/shm_ring.h"

static const CLSID CLSID_MyAsioSplitterTap =
    {0x9a4f2d18, 0x6b3c, 0x4e57, {0xa1, 0xd0, 0x2c, 0x8e, 0x7f, 0x4b, 0x9a, 0x61}};

static std::atomic<int> g_callbacks{0};
static std::atomic<bool> g_sawTapData{false};
static float* g_watch[2] = {};
static long g_watchFrames = 0;

static void WatchBuffers(long index) {
    if (!g_watch[index] || g_watchFrames <= 0) return;
    for (long i = 0; i < g_watchFrames; ++i) {
        if (std::fabs(g_watch[index][i] - 0.25f) < 1e-4f) {
            g_sawTapData.store(true, std::memory_order_relaxed);
            return;
        }
    }
}

static void OnBufferSwitch(long index, ASIOBool) {
    g_callbacks.fetch_add(1, std::memory_order_relaxed);
    WatchBuffers(index);
}

static ASIOTime* OnBufferSwitchTimeInfo(ASIOTime* time, long index, ASIOBool) {
    g_callbacks.fetch_add(1, std::memory_order_relaxed);
    WatchBuffers(index);
    return time;
}

// Publishes one real-time-sized block at a time, like a hardware callback. This
// is what exposes a consumer that throws away data while it is priming.
static void Produce(ShmBlock* block, long chunk, int millis) {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

    LARGE_INTEGER due = {};
    due.QuadPart = -static_cast<LONGLONG>(10000000.0 * static_cast<double>(chunk) / 48000.0);
    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(millis);

    while (GetTickCount64() < end) {
        if (timer) {
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep(1);
        }

        const uint64_t write = block->writeIdx.load(std::memory_order_relaxed);
        const uint64_t read = block->readIdx.load(std::memory_order_acquire);
        if (write - read + static_cast<uint64_t>(chunk) > kRingFrames) continue;

        for (long i = 0; i < chunk; ++i) {
            float* sample = &block->samples[((write + static_cast<uint64_t>(i)) & kRingMask) * 2];
            sample[0] = 0.25f;
            sample[1] = 0.25f;
        }
        block->writeIdx.store(write + static_cast<uint64_t>(chunk), std::memory_order_release);
    }

    if (timer) CloseHandle(timer);
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

    IASIO* driver = nullptr;
    const HRESULT hr = CoCreateInstance(CLSID_MyAsioSplitterTap, nullptr, CLSCTX_INPROC_SERVER,
                                        CLSID_MyAsioSplitterTap, reinterpret_cast<void**>(&driver));
    if (FAILED(hr) || !driver) {
        std::printf("CoCreateInstance failed: 0x%08lX (is the DLL registered?)\n",
                    static_cast<unsigned long>(hr));
        CoUninitialize();
        return 1;
    }

    char name[64] = {};
    driver->getDriverName(name);
    CHECK(std::strcmp(name, "MyASIO Splitter (Tap)") == 0);

    // Stand in for the master before init so the Tap picks up the same format.
    ShmMapping shm;
    CHECK(shm.Attach());
    ShmBlock* block = shm.block();
    CHECK(block != nullptr);
    if (!block) {
        driver->Release();
        CoUninitialize();
        return 1;
    }

    const long bufferSize = 64;
    block->masterPid.store(GetCurrentProcessId(), std::memory_order_release);
    block->generation.fetch_add(1, std::memory_order_acq_rel);
    block->masterStreaming.store(0, std::memory_order_relaxed);
    block->outputChannels.store(2, std::memory_order_relaxed);
    block->bufferSize.store(static_cast<uint32_t>(bufferSize), std::memory_order_relaxed);
    block->sampleRateBits.store(SampleRateToBits(48000.0), std::memory_order_release);
    block->writeIdx.store(0, std::memory_order_relaxed);
    block->readIdx.store(0, std::memory_order_relaxed);

    CHECK(driver->init(nullptr) == ASIOTrue);

    long inputs = -1;
    long outputs = -1;
    CHECK(driver->getChannels(&inputs, &outputs) == ASE_OK);
    CHECK(inputs == 2);
    CHECK(outputs == 0);

    long minSize = 0, maxSize = 0, preferred = 0, granularity = 0;
    CHECK(driver->getBufferSize(&minSize, &maxSize, &preferred, &granularity) == ASE_OK);
    CHECK(preferred == bufferSize);

    ASIOSampleRate rate = 0.0;
    CHECK(driver->getSampleRate(&rate) == ASE_OK);
    CHECK(rate >= 8000.0 && rate <= 192000.0);
    CHECK(driver->canSampleRate(48000.0) == ASE_OK);
    CHECK(driver->canSampleRate(12345.0) == ASE_NoClock);

    ASIOClockSource clock = {};
    long numSources = 0;
    CHECK(driver->getClockSources(&clock, &numSources) == ASE_OK);
    CHECK(numSources == 1);
    CHECK(clock.isCurrentSource == ASIOTrue);

    CHECK(driver->future(kAsioEngineVersion, nullptr) == 2);

    ASIOBufferInfo infos[2] = {};
    infos[0].isInput = ASIOTrue;
    infos[0].channelNum = 0;
    infos[1].isInput = ASIOTrue;
    infos[1].channelNum = 1;

    ASIOCallbacks callbacks = {};
    callbacks.bufferSwitch = &OnBufferSwitch;
    callbacks.bufferSwitchTimeInfo = &OnBufferSwitchTimeInfo;

    CHECK(driver->createBuffers(infos, 2, bufferSize, &callbacks) == ASE_OK);
    CHECK(infos[0].buffers[0] != nullptr && infos[0].buffers[1] != nullptr);
    CHECK(infos[1].buffers[0] != nullptr && infos[1].buffers[1] != nullptr);
    CHECK(infos[0].buffers[0] != infos[0].buffers[1]);

    g_watch[0] = static_cast<float*>(infos[0].buffers[0]);
    g_watch[1] = static_cast<float*>(infos[0].buffers[1]);
    g_watchFrames = bufferSize;

    CHECK(driver->start() == ASE_OK);
    block->masterStreaming.store(1, std::memory_order_release);
    Produce(block, bufferSize, 500);
    block->masterStreaming.store(0, std::memory_order_release);
    CHECK(driver->stop() == ASE_OK);

    CHECK(g_callbacks.load(std::memory_order_relaxed) > 20);
    CHECK(g_sawTapData.load(std::memory_order_relaxed));

    ASIOSamples position = {};
    ASIOTimeStamp stamp = {};
    CHECK(driver->getSamplePosition(&position, &stamp) == ASE_OK);
    CHECK((position.hi != 0) || (position.lo != 0));

    CHECK(driver->disposeBuffers() == ASE_OK);

    block->masterPid.store(0, std::memory_order_release);

    driver->Release();
    shm.Detach();
    CoUninitialize();

    if (failures == 0) {
        std::printf("PASS: COM, IASIO vtable, and tap ring/worker path are correct.\n");
        return 0;
    }
    std::printf("%d check(s) failed.\n", failures);
    return 1;
}
