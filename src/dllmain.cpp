#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <unknwn.h>

#include <avrt.h>
#include <malloc.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "asio_abi.h"
#include "config.h"
#include "shm_ring.h"

static const CLSID CLSID_MyAsioSplitter =
    {0x7c9e5b6a, 0x3f2d, 0x4c81, {0x9e, 0x4a, 0x5d, 0x6f, 0x8b, 0x1c, 0x2a, 0x30}};

static const CLSID CLSID_MyAsioSplitterTap =
    {0x9a4f2d18, 0x6b3c, 0x4e57, {0xa1, 0xd0, 0x2c, 0x8e, 0x7f, 0x4b, 0x9a, 0x61}};

enum class DriverRole { Master, Tap };

struct DriverEntry {
    CLSID clsid;
    const wchar_t* clsidString;
    const wchar_t* asioSubkey;
    const wchar_t* displayName;
    DriverRole role;
};

static const DriverEntry kDrivers[] = {
    {CLSID_MyAsioSplitter, L"{7C9E5B6A-3F2D-4C81-9E4A-5D6F8B1C2A30}",
     L"SOFTWARE\\ASIO\\MyASIO Splitter", L"MyASIO Splitter", DriverRole::Master},
    {CLSID_MyAsioSplitterTap, L"{9A4F2D18-6B3C-4E57-A1D0-2C8E7F4B9A61}",
     L"SOFTWARE\\ASIO\\MyASIO Splitter (Tap)", L"MyASIO Splitter (Tap)", DriverRole::Tap},
};

static HINSTANCE g_hInstance = nullptr;
static std::atomic<long> g_liveObjects{0};
static std::atomic<long> g_serverLocks{0};

static const DriverEntry* FindDriver(REFCLSID clsid) {
    for (const DriverEntry& entry : kDrivers) {
        if (IsEqualGUID(clsid, entry.clsid)) return &entry;
    }
    return nullptr;
}

struct MasterOutChannel {
    const void* buffers[2] = {nullptr, nullptr};
    ASIOSampleType type = ASIOSTFloat32LSB;
};

static bool IsCommonSampleRate(double rate) {
    return std::fabs(rate - 44100.0) < 0.5 || std::fabs(rate - 48000.0) < 0.5 ||
           std::fabs(rate - 88200.0) < 0.5 || std::fabs(rate - 96000.0) < 0.5 ||
           std::fabs(rate - 176400.0) < 0.5 || std::fabs(rate - 192000.0) < 0.5;
}

static long NextPowerOfTwo(long value, long limit) {
    long p = 1;
    while (p < value && p < limit) p <<= 1;
    return p > limit ? limit : p;
}

static float ClampSample(float value) {
    return (value > -1.0f) ? (value < 1.0f ? value : 1.0f) : (value <= -1.0f ? -1.0f : 0.0f);
}

static void WriteChannelSamples(ASIOSampleType type, const void* src, long srcOffset,
                                float* dst, long count) {
    switch (type) {
        case ASIOSTInt16LSB: {
            const auto* in = static_cast<const int16_t*>(src) + srcOffset;
            for (long i = 0; i < count; ++i) {
                dst[i * 2] = static_cast<float>(in[i]) * (1.0f / 32768.0f);
            }
            break;
        }
        case ASIOSTInt24LSB: {
            const auto* in = static_cast<const uint8_t*>(src) + static_cast<size_t>(srcOffset) * 3;
            for (long i = 0; i < count; ++i) {
                const uint8_t* p = in + static_cast<size_t>(i) * 3;
                const uint32_t packed = static_cast<uint32_t>(p[0]) |
                                        (static_cast<uint32_t>(p[1]) << 8) |
                                        (static_cast<uint32_t>(p[2]) << 16);
                const int32_t sample = static_cast<int32_t>(packed << 8) >> 8;
                dst[i * 2] = static_cast<float>(sample) * (1.0f / 8388608.0f);
            }
            break;
        }
        case ASIOSTInt32LSB: {
            const auto* in = static_cast<const int32_t*>(src) + srcOffset;
            for (long i = 0; i < count; ++i) {
                dst[i * 2] = static_cast<float>(in[i]) * (1.0f / 2147483648.0f);
            }
            break;
        }
        case ASIOSTInt32LSB16: {
            const auto* in = static_cast<const int32_t*>(src) + srcOffset;
            for (long i = 0; i < count; ++i) {
                const int32_t sample = static_cast<int32_t>(static_cast<uint32_t>(in[i]) << 16) >> 16;
                dst[i * 2] = static_cast<float>(sample) * (1.0f / 32768.0f);
            }
            break;
        }
        case ASIOSTInt32LSB18: {
            const auto* in = static_cast<const int32_t*>(src) + srcOffset;
            for (long i = 0; i < count; ++i) {
                const int32_t sample = static_cast<int32_t>(static_cast<uint32_t>(in[i]) << 14) >> 14;
                dst[i * 2] = static_cast<float>(sample) * (1.0f / 131072.0f);
            }
            break;
        }
        case ASIOSTInt32LSB20: {
            const auto* in = static_cast<const int32_t*>(src) + srcOffset;
            for (long i = 0; i < count; ++i) {
                const int32_t sample = static_cast<int32_t>(static_cast<uint32_t>(in[i]) << 12) >> 12;
                dst[i * 2] = static_cast<float>(sample) * (1.0f / 524288.0f);
            }
            break;
        }
        case ASIOSTInt32LSB24: {
            const auto* in = static_cast<const int32_t*>(src) + srcOffset;
            for (long i = 0; i < count; ++i) {
                const int32_t sample = static_cast<int32_t>(static_cast<uint32_t>(in[i]) << 8) >> 8;
                dst[i * 2] = static_cast<float>(sample) * (1.0f / 8388608.0f);
            }
            break;
        }
        case ASIOSTFloat32LSB: {
            const auto* in = static_cast<const float*>(src) + srcOffset;
            for (long i = 0; i < count; ++i) {
                dst[i * 2] = ClampSample(in[i]);
            }
            break;
        }
        case ASIOSTFloat64LSB: {
            const auto* in = static_cast<const double*>(src) + srcOffset;
            for (long i = 0; i < count; ++i) {
                dst[i * 2] = ClampSample(static_cast<float>(in[i]));
            }
            break;
        }
        default: {
            for (long i = 0; i < count; ++i) {
                dst[i * 2] = 0.0f;
            }
            break;
        }
    }
}

