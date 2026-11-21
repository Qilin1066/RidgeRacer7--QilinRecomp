// sysPrxForUser's memory pools (sys_mempool_...): equal blocks handed out
// from a piece of memory the caller provides. Taking a block when none is
// free waits until one is given back.
#include <condition_variable>
#include <mutex>
#include <vector>

#include "kernel.hpp"
#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

struct Pool {
    std::mutex mutex;
    std::condition_variable freed;
    uint32_t first = 0, block_size = 0, blocks = 0;
    std::vector<uint32_t> free;  // addresses, the next to hand out last
};
ObjectTable<Pool> g_pools(0x97000001u);

// sys_mempool_create(sys_mempool_t* id, void* chunk, chunk size, block size, alignment)
void sys_mempool_create(Cpu& c) {
    const uint32_t out = static_cast<uint32_t>(c.r[3]), chunk = static_cast<uint32_t>(c.r[4]);
    const uint64_t chunk_size = c.r[5], block_size = c.r[6], alignment = c.r[7];
    if (!out || !chunk) {
        c.r[3] = kEfault;
        return;
    }
    if (block_size == 0 || alignment == 0 || (alignment & (alignment - 1)) || block_size > chunk_size) {
        c.r[3] = kEinval;
        return;
    }
    auto pool = std::make_shared<Pool>();
    pool->first = static_cast<uint32_t>((chunk + alignment - 1) & ~(alignment - 1));
    // Each block starts at the alignment, so a block's stride is its size rounded up to it.
    const uint64_t stride = (block_size + alignment - 1) & ~(alignment - 1);
    const uint64_t room = chunk_size - (pool->first - chunk);
    pool->block_size = static_cast<uint32_t>(stride);
    pool->blocks = static_cast<uint32_t>(room / stride);
    if (pool->blocks == 0) {
        c.r[3] = kEinval;
        return;
    }
    for (uint32_t i = pool->blocks; i-- > 0;) pool->free.push_back(pool->first + i * pool->block_size);
    store32(out, g_pools.add(std::move(pool)));
    c.r[3] = kOk;
}

// sys_mempool_destroy(id)
void sys_mempool_destroy(Cpu& c) {
    g_pools.remove(static_cast<uint32_t>(c.r[3]));
    c.r[3] = kOk;
}

// sys_mempool_allocate_block(id): a block's address, waiting for one if need be.
// sys_mempool_try_allocate_block(id): or null at once.
void allocate_block(Cpu& c, bool waiting) {
    const auto pool = g_pools.find(static_cast<uint32_t>(c.r[3]));
    if (!pool) {
        c.r[3] = 0;
        return;
    }
    std::unique_lock lock(pool->mutex);
    if (pool->free.empty()) {
        if (!waiting) {
            c.r[3] = 0;
            return;
        }
        pool->freed.wait(lock, [&] { return !pool->free.empty(); });
    }
    c.r[3] = pool->free.back();
    pool->free.pop_back();
}
void sys_mempool_allocate_block(Cpu& c) { allocate_block(c, true); }
void sys_mempool_try_allocate_block(Cpu& c) { allocate_block(c, false); }

// sys_mempool_free_block(id, block)
void sys_mempool_free_block(Cpu& c) {
    const auto pool = g_pools.find(static_cast<uint32_t>(c.r[3]));
    const uint32_t block = static_cast<uint32_t>(c.r[4]);
    if (!pool) {
        c.r[3] = kEsrch;
        return;
    }
    {
        const std::lock_guard lock(pool->mutex);
        if (block < pool->first || (block - pool->first) % pool->block_size || (block - pool->first) / pool->block_size >= pool->blocks) {
            c.r[3] = kEinval;
            return;
        }
        pool->free.push_back(block);
    }
    pool->freed.notify_one();
    c.r[3] = kOk;
}

constexpr NativeImport kImports[] = {
    {"sysPrxForUser", "sys_mempool_create", sys_mempool_create},
    {"sysPrxForUser", "sys_mempool_destroy", sys_mempool_destroy},
    {"sysPrxForUser", "sys_mempool_allocate_block", sys_mempool_allocate_block},
    {"sysPrxForUser", "sys_mempool_try_allocate_block", sys_mempool_try_allocate_block},
    {"sysPrxForUser", "sys_mempool_free_block", sys_mempool_free_block},
};

}  // namespace

std::span<const NativeImport> mempool_imports() { return kImports; }

}  // namespace rr7
