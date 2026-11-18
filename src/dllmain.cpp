#include <windows.h>
#include <unknwn.h>

#include <avrt.h>
#include <malloc.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "asio_abi.h"
#include "config.h"
#include "shm_ring.h"

// Master entry (owns the hardware ASIO device).
// {7C9E5B6A-3F2D-4C81-9E4A-5D6F8B1C2A30}
static const CLSID CLSID_MyAsioSplitter =
    {0x7c9e5b6a, 0x3f2d, 0x4c81, {0x9e, 0x4a, 0x5d, 0x6f, 0x8b, 0x1c, 0x2a, 0x30}};

// Tap entry (always slave capture; never touches hardware).
// {9A4F2D18-6B3C-4E57-A1D0-2C8E7F4B9A61}
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
static LONG g_driverRefs = 0;
static LONG g_serverLocks = 0;

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
           std::fabs(rate - 88200.0) < 0.5 || std::fabs(rate - 96000.0) < 0.5;
}

static float ClampSample(float value) {
    return value < -1.0f ? -1.0f : (value > 1.0f ? 1.0f : value);
}

static float ReadSampleAsFloat(ASIOSampleType type, const void* buffer, long index) {
    switch (type) {
        case ASIOSTInt16LSB:
            return static_cast<float>(static_cast<const int16_t*>(buffer)[index]) / 32768.0f;
        case ASIOSTInt24LSB: {
            // ASIOSTInt24LSB is tightly packed 3 bytes, little-endian, NOT padded int32.
            const auto* bytes = static_cast<const uint8_t*>(buffer) + static_cast<size_t>(index) * 3;
            int32_t value = static_cast<int32_t>(bytes[0]) | (static_cast<int32_t>(bytes[1]) << 8) |
                            (static_cast<int32_t>(bytes[2]) << 16);
            if (value & 0x800000) value |= ~0xFFFFFF;
            return static_cast<float>(value) / 8388608.0f;
        }
        case ASIOSTInt32LSB:
            return static_cast<float>(static_cast<const int32_t*>(buffer)[index]) / 2147483648.0f;
        case ASIOSTFloat32LSB:
            return ClampSample(static_cast<const float*>(buffer)[index]);
        default:
            return 0.0f;
    }
}

static uint64_t QpcNanoseconds() {
    LARGE_INTEGER counter = {};
    LARGE_INTEGER frequency = {};
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    const uint64_t ticks = static_cast<uint64_t>(counter.QuadPart);
    const uint64_t hz = static_cast<uint64_t>(frequency.QuadPart);
    if (hz == 0) return 0;
    return (ticks / hz) * 1000000000ULL + ((ticks % hz) * 1000000000ULL) / hz;
}

class MyAsioSplitter final : public IASIO {
public:
    explicit MyAsioSplitter(const DriverEntry& entry) : m_entry(entry) {}

    MyAsioSplitter(const MyAsioSplitter&) = delete;
    MyAsioSplitter& operator=(const MyAsioSplitter&) = delete;

