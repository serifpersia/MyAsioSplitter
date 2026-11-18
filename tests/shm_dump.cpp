#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cinttypes>
#include <cstdio>

#include "../src/shm_ring.h"

struct Snapshot {
    uint64_t write = 0;
    uint64_t read = 0;
    uint64_t tickMs = 0;
    double rate = 0.0;
};

static Snapshot Dump(const ShmBlock* block, const char* tag) {
    const uint32_t pid = block->masterPid.load(std::memory_order_acquire);
    const uint64_t write = block->writeIdx.load(std::memory_order_acquire);
    const uint64_t read = block->readIdx.load(std::memory_order_acquire);
    const double rate = SampleRateFromBits(block->sampleRateBits.load(std::memory_order_acquire));
    const int64_t avail = static_cast<int64_t>(write - read);
    const double availMs = rate > 0.0 ? (static_cast<double>(avail) * 1000.0 / rate) : 0.0;

    std::printf(
        "[%s] masterPid=%u alive=%d gen=%u streaming=%u tapClients=%u ch=%u bsize=%u "
        "rate=%.0f write=%" PRIu64 " read=%" PRIu64 " avail=%" PRId64 " (%.2f ms)\n",
        tag, pid, ProcessAlive(pid) ? 1 : 0, block->generation.load(std::memory_order_acquire),
        block->masterStreaming.load(std::memory_order_acquire),
        block->tapClients.load(std::memory_order_acquire),
        block->outputChannels.load(std::memory_order_acquire),
        block->bufferSize.load(std::memory_order_acquire), rate, write, read, avail, availMs);

    return Snapshot{write, read, GetTickCount64(), rate};
}

int main() {
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, kShmName);
    if (!mapping) {
        std::printf("No shared memory. Start Pianoteq on the Master first.\n");
        return 1;
    }

    auto* block = static_cast<const ShmBlock*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    if (!block) {
        std::printf("MapViewOfFile failed: %lu\n", GetLastError());
        CloseHandle(mapping);
        return 1;
    }

    const Snapshot t0 = Dump(block, "t0");
    Sleep(500);
    const Snapshot t1 = Dump(block, "t1");

    const double dt = static_cast<double>(t1.tickMs - t0.tickMs) / 1000.0;
    if (dt > 0.0) {
        const double pubHz = static_cast<double>(t1.write - t0.write) / dt;
        const double tapHz = static_cast<double>(t1.read - t0.read) / dt;
        std::printf("[rate] master=%.1f frames/s  tap=%.1f frames/s\n", pubHz, tapHz);
    }

    UnmapViewOfFile(block);
    CloseHandle(mapping);
    return 0;
}