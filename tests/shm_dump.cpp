// Read-only snapshot of the live shared-memory ring. Run it while Pianoteq and
// OBS are open; it does not load the driver DLL, so it cannot disturb audio.
#include <windows.h>

#include <cinttypes>
#include <cstdio>

#include "../src/shm_ring.h"

static void Dump(const ShmBlock* block, const char* tag) {
    const uint32_t pid = block->masterPid.load(std::memory_order_acquire);
    const uint64_t write = block->writeIdx.load(std::memory_order_acquire);
    const uint64_t read = block->readIdx.load(std::memory_order_acquire);
    const double rate = SampleRateFromBits(block->sampleRateBits.load(std::memory_order_acquire));

    std::printf(
        "[%s] masterPid=%u alive=%d gen=%u streaming=%u tapClients=%u ch=%u bsize=%u "
        "rate=%.0f write=%" PRIu64 " read=%" PRIu64 " avail=%" PRId64 "\n",
        tag, pid, ProcessAlive(pid) ? 1 : 0, block->generation.load(std::memory_order_acquire),
        block->masterStreaming.load(std::memory_order_acquire),
        block->tapClients.load(std::memory_order_acquire),
        block->outputChannels.load(std::memory_order_acquire),
        block->bufferSize.load(std::memory_order_acquire), rate, write, read,
        static_cast<int64_t>(write - read));
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

    Dump(block, "t0");
    Sleep(500);
    Dump(block, "t1");
    std::printf("If write/read did not advance, the master is not publishing or the tap is not consuming.\n");

    UnmapViewOfFile(block);
    CloseHandle(mapping);
    return 0;
}
