#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <bit>
#include <cstdint>

constexpr uint32_t kRingFrames = 16384;
constexpr uint64_t kRingMask = kRingFrames - 1;
static_assert((kRingFrames & (kRingFrames - 1)) == 0);

constexpr wchar_t kShmName[] = L"Local\\MyAsioSplitter_SHM";

#pragma warning(push)
#pragma warning(disable : 4324)
struct ShmBlock {
    std::atomic<uint32_t> masterPid;
    std::atomic<uint32_t> generation;
    std::atomic<uint32_t> masterStreaming;
    std::atomic<uint32_t> outputChannels;
    std::atomic<uint32_t> bufferSize;
    std::atomic<uint32_t> tapClients;
    std::atomic<uint64_t> sampleRateBits;

    alignas(64) std::atomic<uint64_t> writeIdx;
    alignas(64) std::atomic<uint64_t> readIdx;

    alignas(64) float samples[kRingFrames * 2];
};
#pragma warning(pop)

static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);

inline uint64_t SampleRateToBits(double rate) { return std::bit_cast<uint64_t>(rate); }
inline double SampleRateFromBits(uint64_t bits) { return std::bit_cast<double>(bits); }

inline bool ProcessAlive(uint32_t pid) {
    if (pid == 0) return false;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) return false;
    const DWORD state = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return state == WAIT_TIMEOUT;
}

class ShmMapping {
public:
    ShmMapping() = default;
    ~ShmMapping() { Detach(); }
    ShmMapping(const ShmMapping&) = delete;
    ShmMapping& operator=(const ShmMapping&) = delete;

    bool Attach() {
        if (view_) return true;
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      static_cast<DWORD>(sizeof(ShmBlock)), kShmName);
        if (!mapping_) return false;
        view_ = static_cast<ShmBlock*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        if (!view_) {
            CloseHandle(mapping_);
            mapping_ = nullptr;
            return false;
        }
        return true;
    }

    void Detach() {
        if (view_) {
            UnmapViewOfFile(view_);
            view_ = nullptr;
        }
        if (mapping_) {
            CloseHandle(mapping_);
            mapping_ = nullptr;
        }
    }

    ShmBlock* block() const { return view_; }

private:
    HANDLE mapping_ = nullptr;
    ShmBlock* view_ = nullptr;
};