    // ---- IUnknown ----
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

    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&g_driverRefs)); }

    ULONG STDMETHODCALLTYPE Release() override {
        const LONG refs = InterlockedDecrement(&g_driverRefs);
        if (refs == 0) shutdown();
        return static_cast<ULONG>(refs < 0 ? 0 : refs);
    }

    // ---- IASIO (21 methods; with IUnknown that is exactly 24 vtable slots) ----
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
        if (m_hwAsio) return m_hwAsio->getChannels(numInputChannels, numOutputChannels);

        if (numInputChannels) *numInputChannels = 2;
        if (numOutputChannels) *numOutputChannels = 0;
        return ASE_OK;
    }

    ASIOError getLatencies(long* inputLatency, long* outputLatency) override {
        if (m_hwAsio) return m_hwAsio->getLatencies(inputLatency, outputLatency);

        if (inputLatency) *inputLatency = 0;
        if (outputLatency) *outputLatency = 0;
        return ASE_OK;
    }

    ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) override {
        if (m_hwAsio) return m_hwAsio->getBufferSize(minSize, maxSize, preferredSize, granularity);

        const long size = m_entry.role == DriverRole::Tap ? liveTapBufferSize() : 512;
        if (minSize) *minSize = size;
        if (maxSize) *maxSize = size;
        if (preferredSize) *preferredSize = size;
        if (granularity) *granularity = 0;
        return ASE_OK;
    }

    ASIOError canSampleRate(ASIOSampleRate sampleRate) override {
        if (m_hwAsio) return m_hwAsio->canSampleRate(sampleRate);

        if (m_entry.role == DriverRole::Tap) {
            if (tapMasterPresent()) {
                return std::fabs(sampleRate - liveTapRate()) < 0.5 ? ASE_OK : ASE_NoClock;
            }
            return IsCommonSampleRate(sampleRate) ? ASE_OK : ASE_NoClock;
        }
        return IsCommonSampleRate(sampleRate) ? ASE_OK : ASE_NoClock;
    }

    ASIOError getSampleRate(ASIOSampleRate* sampleRate) override {
        if (m_hwAsio) return m_hwAsio->getSampleRate(sampleRate);

        if (sampleRate) *sampleRate = (m_entry.role == DriverRole::Tap) ? liveTapRate() : m_sampleRate;
        return ASE_OK;
    }

    ASIOError setSampleRate(ASIOSampleRate sampleRate) override {
        if (m_hwAsio) {
            const ASIOError result = m_hwAsio->setSampleRate(sampleRate);
            if (result == ASE_OK) publishRate(sampleRate);
            return result;
        }

        if (m_entry.role == DriverRole::Tap && tapMasterPresent()) {
            return std::fabs(sampleRate - liveTapRate()) < 0.5 ? ASE_OK : ASE_NoClock;
        }
        if (!IsCommonSampleRate(sampleRate)) return ASE_NoClock;
        m_sampleRate = sampleRate;
        return ASE_OK;
    }

    ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) override {
        if (m_hwAsio) return m_hwAsio->getClockSources(clocks, numSources);
        if (m_entry.role != DriverRole::Tap) {
            if (numSources) *numSources = 0;
            return ASE_OK;
        }

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

    ASIOError setClockSource(long reference) override {
        if (m_hwAsio) return m_hwAsio->setClockSource(reference);
        if (m_entry.role == DriverRole::Tap && reference == 0) return ASE_OK;
        return ASE_InvalidParameter;
    }

    ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) override {
        if (m_hwAsio) return m_hwAsio->getSamplePosition(sPos, tStamp);
        if (m_entry.role != DriverRole::Tap) {
            if (sPos) {
                sPos->hi = 0;
                sPos->lo = 0;
            }
            if (tStamp) {
                tStamp->hi = 0;
                tStamp->lo = 0;
            }
            return ASE_OK;
        }

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

    ASIOError getChannelInfo(ASIOChannelInfo* info) override {
        if (m_hwAsio) return m_hwAsio->getChannelInfo(info);
        if (!info) return ASE_InvalidParameter;

        if (m_entry.role == DriverRole::Tap) {
            if (!info->isInput || info->channel < 0 || info->channel >= 2) return ASE_InvalidParameter;
            info->isActive = ASIOTrue;
            info->channelGroup = 0;
            info->type = ASIOSTFloat32LSB;
            strncpy_s(info->name, info->channel == 0 ? "Tap 1" : "Tap 2", _TRUNCATE);
            return ASE_OK;
        }

        info->isActive = ASIOFalse;
        info->channelGroup = 0;
        info->type = ASIOSTFloat32LSB;
        std::memcpy(info->name, "Channel", sizeof("Channel"));
        return ASE_OK;
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
        }
        return ASE_OK;
    }

    ASIOError disposeBuffers() override {
        if (m_entry.role == DriverRole::Tap) {
            stopTapWorker();
            freeTapBuffers();
            m_hostCallbacks = {};
            m_buffersCreated = false;
            return ASE_OK;
        }

        s_activeMaster.store(nullptr, std::memory_order_release);
        m_hostCallbacks = {};
        if (m_isPublisher && m_shm.block()) {
            m_shm.block()->masterStreaming.store(0, std::memory_order_release);
        }
        m_outCount = 0;

        if (!m_hwAsio || !m_buffersCreated) {
            m_buffersCreated = false;
            return ASE_OK;
        }

        m_started = false;
        m_hwAsio->stop();
        const ASIOError result = m_hwAsio->disposeBuffers();
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
        if (m_hwAsio) {
            if (selector == kAsioSelectorSupported &&
                static_cast<long>(reinterpret_cast<intptr_t>(opt)) == kAsioResetRequest) {
                return ASIOTrue;
            }
            return m_hwAsio->future(selector, opt);
        }

        if (m_entry.role == DriverRole::Tap) {
            switch (selector) {
                case kAsioEngineVersion:
                    return 2;
                case kAsioCanTimeInfo:
                    return ASE_SUCCESS;
                case kAsioCanTimeCode:
                case kAsioCanInputMonitor:
                    return ASE_NotPresent;
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

        switch (selector) {
            case kAsioEngineVersion:
                return 2;
            case kAsioSelectorSupported:
                switch (static_cast<long>(reinterpret_cast<intptr_t>(opt))) {
                    case kAsioResetRequest:
                        return ASIOTrue;
                    default:
                        return ASIOFalse;
                }
            default:
                return ASE_NotPresent;
        }
    }

    ASIOError outputReady() override {
        if (m_hwAsio) return m_hwAsio->outputReady();
        return ASE_NotPresent;
    }

private:
    // ---- hardware -> host trampolines (ASIOCallbacks has no userData) ----
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

    // ---- tap publisher (master) ----
    void publishTap(long doubleBufferIndex) {
        ShmBlock* block = m_shm.block();
        if (!block || !m_isPublisher || m_outCount <= 0 || m_bufferSize <= 0) return;
        // Zero audio-thread work when no tap client is attached.
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

        const ASIOSampleType leftType = m_outChannels[0].type;
        const ASIOSampleType rightType = m_outCount > 1 ? m_outChannels[1].type : leftType;
        for (long i = 0; i < count; ++i) {
            const float l = ReadSampleAsFloat(leftType, left, i);
            const float r = ReadSampleAsFloat(rightType, right, i);
            const uint64_t index = (write + static_cast<uint64_t>(i)) & kRingMask;
            block->samples[index * 2] = l;
            block->samples[index * 2 + 1] = r;
        }
        if (count > 0) {
            block->writeIdx.store(write + static_cast<uint64_t>(count), std::memory_order_release);
        }
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

    // ---- tap consumer (slave) ----
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

        long channels = 0;
        for (long i = 0; i < numChannels && channels < 2; ++i) {
            if (bufferInfos[i].isInput) ++channels;
        }
        if (channels == 0) return ASE_InvalidParameter;

        for (long ch = 0; ch < channels; ++ch) {
            const size_t bytes = static_cast<size_t>(bufferSize) * sizeof(float);
            m_tapBuffer[ch][0] = static_cast<float*>(_aligned_malloc(bytes, 64));
            m_tapBuffer[ch][1] = static_cast<float*>(_aligned_malloc(bytes, 64));
            if (!m_tapBuffer[ch][0] || !m_tapBuffer[ch][1]) {
                freeTapBuffers();
                return ASE_NoMemory;
            }
            std::memset(m_tapBuffer[ch][0], 0, bytes);
            std::memset(m_tapBuffer[ch][1], 0, bytes);
        }

        long mapped = 0;
        for (long i = 0; i < numChannels && mapped < channels; ++i) {
            if (!bufferInfos[i].isInput) continue;
            bufferInfos[i].buffers[0] = m_tapBuffer[mapped][0];
            bufferInfos[i].buffers[1] = m_tapBuffer[mapped][1];
            ++mapped;
        }

        m_tapChannels = channels;
        m_tapBufferSize = bufferSize;
        m_hostCallbacks = *callbacks;
        m_buffersCreated = true;

        if (ShmBlock* block = m_shm.block()) {
            block->readIdx.store(block->writeIdx.load(std::memory_order_acquire),
                                 std::memory_order_release);
        }

        if (m_hostCallbacks.asioMessage) {
            m_hostCallbacks.asioMessage(kAsioSupportsTimeInfo, ASIOTrue, nullptr, nullptr);
        }
        return ASE_OK;
    }

    void freeTapBuffers() {
        for (auto& channel : m_tapBuffer) {
            for (float*& buffer : channel) {
                if (buffer) {
                    _aligned_free(buffer);
                    buffer = nullptr;
                }
            }
        }
        m_tapChannels = 0;
        m_tapBufferSize = 0;
    }

    ASIOError startTapWorker() {
        if (!m_buffersCreated || m_tapChannels == 0 || !m_hostCallbacks.bufferSwitch) {
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

        LARGE_INTEGER frequency = {};
        QueryPerformanceFrequency(&frequency);
        const double qpcHz = frequency.QuadPart > 0 ? static_cast<double>(frequency.QuadPart) : 1.0;

        ShmBlock* block = m_shm.block();
        uint32_t lastGeneration = block ? block->generation.load(std::memory_order_acquire) : 0;

        bool waitingForFill = true;
        long doubleBuffer = 0;
        bool firstTick = true;
        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);
        double targetQpc = static_cast<double>(now.QuadPart);

        while (!m_workerStop.load(std::memory_order_relaxed)) {
            bool streaming = false;
            double rate = m_sampleRate;
            uint64_t write = 0;

            if (block) {
                const uint32_t generation = block->generation.load(std::memory_order_acquire);
                streaming = block->masterStreaming.load(std::memory_order_acquire) != 0;
                const double sharedRate =
                    SampleRateFromBits(block->sampleRateBits.load(std::memory_order_acquire));
                if (sharedRate >= 8000.0 && sharedRate <= 384000.0) rate = sharedRate;
                write = block->writeIdx.load(std::memory_order_acquire);

                uint64_t read = block->readIdx.load(std::memory_order_relaxed);
                if (generation != lastGeneration || write < read || write - read > kRingFrames) {
                    block->readIdx.store(write, std::memory_order_release);
                    lastGeneration = generation;
                    waitingForFill = true;
                }
            }

            if (!streaming) waitingForFill = true;

            uint64_t read = block ? block->readIdx.load(std::memory_order_relaxed) : 0;
            uint64_t available = write >= read ? write - read : 0;
            if (available > kRingFrames) available = 0;

            const long frames = m_tapBufferSize;
            long targetFill = frames * 3;
            const long maxFill = static_cast<long>(kRingFrames / 2);
            if (targetFill > maxFill) targetFill = maxFill;
            if (targetFill < frames) targetFill = frames;

            if (streaming && waitingForFill && available >= static_cast<uint64_t>(targetFill)) {
                waitingForFill = false;
            }

            if (!firstTick) {
                double adjust = 1.0;
                if (!waitingForFill) {
                    double errorRatio = 0.0;
                    if (frames > 0) {
                        errorRatio = (static_cast<double>(available) - static_cast<double>(targetFill)) /
                                     static_cast<double>(frames);
                        if (errorRatio > 1.0) errorRatio = 1.0;
                        if (errorRatio < -1.0) errorRatio = -1.0;
                    }
                    // Too much buffered -> consume a little faster; too little -> slower.
                    adjust = 1.0 - 0.04 * errorRatio;
                }

                const double periodSeconds = static_cast<double>(frames) / (rate > 0.0 ? rate : 48000.0);
                targetQpc += periodSeconds * adjust * qpcHz;
                QueryPerformanceCounter(&now);
                double waitTicks = targetQpc - static_cast<double>(now.QuadPart);
                if (waitTicks < 0.0) {
                    targetQpc = static_cast<double>(now.QuadPart);
                    waitTicks = 0.0;
                }

                LONGLONG due100ns = static_cast<LONGLONG>(waitTicks * 10000000.0 / qpcHz);
                if (due100ns < 1) due100ns = 1;
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
            firstTick = false;

            if (m_workerStop.load(std::memory_order_relaxed)) break;

            if (block) {
                write = block->writeIdx.load(std::memory_order_acquire);
                read = block->readIdx.load(std::memory_order_relaxed);
                if (write < read || write - read > kRingFrames) {
                    block->readIdx.store(write, std::memory_order_release);
                    waitingForFill = true;
                    read = write;
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

            for (long ch = 0; ch < m_tapChannels; ++ch) {
                float* destination = m_tapBuffer[ch][doubleBuffer];
                if (!haveData) {
                    std::memset(destination, 0, static_cast<size_t>(frames) * sizeof(float));
                    continue;
                }
                for (long i = 0; i < frames; ++i) {
                    const uint64_t index = (read + static_cast<uint64_t>(i)) & kRingMask;
                    destination[i] = block->samples[index * 2 + static_cast<uint64_t>(ch)];
                }
            }

            if (haveData) {
                block->readIdx.store(read + static_cast<uint64_t>(frames), std::memory_order_release);
            } else if (block && !streaming) {
                // Master idle: drop whatever is left. When streaming but still
                // priming, keep readIdx put so the ring can actually fill.
                block->readIdx.store(write, std::memory_order_release);
            }

            const uint64_t position = m_samplePos.load(std::memory_order_relaxed);
            const uint64_t timeStamp = QpcNanoseconds();
            ASIOTime time = {};
            time.timeInfo.speed = 1.0;
            time.timeInfo.samplePosition = static_cast<unsigned long>(position & 0xFFFFFFFFu);
            time.timeInfo.sampleRate = rate;
            time.timeInfo.flags =
                kSystemTimeValid | kSamplePositionValid | kSampleRateValid | kSpeedValid;
            time.timeInfo.systemTime.hi = static_cast<unsigned long>(timeStamp >> 32);
            time.timeInfo.systemTime.lo = static_cast<unsigned long>(timeStamp & 0xFFFFFFFFu);
            time.timeCode.speed = 1.0;

            if (m_hostCallbacks.bufferSwitchTimeInfo) {
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

    // ---- lifecycle helpers ----
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
            m_buffersCreated = false;
            if (m_tapRegistered && m_shm.block()) {
                m_shm.block()->tapClients.fetch_sub(1, std::memory_order_acq_rel);
            }
            m_tapRegistered = false;
            m_shm.Detach();
            return;
        }

        s_activeMaster.store(nullptr, std::memory_order_release);
        m_hostCallbacks = {};
        m_outCount = 0;

        if (m_buffersCreated && m_hwAsio) {
            m_hwAsio->stop();
            m_hwAsio->disposeBuffers();
            m_buffersCreated = false;
        }
        m_started = false;

        releaseMaster();

        if (m_hwAsio) {
            m_hwAsio->Release();
            m_hwAsio = nullptr;
        }
        m_shm.Detach();
    }

    void setError(const char* message) { strncpy_s(m_lastError, message, _TRUNCATE); }

    void copyHardwareError() {
        if (!m_hwAsio) return;
        char reason[128] = {};
        m_hwAsio->getErrorMessage(reason);
        if (reason[0]) setError(reason);
    }

    const DriverEntry& m_entry;
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
    long m_tapChannels = 0;
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
    // One object per role, per host process, shared by every CoCreateInstance.
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

    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&m_ref)); }

    ULONG STDMETHODCALLTYPE Release() override {
        const LONG refs = InterlockedDecrement(&m_ref);
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
            InterlockedIncrement(&g_serverLocks);
        } else {
            InterlockedDecrement(&g_serverLocks);
        }
        return S_OK;
    }

private:
    const DriverEntry& m_entry;
    LONG m_ref = 1;
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
    return (g_driverRefs == 0 && g_serverLocks == 0) ? S_OK : S_FALSE;
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
