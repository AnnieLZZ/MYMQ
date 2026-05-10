#ifndef MYMQ_CLIENT_BUFFERPOOL_H
#define MYMQ_CLIENT_BUFFERPOOL_H

#include <cstddef>
#include <cstdint>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <algorithm>
#include <new>
#include <cstdlib>
#if __has_include(<mimalloc.h>)
#include <mimalloc.h>
#else
#include "../thirdparty/mimalloc/include/mimalloc.h"
#endif

namespace MYMQ {
namespace Client {

class BufferPool {
public:
    static constexpr size_t kChunkBytes = 16ULL * 1024;
    static constexpr size_t kDefaultMaxBytes = 256ULL * 1024 * 1024;

    struct Block {
        uint32_t start_chunk = 0;
        uint32_t chunk_count = 0;
        uint8_t* data = nullptr;
        size_t size_bytes = 0;
        bool valid() const { return data != nullptr && size_bytes > 0 && chunk_count > 0; }
    };

    static BufferPool& instance() {
        static BufferPool inst;
        return inst;
    }

    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    size_t max_bytes() const { return max_bytes_; }
    size_t used_bytes() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return used_bytes_;
    }

    bool try_allocate(size_t bytes, Block& out) {
        std::lock_guard<std::mutex> lock(mtx_);
        return allocate_locked(bytes, out);
    }

    bool try_allocate_for(size_t bytes, std::chrono::milliseconds timeout, Block& out) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        std::unique_lock<std::mutex> lock(mtx_);
        while (true) {
            if (allocate_locked(bytes, out)) return true;
            if (timeout.count() == 0) return false;
            if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) return false;
        }
    }

    void release(Block& block) {
        if (!block.valid()) return;
        std::lock_guard<std::mutex> lock(mtx_);

        const uint32_t start = block.start_chunk;
        const uint32_t count = block.chunk_count;
        insert_and_coalesce_locked(Extent{start, count});

        const size_t bytes = static_cast<size_t>(count) * kChunkBytes;
        used_bytes_ = (used_bytes_ >= bytes) ? (used_bytes_ - bytes) : 0;

        block = Block{};
        cv_.notify_one();
    }

private:
    struct Extent {
        uint32_t start = 0;
        uint32_t count = 0;
    };

    BufferPool() : max_bytes_(kDefaultMaxBytes) {
        storage_ = static_cast<uint8_t*>(mi_malloc(max_bytes_));
        if (!storage_) {
            throw std::bad_alloc();
        }
        const uint32_t total_chunks = static_cast<uint32_t>(max_bytes_ / kChunkBytes);
        free_extents_.push_back(Extent{0, total_chunks});
    }
    ~BufferPool() {
        mi_free(storage_);
        storage_ = nullptr;
    }

    static uint32_t bytes_to_chunks(size_t bytes) {
        if (bytes == 0) return 0;
        return static_cast<uint32_t>((bytes + kChunkBytes - 1) / kChunkBytes);
    }

    bool allocate_locked(size_t bytes, Block& out) {
        out = Block{};
        const uint32_t need_chunks = bytes_to_chunks(bytes);
        if (need_chunks == 0) return false;

        for (size_t i = 0; i < free_extents_.size(); ++i) {
            auto& ext = free_extents_[i];
            if (ext.count < need_chunks) continue;

            const uint32_t start = ext.start;
            ext.start += need_chunks;
            ext.count -= need_chunks;

            if (ext.count == 0) {
                free_extents_.erase(free_extents_.begin() + static_cast<std::ptrdiff_t>(i));
            }

            out.start_chunk = start;
            out.chunk_count = need_chunks;
            out.data = storage_ + static_cast<size_t>(start) * kChunkBytes;
            out.size_bytes = static_cast<size_t>(need_chunks) * kChunkBytes;

            used_bytes_ += out.size_bytes;
            return true;
        }
        return false;
    }

    void insert_and_coalesce_locked(const Extent& released) {
        auto pos = std::lower_bound(free_extents_.begin(), free_extents_.end(), released.start,
                                    [](const Extent& ext, uint32_t start) {
                                        return ext.start < start;
                                    });

        pos = free_extents_.insert(pos, released);

        if (pos != free_extents_.begin()) {
            auto prev = pos - 1;
            if (prev->start + prev->count == pos->start) {
                prev->count += pos->count;
                pos = free_extents_.erase(pos);
                pos = prev;
            }
        }

        if ((pos + 1) != free_extents_.end()) {
            auto next = pos + 1;
            if (pos->start + pos->count == next->start) {
                pos->count += next->count;
                free_extents_.erase(next);
            }
        }
    }

    mutable std::mutex mtx_;
    std::condition_variable cv_;

    uint8_t* storage_ = nullptr;
    const size_t max_bytes_;
    size_t used_bytes_ = 0;
    std::vector<Extent> free_extents_;
};

} // namespace Client
} // namespace MYMQ

#endif