static uint64_t QpcFrequency() {
    static const uint64_t hz = []() {
        LARGE_INTEGER freq = {};
        QueryPerformanceFrequency(&freq);
        return freq.QuadPart > 0 ? static_cast<uint64_t>(freq.QuadPart) : 1ULL;
    }();
    return hz;
}

static uint64_t QpcNanoseconds() {
    LARGE_INTEGER counter = {};
    QueryPerformanceCounter(&counter);
    const uint64_t ticks = static_cast<uint64_t>(counter.QuadPart);
    const uint64_t hz = QpcFrequency();
    return (ticks / hz) * 1000000000ULL + ((ticks % hz) * 1000000000ULL) / hz;
}

class MyAsioSplitter final : public IASIO {
public:
    explicit MyAsioSplitter(const DriverEntry& entry) : m_entry(entry) {}

    MyAsioSplitter(const MyAsioSplitter&) = delete;
    MyAsioSplitter& operator=(const MyAsioSplitter&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || IsEqualGUID(riid, m_entry.clsid)) {
            *ppv = static_cast<IASIO*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        const long refs = m_ref.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (refs == 1) g_liveObjects.fetch_add(1, std::memory_order_acq_rel);
        return static_cast<ULONG>(refs);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const long refs = m_ref.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (refs == 0) {
            shutdown();
            g_liveObjects.fetch_sub(1, std::memory_order_acq_rel);
            return 0;
        }
        if (refs < 0) {
            m_ref.store(0, std::memory_order_release);
            return 0;
        }
        return static_cast<ULONG>(refs);
    }

    ASIOBool init(void* sysHandle) override {
        if (m_entry.role == DriverRole::Tap) return initTap();
        if (m_hwAsio) return ASIOTrue;

        const std::vector<AsioDriverInfo> drivers =
            EnumerateAsioDrivers(kDrivers[0].clsid, kDrivers[1].clsid);
        if (drivers.empty()) {
            setError("MyASIO Splitter: no hardware ASIO drivers found under HKLM\\SOFTWARE\\ASIO.");
            return ASIOFalse;
        }

        CLSID selected = {};
        const bool haveSelection = LoadSelectedClsid(selected);
        bool available = false;
        if (haveSelection) {
            for (const AsioDriverInfo& driver : drivers) {
                if (IsEqualGUID(driver.clsid, selected)) {
                    available = true;
                    break;
                }
            }
        }

        if (!available) {
            const int index = ShowAsioDevicePicker(drivers, haveSelection ? &selected : nullptr);
            if (index < 0) {
                setError("MyASIO Splitter: no hardware ASIO device was selected.");
                return ASIOFalse;
            }
            selected = drivers[static_cast<size_t>(index)].clsid;
            SaveSelectedClsid(selected);
        }

        if (!openHardware(selected, sysHandle)) return ASIOFalse;

        if (m_shm.Attach()) {
            ShmBlock* block = m_shm.block();
            m_isPublisher = claimMaster();
            if (m_isPublisher && block) {
                std::memset(block->samples, 0, sizeof(block->samples));
                block->outputChannels.store(2, std::memory_order_relaxed);
                block->bufferSize.store(0, std::memory_order_relaxed);
                ASIOSampleRate rate = 0.0;
                if (m_hwAsio->getSampleRate(&rate) == ASE_OK) publishRate(rate);
            }
        }
        return ASIOTrue;
    }

    void getDriverName(char* name) override {
        if (name) {
            const char* text = (m_entry.role == DriverRole::Tap) ? "MyASIO Splitter (Tap)" : "MyASIO Splitter";
            std::memcpy(name, text, std::strlen(text) + 1);
        }
    }

    long getDriverVersion() override { return m_hwAsio ? m_hwAsio->getDriverVersion() : 1; }

    void getErrorMessage(char* string) override {
        if (!string) return;
        if (m_hwAsio) {
            m_hwAsio->getErrorMessage(string);
        } else {
            std::memcpy(string, m_lastError, sizeof(m_lastError));
        }
    }

    ASIOError start() override {
        if (m_entry.role == DriverRole::Tap) return startTapWorker();
        if (!m_hwAsio || !m_buffersCreated) return ASE_NotPresent;

        const ASIOError result = m_hwAsio->start();
        if (result == ASE_OK) {
            m_started = true;
            if (m_isPublisher && m_shm.block()) {
                m_shm.block()->masterStreaming.store(1, std::memory_order_release);
            }
        }
        return result;
    }

    ASIOError stop() override {
        if (m_entry.role == DriverRole::Tap) {
            stopTapWorker();
            return ASE_OK;
        }
        if (!m_hwAsio) return ASE_OK;
        if (m_isPublisher && m_shm.block()) {
            m_shm.block()->masterStreaming.store(0, std::memory_order_release);
        }
        m_started = false;
        return m_hwAsio->stop();
    }

    ASIOError getChannels(long* numInputChannels, long* numOutputChannels) override {
        if (m_entry.role == DriverRole::Tap) {
            if (numInputChannels) *numInputChannels = 2;
            if (numOutputChannels) *numOutputChannels = 0;
            return ASE_OK;
        }
        if (!m_hwAsio) return ASE_NotPresent;
        return m_hwAsio->getChannels(numInputChannels, numOutputChannels);
    }

