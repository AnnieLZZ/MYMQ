#include <chrono>
#include <iostream>
#include <vector>

#include "BufferPool.h"

int main() {
    using Pool = MYMQ::Client::BufferPool;
    auto& pool = Pool::instance();

    std::vector<Pool::Block> blocks;
    blocks.reserve(pool.max_bytes() / Pool::kChunkBytes);

    Pool::Block blk;
    while (pool.try_allocate(Pool::kChunkBytes, blk)) {
        blocks.push_back(blk);
    }

    if (pool.used_bytes() > pool.max_bytes()) {
        std::cerr << "FAIL: used_bytes > max_bytes\n";
        return 2;
    }

    Pool::Block out;
    auto t0 = std::chrono::steady_clock::now();
    bool ok = pool.try_allocate_for(Pool::kChunkBytes, std::chrono::milliseconds(100), out);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    if (ok) {
        std::cerr << "FAIL: allocation should timeout when pool is full\n";
        return 3;
    }
    if (elapsed_ms > 150) {
        std::cerr << "FAIL: timeout too long: " << elapsed_ms << "ms\n";
        return 4;
    }

    for (auto& b : blocks) {
        pool.release(b);
    }

    if (pool.used_bytes() != 0) {
        std::cerr << "FAIL: used_bytes not back to zero: " << pool.used_bytes() << "\n";
        return 5;
    }

    std::cout << "PASS\n";
    return 0;
}