    ASIOError getLatencies(long* inputLatency, long* outputLatency) override {
        if (m_entry.role == DriverRole::Tap) {
            if (inputLatency) *inputLatency = m_tapBufferSize;
            if (outputLatency) *outputLatency = 0;
            return ASE_OK;
        }
        if (!m_hwAsio) return ASE_NotPresent;
        return m_hwAsio->getLatencies(inputLatency, outputLatency);
    }

    ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) override {
        if (m_entry.role == DriverRole::Tap) {
            const long master = liveTapBufferSize();
            const double rate = liveTapRate();
            const long minSafe = (rate > 60000.0) ? 256L : 128L;
            const long prefSafe = (rate > 60000.0) ? 512L : 256L;
            const long maxBuf = 4096L;
            const long prefBuf = (master > prefSafe) ? NextPowerOfTwo(master, maxBuf) : prefSafe;
            if (minSize) *minSize = minSafe;
            if (maxSize) *maxSize = maxBuf;
            if (preferredSize) *preferredSize = prefBuf;
            if (granularity) *granularity = -1;
            return ASE_OK;
        }
        if (!m_hwAsio) return ASE_NotPresent;
        return m_hwAsio->getBufferSize(minSize, maxSize, preferredSize, granularity);
    }

    ASIOError canSampleRate(ASIOSampleRate sampleRate) override {
        if (m_entry.role == DriverRole::Tap) {
            if (tapMasterPresent()) {
                return std::fabs(sampleRate - liveTapRate()) < 0.5 ? ASE_OK : ASE_NoClock;
            }
            return IsCommonSampleRate(sampleRate) ? ASE_OK : ASE_NoClock;
        }
        if (!m_hwAsio) return ASE_NotPresent;
        return m_hwAsio->canSampleRate(sampleRate);
    }

    ASIOError getSampleRate(ASIOSampleRate* sampleRate) override {
        if (m_entry.role == DriverRole::Tap) {
            if (sampleRate) *sampleRate = liveTapRate();
            return ASE_OK;
        }
        if (!m_hwAsio) return ASE_NotPresent;
        return m_hwAsio->getSampleRate(sampleRate);
    }

    ASIOError setSampleRate(ASIOSampleRate sampleRate) override {
        if (m_entry.role == DriverRole::Tap) {
            if (tapMasterPresent()) {
                return std::fabs(sampleRate - liveTapRate()) < 0.5 ? ASE_OK : ASE_NoClock;
            }
            if (!IsCommonSampleRate(sampleRate)) return ASE_NoClock;
            m_sampleRate = sampleRate;
            return ASE_OK;
        }
        if (!m_hwAsio) return ASE_NotPresent;
        const ASIOError result = m_hwAsio->setSampleRate(sampleRate);
        if (result == ASE_OK) publishRate(sampleRate);
        return result;
    }

    ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) override {
        if (m_entry.role == DriverRole::Tap) {
            if (numSources) *numSources = 1;
            if (clocks) {
                ASIOClockSource source = {};
                source.index = 0;
                source.associatedChannel = -1;
                source.associatedGroup = -1;
                source.isCurrentSource = ASIOTrue;
                strncpy_s(source.name, "Internal", _TRUNCATE);
                clocks[0] = source;
            }
            return ASE_OK;
        }
        if (!m_hwAsio) return ASE_NotPresent;
        return m_hwAsio->getClockSources(clocks, numSources);
    }

    ASIOError setClockSource(long reference) override {
        if (m_entry.role == DriverRole::Tap) {
            return reference == 0 ? ASE_OK : ASE_InvalidParameter;
        }
        if (!m_hwAsio) return ASE_NotPresent;
        return m_hwAsio->setClockSource(reference);
    }

    ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) override {
        if (m_entry.role == DriverRole::Tap) {
            const uint64_t position = m_samplePos.load(std::memory_order_relaxed);
            if (sPos) {
                sPos->hi = static_cast<unsigned long>(position >> 32);
                sPos->lo = static_cast<unsigned long>(position & 0xFFFFFFFFu);
            }
            if (tStamp) {
                const uint64_t now = QpcNanoseconds();
                tStamp->hi = static_cast<unsigned long>(now >> 32);
                tStamp->lo = static_cast<unsigned long>(now & 0xFFFFFFFFu);
            }
            return ASE_OK;
        }
        if (!m_hwAsio) return ASE_NotPresent;
        return m_hwAsio->getSamplePosition(sPos, tStamp);
    }

    ASIOError getChannelInfo(ASIOChannelInfo* info) override {
        if (!info) return ASE_InvalidParameter;
        if (m_entry.role == DriverRole::Tap) {
            if (!info->isInput || info->channel < 0 || info->channel >= 2) return ASE_InvalidParameter;
            info->isActive = ASIOTrue;
            info->channelGroup = 0;
            info->type = ASIOSTFloat32LSB;
            strncpy_s(info->name, info->channel == 0 ? "Tap 1" : "Tap 2", _TRUNCATE);
            return ASE_OK;
        }
        if (!m_hwAsio) return ASE_NotPresent;
        return m_hwAsio->getChannelInfo(info);
    }

    ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize,
                            ASIOCallbacks* callbacks) override {
        if (m_entry.role == DriverRole::Tap) {
            return createTapBuffers(bufferInfos, numChannels, bufferSize, callbacks);
        }
        if (!m_hwAsio || !callbacks) {
            setError("MyASIO Splitter: the hardware ASIO driver is not open.");
            return ASE_NotPresent;
        }
        if (m_buffersCreated) return ASE_InvalidMode;

        m_hostCallbacks = *callbacks;
        m_hwCallbacks.bufferSwitch = &MyAsioSplitter::onHardwareBufferSwitch;
        m_hwCallbacks.sampleRateDidChange = &MyAsioSplitter::onHardwareSampleRateDidChange;
        m_hwCallbacks.asioMessage = &MyAsioSplitter::onHardwareAsioMessage;
        m_hwCallbacks.bufferSwitchTimeInfo = &MyAsioSplitter::onHardwareBufferSwitchTimeInfo;

        s_activeMaster.store(this, std::memory_order_release);
        const ASIOError result =
            m_hwAsio->createBuffers(bufferInfos, numChannels, bufferSize, &m_hwCallbacks);
        if (result != ASE_OK) {
            s_activeMaster.store(nullptr, std::memory_order_release);
            m_hostCallbacks = {};
            copyHardwareError();
            return result;
        }

        m_buffersCreated = true;
        m_bufferSize = bufferSize;
        m_outCount = 0;
        for (long i = 0; i < numChannels && m_outCount < 2; ++i) {
            if (bufferInfos[i].isInput) continue;
            if (!bufferInfos[i].buffers[0] || !bufferInfos[i].buffers[1]) continue;

            ASIOChannelInfo info = {};
            info.channel = bufferInfos[i].channelNum;
            info.isInput = ASIOFalse;
            if (m_hwAsio->getChannelInfo(&info) != ASE_OK) info.type = ASIOSTFloat32LSB;

            MasterOutChannel& channel = m_outChannels[m_outCount];
            channel.buffers[0] = bufferInfos[i].buffers[0];
            channel.buffers[1] = bufferInfos[i].buffers[1];
            channel.type = info.type;
            ++m_outCount;
        }

        if (m_isPublisher && m_shm.block()) {
            ShmBlock* block = m_shm.block();
            block->masterStreaming.store(0, std::memory_order_release);
            block->bufferSize.store(static_cast<uint32_t>(bufferSize), std::memory_order_relaxed);
            block->outputChannels.store(static_cast<uint32_t>(m_outCount), std::memory_order_relaxed);
            ASIOSampleRate rate = 0.0;
            if (m_hwAsio->getSampleRate(&rate) == ASE_OK) publishRate(rate);
            block->generation.fetch_add(1, std::memory_order_release);
        }
        return ASE_OK;
    }

    ASIOError disposeBuffers() override {
        if (m_entry.role == DriverRole::Tap) {
            stopTapWorker();
            freeTapBuffers();
            m_hostCallbacks = {};
            m_tapUseTimeInfo = false;
            m_buffersCreated = false;
            return ASE_OK;
        }

        if (m_isPublisher && m_shm.block()) {
            m_shm.block()->masterStreaming.store(0, std::memory_order_release);
        }
        m_started = false;

        ASIOError result = ASE_OK;
        if (m_hwAsio && m_buffersCreated) {
            m_hwAsio->stop();
            result = m_hwAsio->disposeBuffers();
        }

        s_activeMaster.store(nullptr, std::memory_order_release);
        m_hostCallbacks = {};
        m_outCount = 0;
        m_buffersCreated = false;
        return result;
    }

    ASIOError controlPanel() override {
        if (m_entry.role == DriverRole::Tap) return ASE_NotPresent;
        if (!m_hwAsio) return ASE_NotPresent;

        if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) return pickNewHardwareTarget();
        return m_hwAsio->controlPanel();
    }

    ASIOError future(long selector, void* opt) override {
        if (m_entry.role == DriverRole::Tap) {
            switch (selector) {
                case kAsioEngineVersion:
                    return 2;
                case kAsioCanTimeInfo:
                    return ASE_SUCCESS;
                case kAsioSelectorSupported:
                    switch (static_cast<long>(reinterpret_cast<intptr_t>(opt))) {
                        case kAsioEngineVersion:
                        case kAsioSelectorSupported:
                        case kAsioSupportsTimeInfo:
                        case kAsioResetRequest:
                        case kAsioBufferSizeChange:
                        case kAsioLatenciesChanged:
                            return ASIOTrue;
                        default:
                            return ASIOFalse;
                    }
                default:
                    return ASE_NotPresent;
            }
        }
        if (!m_hwAsio) return ASE_NotPresent;
        if (selector == kAsioSelectorSupported &&
            static_cast<long>(reinterpret_cast<intptr_t>(opt)) == kAsioResetRequest) {
            return ASIOTrue;
        }
        return m_hwAsio->future(selector, opt);
    }

    ASIOError outputReady() override {
        if (m_hwAsio) return m_hwAsio->outputReady();
        return ASE_NotPresent;
    }

private:
    static void onHardwareBufferSwitch(long doubleBufferIndex, ASIOBool directProcess) {
        MyAsioSplitter* self = s_activeMaster.load(std::memory_order_acquire);
        if (!self) return;
        if (self->m_hostCallbacks.bufferSwitch) {
            self->m_hostCallbacks.bufferSwitch(doubleBufferIndex, directProcess);
        }
        self->publishTap(doubleBufferIndex);
    }

    static ASIOTime* onHardwareBufferSwitchTimeInfo(ASIOTime* params, long doubleBufferIndex,
                                                    ASIOBool directProcess) {
        MyAsioSplitter* self = s_activeMaster.load(std::memory_order_acquire);
        if (!self) return params;

        ASIOTime* result = params;
        if (self->m_hostCallbacks.bufferSwitchTimeInfo) {
            result = self->m_hostCallbacks.bufferSwitchTimeInfo(params, doubleBufferIndex, directProcess);
        } else if (self->m_hostCallbacks.bufferSwitch) {
            self->m_hostCallbacks.bufferSwitch(doubleBufferIndex, directProcess);
        }
        self->publishTap(doubleBufferIndex);
        return result;
    }

    static void onHardwareSampleRateDidChange(ASIOSampleRate sampleRate) {
        MyAsioSplitter* self = s_activeMaster.load(std::memory_order_acquire);
        if (!self) return;
        self->publishRate(sampleRate);
        if (self->m_hostCallbacks.sampleRateDidChange) {
            self->m_hostCallbacks.sampleRateDidChange(sampleRate);
        }
    }

    static long onHardwareAsioMessage(long selector, long value, void* message, double* opt) {
        MyAsioSplitter* self = s_activeMaster.load(std::memory_order_acquire);
        if (self && self->m_hostCallbacks.asioMessage) {
            return self->m_hostCallbacks.asioMessage(selector, value, message, opt);
        }
        return 0;
    }

    void publishTap(long doubleBufferIndex) {
        ShmBlock* block = m_shm.block();
        if (!block || !m_isPublisher || m_outCount <= 0 || m_bufferSize <= 0) return;
        if (block->tapClients.load(std::memory_order_acquire) == 0) return;

        const void* left = m_outChannels[0].buffers[doubleBufferIndex];
        const void* right = m_outCount > 1 ? m_outChannels[1].buffers[doubleBufferIndex] : left;
        if (!left || !right) return;

        const long frames = m_bufferSize;
        const uint64_t write = block->writeIdx.load(std::memory_order_relaxed);
        const uint64_t read = block->readIdx.load(std::memory_order_acquire);
        uint64_t used = write >= read ? write - read : 0;
        if (used > kRingFrames) used = kRingFrames;
        const uint64_t space = kRingFrames - used;
        const long count = static_cast<uint64_t>(frames) < space ? frames : static_cast<long>(space);
        if (count <= 0) return;

        const uint64_t pos = write & kRingMask;
        const long part1 = static_cast<long>(std::min<uint64_t>(static_cast<uint64_t>(count), kRingFrames - pos));
        const long part2 = count - part1;

        const ASIOSampleType leftType = m_outChannels[0].type;
        const ASIOSampleType rightType = m_outCount > 1 ? m_outChannels[1].type : leftType;

        WriteChannelSamples(leftType, left, 0, &block->samples[pos * 2], part1);
        WriteChannelSamples(rightType, right, 0, &block->samples[pos * 2 + 1], part1);
        if (part2 > 0) {
            WriteChannelSamples(leftType, left, part1, &block->samples[0], part2);
            WriteChannelSamples(rightType, right, part1, &block->samples[1], part2);
        }

        block->writeIdx.store(write + static_cast<uint64_t>(count), std::memory_order_release);
    }

    bool claimMaster() {
        ShmBlock* block = m_shm.block();
        if (!block) return false;

        const uint32_t pid = static_cast<uint32_t>(GetCurrentProcessId());
        for (int attempt = 0; attempt < 4; ++attempt) {
            uint32_t current = block->masterPid.load(std::memory_order_acquire);
            if (current == pid) return true;

            if (current != 0) {
                if (ProcessAlive(current)) return false;
                uint32_t expected = current;
                block->masterPid.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
                continue;
            }

            if (block->masterPid.compare_exchange_strong(current, pid, std::memory_order_acq_rel)) {
                block->masterStreaming.store(0, std::memory_order_relaxed);
                block->writeIdx.store(0, std::memory_order_relaxed);
                block->readIdx.store(0, std::memory_order_relaxed);
                block->generation.fetch_add(1, std::memory_order_acq_rel);
                return true;
            }
        }
        return false;
    }

    void releaseMaster() {
        ShmBlock* block = m_shm.block();
        if (!block || !m_isPublisher) return;

        block->masterStreaming.store(0, std::memory_order_release);
        uint32_t expected = static_cast<uint32_t>(GetCurrentProcessId());
        block->masterPid.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
        m_isPublisher = false;
    }

    void publishRate(double rate) {
        if (m_isPublisher && m_shm.block()) {
            m_shm.block()->sampleRateBits.store(SampleRateToBits(rate), std::memory_order_release);
        }
    }

    ASIOBool initTap() {
        m_shm.Attach();
        ShmBlock* block = m_shm.block();
        if (!block) return ASIOTrue;

        if (!m_tapRegistered) {
            block->tapClients.fetch_add(1, std::memory_order_acq_rel);
            m_tapRegistered = true;
        }

        uint32_t pid = block->masterPid.load(std::memory_order_acquire);
        if (pid != 0 && !ProcessAlive(pid)) {
            uint32_t expected = pid;
            if (block->masterPid.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) {
                block->masterStreaming.store(0, std::memory_order_release);
            }
        }

        const double rate = SampleRateFromBits(block->sampleRateBits.load(std::memory_order_acquire));
        if (rate >= 8000.0 && rate <= 384000.0) m_sampleRate = rate;

        const uint32_t bufferSize = block->bufferSize.load(std::memory_order_acquire);
        if (bufferSize > 0 && bufferSize <= kRingFrames) {
            m_tapFallbackBuffer = static_cast<long>(bufferSize);
        }
        return ASIOTrue;
    }

    bool tapMasterPresent() const {
        ShmBlock* block = m_shm.block();
        return block && block->masterPid.load(std::memory_order_acquire) != 0;
    }

    double liveTapRate() const {
        ShmBlock* block = m_shm.block();
        if (block) {
            const double rate = SampleRateFromBits(block->sampleRateBits.load(std::memory_order_acquire));
            if (rate >= 8000.0 && rate <= 384000.0) return rate;
        }
        return m_sampleRate;
    }

    long liveTapBufferSize() const {
        ShmBlock* block = m_shm.block();
        if (block) {
            const uint32_t size = block->bufferSize.load(std::memory_order_acquire);
            if (size > 0 && size <= kRingFrames) return static_cast<long>(size);
        }
        return m_tapFallbackBuffer;
    }

    ASIOError createTapBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize,
                               ASIOCallbacks* callbacks) {
        if (!bufferInfos || !callbacks || numChannels <= 0 || bufferSize <= 0) {
            return ASE_InvalidParameter;
        }
        if (m_buffersCreated) return ASE_InvalidMode;

        freeTapBuffers();

        long channels = 0;
        for (long i = 0; i < numChannels; ++i) {
            if (!bufferInfos[i].isInput) continue;
            const long ch = bufferInfos[i].channelNum;
            if (ch < 0 || ch >= 2 || m_tapActive[ch]) {
                freeTapBuffers();
                return ASE_InvalidParameter;
            }
            const size_t bytes = static_cast<size_t>(bufferSize) * sizeof(float);
            m_tapBuffer[ch][0] = static_cast<float*>(_aligned_malloc(bytes, 64));
            m_tapBuffer[ch][1] = static_cast<float*>(_aligned_malloc(bytes, 64));
            if (!m_tapBuffer[ch][0] || !m_tapBuffer[ch][1]) {
                freeTapBuffers();
                return ASE_NoMemory;
            }
            std::memset(m_tapBuffer[ch][0], 0, bytes);
            std::memset(m_tapBuffer[ch][1], 0, bytes);
            bufferInfos[i].buffers[0] = m_tapBuffer[ch][0];
            bufferInfos[i].buffers[1] = m_tapBuffer[ch][1];
            m_tapActive[ch] = true;
            ++channels;
        }
        if (channels == 0) return ASE_InvalidParameter;

        m_tapBufferSize = bufferSize;
        m_hostCallbacks = *callbacks;
        m_buffersCreated = true;
        m_tapUseTimeInfo = m_hostCallbacks.asioMessage &&
                           m_hostCallbacks.asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr) == 1;

        if (ShmBlock* block = m_shm.block()) {
            block->readIdx.store(block->writeIdx.load(std::memory_order_acquire),
                                 std::memory_order_release);
        }
        return ASE_OK;
    }

    void freeTapBuffers() {
        for (int ch = 0; ch < 2; ++ch) {
            m_tapActive[ch] = false;
            for (float*& buffer : m_tapBuffer[ch]) {
                if (buffer) {
                    _aligned_free(buffer);
                    buffer = nullptr;
                }
            }
        }
        m_tapBufferSize = 0;
    }

    ASIOError startTapWorker() {
        if (!m_buffersCreated || (!m_tapActive[0] && !m_tapActive[1]) ||
            (!m_hostCallbacks.bufferSwitch && !m_hostCallbacks.bufferSwitchTimeInfo)) {
            return ASE_InvalidMode;
        }
        if (m_worker.joinable()) return ASE_OK;

        m_workerWakeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!m_workerWakeEvent) return ASE_NoMemory;

        m_workerStop.store(false, std::memory_order_relaxed);
        try {
            m_worker = std::thread(&MyAsioSplitter::slaveLoop, this);
        } catch (...) {
            CloseHandle(m_workerWakeEvent);
            m_workerWakeEvent = nullptr;
            return ASE_NoMemory;
        }
        return ASE_OK;
    }

    void stopTapWorker() {
        m_workerStop.store(true, std::memory_order_relaxed);
        if (m_workerWakeEvent) SetEvent(m_workerWakeEvent);
        if (m_worker.joinable()) m_worker.join();
        if (m_workerWakeEvent) {
            CloseHandle(m_workerWakeEvent);
            m_workerWakeEvent = nullptr;
        }
    }

    void slaveLoop() {
        DWORD taskIndex = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                              TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

        const double qpcHz = static_cast<double>(QpcFrequency());
        ShmBlock* block = m_shm.block();
        uint32_t lastGeneration = block ? block->generation.load(std::memory_order_acquire) : 0;

        bool waitingForFill = true;
        bool wasAudioActive = false;
        float lastSample[2] = {0.0f, 0.0f};
        long doubleBuffer = 0;
        bool firstTick = true;
        double lastRate = liveTapRate();

        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);
        double targetQpc = static_cast<double>(now.QuadPart);

        while (!m_workerStop.load(std::memory_order_relaxed)) {
            bool streaming = false;
            double rate = m_sampleRate;
            uint64_t write = 0;
            long masterBuf = 0;

            if (block) {
                const uint32_t generation = block->generation.load(std::memory_order_acquire);
                streaming = block->masterStreaming.load(std::memory_order_acquire) != 0;
                masterBuf = static_cast<long>(block->bufferSize.load(std::memory_order_acquire));
                const double sharedRate =
                    SampleRateFromBits(block->sampleRateBits.load(std::memory_order_acquire));
                if (sharedRate >= 8000.0 && sharedRate <= 384000.0) rate = sharedRate;
                write = block->writeIdx.load(std::memory_order_acquire);

                const uint64_t read = block->readIdx.load(std::memory_order_relaxed);
                if (generation != lastGeneration || write < read || write - read > kRingFrames) {
                    block->readIdx.store(write, std::memory_order_release);
                    lastGeneration = generation;
                    waitingForFill = true;
                }
            }

            if (streaming && std::fabs(rate - lastRate) >= 0.5) {
                lastRate = rate;
                m_sampleRate = rate;
                if (m_hostCallbacks.sampleRateDidChange) {
                    m_hostCallbacks.sampleRateDidChange(rate);
                }
            }

            if (!streaming) waitingForFill = true;

            uint64_t read = block ? block->readIdx.load(std::memory_order_relaxed) : 0;
            uint64_t available = write >= read ? write - read : 0;
            if (available > kRingFrames) available = 0;

            const long frames = m_tapBufferSize;
            const long minTimeFrames = static_cast<long>((rate > 0.0 ? rate : 48000.0) * 0.008);
            long targetFill = frames * 2;
            if (masterBuf * 2 > targetFill) targetFill = masterBuf * 2;
            if (minTimeFrames > targetFill) targetFill = minTimeFrames;
            const long maxFill = static_cast<long>(kRingFrames / 2);
            if (targetFill > maxFill) targetFill = maxFill;
            if (targetFill < frames) targetFill = frames;

            if (streaming && waitingForFill && available >= static_cast<uint64_t>(targetFill)) {
                waitingForFill = false;
            }

            if (!firstTick) {
                double adjust = 1.0;
                if (!waitingForFill && frames > 0) {
                    double errorRatio = (static_cast<double>(available) - static_cast<double>(targetFill)) /
                                        static_cast<double>(frames);
                    if (errorRatio > 1.0) errorRatio = 1.0;
                    if (errorRatio < -1.0) errorRatio = -1.0;
                    adjust = 1.0 - 0.04 * errorRatio;
                }

                const double periodTicks =
                    (static_cast<double>(frames) / (rate > 0.0 ? rate : 48000.0)) * qpcHz;
                targetQpc += periodTicks * adjust;
                QueryPerformanceCounter(&now);
                double waitTicks = targetQpc - static_cast<double>(now.QuadPart);
                if (waitTicks < -2.0 * periodTicks) {
                    targetQpc = static_cast<double>(now.QuadPart);
                    waitTicks = 0.0;
                }

                if (waitTicks > 0.0) {
                    const LONGLONG due100ns = static_cast<LONGLONG>(waitTicks * 10000000.0 / qpcHz);
                    if (due100ns >= 1) {
                        LARGE_INTEGER due = {};
                        due.QuadPart = -due100ns;
                        if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                            Sleep(1);
                        } else {
                            HANDLE handles[2] = {timer, m_workerWakeEvent};
                            const DWORD waitResult =
                                WaitForMultipleObjects(m_workerWakeEvent ? 2 : 1, handles, FALSE, INFINITE);
                            if (m_workerWakeEvent && waitResult == WAIT_OBJECT_0 + 1) break;
                            if (waitResult != WAIT_OBJECT_0 &&
                                !m_workerStop.load(std::memory_order_relaxed)) {
                                Sleep(1);
                            }
                        }
                    }
                }
            }
            firstTick = false;

            if (m_workerStop.load(std::memory_order_relaxed)) break;

            if (block) {
                write = block->writeIdx.load(std::memory_order_acquire);
                read = block->readIdx.load(std::memory_order_relaxed);
                const uint64_t maxLag =
                    static_cast<uint64_t>(targetFill) + static_cast<uint64_t>(frames) * 8;
                if (write < read || write - read > kRingFrames) {
                    block->readIdx.store(write, std::memory_order_release);
                    waitingForFill = true;
                    read = write;
                } else if (!waitingForFill && (write - read) > maxLag) {
                    read = write - static_cast<uint64_t>(targetFill);
                    block->readIdx.store(read, std::memory_order_release);
                }
                available = write - read;
            } else {
                available = 0;
            }

            if (streaming && waitingForFill && available >= static_cast<uint64_t>(targetFill)) {
                waitingForFill = false;
            }
            if (available < static_cast<uint64_t>(frames)) waitingForFill = true;

            const bool haveData = streaming && !waitingForFill && block && available >= static_cast<uint64_t>(frames);
            const long fadeLen = frames < 32L ? frames : 32L;

            for (long ch = 0; ch < 2; ++ch) {
                if (!m_tapActive[ch]) continue;
                float* destination = m_tapBuffer[ch][doubleBuffer];

                if (!haveData) {
                    std::memset(destination, 0, static_cast<size_t>(frames) * sizeof(float));
                    if (wasAudioActive && lastSample[ch] != 0.0f) {
                        for (long i = 0; i < fadeLen; ++i) {
                            const float gain = 1.0f - static_cast<float>(i + 1) / static_cast<float>(fadeLen);
                            destination[i] = lastSample[ch] * gain;
                        }
                    }
                    lastSample[ch] = 0.0f;
                    continue;
                }

                const uint64_t pos = read & kRingMask;
                const long part1 =
                    static_cast<long>(std::min<uint64_t>(static_cast<uint64_t>(frames), kRingFrames - pos));
                const long part2 = frames - part1;

                const float* src1 = &block->samples[pos * 2 + static_cast<size_t>(ch)];
                for (long i = 0; i < part1; ++i) {
                    destination[i] = src1[i * 2];
                }
                if (part2 > 0) {
                    const float* src2 = &block->samples[static_cast<size_t>(ch)];
                    for (long i = 0; i < part2; ++i) {
                        destination[part1 + i] = src2[i * 2];
                    }
                }

                if (!wasAudioActive) {
                    for (long i = 0; i < fadeLen; ++i) {
                        destination[i] *= static_cast<float>(i + 1) / static_cast<float>(fadeLen);
                    }
                }
                lastSample[ch] = destination[frames - 1];
            }
            wasAudioActive = haveData;

            if (haveData) {
                block->readIdx.store(read + static_cast<uint64_t>(frames), std::memory_order_release);
            } else if (block && !streaming) {
                block->readIdx.store(write, std::memory_order_release);
            }

            const uint64_t position = m_samplePos.load(std::memory_order_relaxed);
            const uint64_t timeStamp = QpcNanoseconds();
            ASIOTime time = {};
            time.timeInfo.speed = 1.0;
            time.timeInfo.samplePosition.hi = static_cast<unsigned long>(position >> 32);
            time.timeInfo.samplePosition.lo = static_cast<unsigned long>(position & 0xFFFFFFFFu);
            time.timeInfo.sampleRate = rate;
            time.timeInfo.flags =
                kSystemTimeValid | kSamplePositionValid | kSampleRateValid | kSpeedValid;
            time.timeInfo.systemTime.hi = static_cast<unsigned long>(timeStamp >> 32);
            time.timeInfo.systemTime.lo = static_cast<unsigned long>(timeStamp & 0xFFFFFFFFu);
            time.timeCode.speed = 1.0;

            if (m_tapUseTimeInfo && m_hostCallbacks.bufferSwitchTimeInfo) {
                m_hostCallbacks.bufferSwitchTimeInfo(&time, doubleBuffer, ASIOFalse);
            } else if (m_hostCallbacks.bufferSwitch) {
                m_hostCallbacks.bufferSwitch(doubleBuffer, ASIOFalse);
            }

            m_samplePos.fetch_add(static_cast<uint64_t>(frames), std::memory_order_relaxed);
            doubleBuffer ^= 1;
        }

        if (timer) CloseHandle(timer);
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    }

    bool openHardware(REFCLSID clsid, void* sysHandle) {
        IASIO* hardware = nullptr;
        const HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid,
                                            reinterpret_cast<void**>(&hardware));
        if (FAILED(hr) || !hardware) {
            setError("MyASIO Splitter: the selected hardware ASIO driver could not be created "
                     "(is it a 64-bit driver?).");
            return false;
        }
        if (!hardware->init(sysHandle)) {
            char reason[128] = {};
            hardware->getErrorMessage(reason);
            setError(reason[0] ? reason : "MyASIO Splitter: hardware ASIO init failed.");
            hardware->Release();
            return false;
        }

        m_hwAsio = hardware;
        return true;
    }

    ASIOError pickNewHardwareTarget() {
        const std::vector<AsioDriverInfo> drivers =
            EnumerateAsioDrivers(kDrivers[0].clsid, kDrivers[1].clsid);
        if (drivers.empty()) return ASE_NotPresent;

        CLSID current = {};
        const bool haveCurrent = LoadSelectedClsid(current);
        const int index = ShowAsioDevicePicker(drivers, haveCurrent ? &current : nullptr);
        if (index < 0) return ASE_OK;

        const CLSID chosen = drivers[static_cast<size_t>(index)].clsid;
        if (haveCurrent && IsEqualGUID(chosen, current)) return ASE_OK;

        SaveSelectedClsid(chosen);
        if (m_hostCallbacks.asioMessage) {
            m_hostCallbacks.asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
        }
        return ASE_OK;
    }

    void shutdown() {
        if (m_entry.role == DriverRole::Tap) {
            stopTapWorker();
            freeTapBuffers();
            m_hostCallbacks = {};
            m_tapUseTimeInfo = false;
            m_buffersCreated = false;
            m_samplePos.store(0, std::memory_order_relaxed);
            if (m_tapRegistered && m_shm.block()) {
                m_shm.block()->tapClients.fetch_sub(1, std::memory_order_acq_rel);
            }
            m_tapRegistered = false;
            m_shm.Detach();
            setError("MyASIO Splitter: no error.");
            return;
        }

        if (m_isPublisher && m_shm.block()) {
            m_shm.block()->masterStreaming.store(0, std::memory_order_release);
        }
        m_started = false;

        if (m_buffersCreated && m_hwAsio) {
            m_hwAsio->stop();
            m_hwAsio->disposeBuffers();
            m_buffersCreated = false;
        }

        s_activeMaster.store(nullptr, std::memory_order_release);
        m_hostCallbacks = {};
        m_outCount = 0;

        releaseMaster();

        if (m_hwAsio) {
            m_hwAsio->Release();
            m_hwAsio = nullptr;
        }
        m_shm.Detach();
        setError("MyASIO Splitter: no error.");
    }

    void setError(const char* message) { strncpy_s(m_lastError, message, _TRUNCATE); }

    void copyHardwareError() {
        if (!m_hwAsio) return;
        char reason[128] = {};
        m_hwAsio->getErrorMessage(reason);
        if (reason[0]) setError(reason);
    }

    const DriverEntry& m_entry;
    std::atomic<long> m_ref{0};
    IASIO* m_hwAsio = nullptr;
    ASIOCallbacks m_hostCallbacks = {};
    ASIOCallbacks m_hwCallbacks = {};
    bool m_buffersCreated = false;
    bool m_started = false;
    long m_bufferSize = 0;
    ASIOSampleRate m_sampleRate = 48000.0;
    char m_lastError[128] = "MyASIO Splitter: no error.";

    ShmMapping m_shm;
    bool m_isPublisher = false;
    bool m_tapRegistered = false;
    MasterOutChannel m_outChannels[2] = {};
    long m_outCount = 0;

    float* m_tapBuffer[2][2] = {};
    bool m_tapActive[2] = {};
    bool m_tapUseTimeInfo = false;
    long m_tapBufferSize = 0;
    long m_tapFallbackBuffer = 512;
    std::thread m_worker;
    std::atomic<bool> m_workerStop{false};
    HANDLE m_workerWakeEvent = nullptr;
    std::atomic<uint64_t> m_samplePos{0};

    static std::atomic<MyAsioSplitter*> s_activeMaster;
};

std::atomic<MyAsioSplitter*> MyAsioSplitter::s_activeMaster{nullptr};

static MyAsioSplitter& DriverInstance(const DriverEntry& entry) {
    static MyAsioSplitter* master = new MyAsioSplitter(kDrivers[0]);
    static MyAsioSplitter* tap = new MyAsioSplitter(kDrivers[1]);
    return entry.role == DriverRole::Tap ? *tap : *master;
}

class MyClassFactory final : public IClassFactory {
public:
    explicit MyClassFactory(const DriverEntry& entry) : m_entry(entry) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory || IsEqualGUID(riid, m_entry.clsid)) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(m_ref.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const long refs = m_ref.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (refs == 0) delete this;
        return static_cast<ULONG>(refs < 0 ? 0 : refs);
    }

    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        if (!(riid == IID_IUnknown || IsEqualGUID(riid, m_entry.clsid))) return E_NOINTERFACE;

        MyAsioSplitter& driver = DriverInstance(m_entry);
        driver.AddRef();
        *ppv = static_cast<IASIO*>(&driver);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        if (lock) {
            g_serverLocks.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_serverLocks.fetch_sub(1, std::memory_order_relaxed);
        }
        return S_OK;
    }

private:
    const DriverEntry& m_entry;
    std::atomic<long> m_ref{1};
};

static HRESULT SetRegString(HKEY root, const wchar_t* subKey, const wchar_t* value, const wchar_t* data) {
    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(root, subKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);

    status = RegSetValueExW(key, value, 0, REG_SZ, reinterpret_cast<const BYTE*>(data),
                            static_cast<DWORD>((std::wcslen(data) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return HRESULT_FROM_WIN32(status);
}

static HRESULT RegisterDriver(const DriverEntry& entry, const wchar_t* modulePath) {
    const std::wstring clsidKey = std::wstring(L"CLSID\\") + entry.clsidString;
    const std::wstring inprocKey = clsidKey + L"\\InprocServer32";

    HRESULT hr = SetRegString(HKEY_CLASSES_ROOT, inprocKey.c_str(), nullptr, modulePath);
    if (SUCCEEDED(hr)) hr = SetRegString(HKEY_CLASSES_ROOT, inprocKey.c_str(), L"ThreadingModel", L"Both");
    if (SUCCEEDED(hr)) hr = SetRegString(HKEY_LOCAL_MACHINE, entry.asioSubkey, L"CLSID", entry.clsidString);
    if (SUCCEEDED(hr)) hr = SetRegString(HKEY_LOCAL_MACHINE, entry.asioSubkey, L"Description", entry.displayName);
    return hr;
}

extern "C" STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    const DriverEntry* entry = FindDriver(rclsid);
    if (!entry) return CLASS_E_CLASSNOTAVAILABLE;

    auto* factory = new (std::nothrow) MyClassFactory(*entry);
    if (!factory) return E_OUTOFMEMORY;

    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

extern "C" STDAPI DllCanUnloadNow() {
    return (g_liveObjects.load(std::memory_order_acquire) == 0 &&
            g_serverLocks.load(std::memory_order_acquire) == 0)
               ? S_OK
               : S_FALSE;
}

extern "C" STDAPI DllRegisterServer() {
    wchar_t modulePath[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(g_hInstance, modulePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return HRESULT_FROM_WIN32(GetLastError());

    for (const DriverEntry& entry : kDrivers) {
        const HRESULT hr = RegisterDriver(entry, modulePath);
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

extern "C" STDAPI DllUnregisterServer() {
    for (const DriverEntry& entry : kDrivers) {
        const std::wstring clsidKey = std::wstring(L"CLSID\\") + entry.clsidString;
        RegDeleteTreeW(HKEY_CLASSES_ROOT, clsidKey.c_str());
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, entry.asioSubkey);
    }
    return S_OK;
}

BOOL APIENTRY DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) g_hInstance = instance;
    return TRUE;
}