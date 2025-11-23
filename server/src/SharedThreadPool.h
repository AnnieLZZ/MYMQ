#ifndef SHAREDTHREADPOOL_H
#define SHAREDTHREADPOOL_H

#pragma once

#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <functional>
#include <iostream>
#include "blockingconcurrentqueue.h"

class ShardedThreadPool {
public:
    using Task = std::function<void()>;

    static ShardedThreadPool& instance(size_t thread_count = std::thread::hardware_concurrency()) {
        static ShardedThreadPool pool(thread_count);
        return pool;
    }

    explicit ShardedThreadPool(size_t thread_count) : stop_(false) {
        if (thread_count == 0) thread_count = 1; // 兜底防呆

        workers_.reserve(thread_count);
        for (size_t i = 0; i < thread_count; ++i) {
            // 创建并启动 Worker
            workers_.emplace_back(std::make_unique<Worker>(this, i));
        }
        std::cout << "[ShardedThreadPool] Initialized with " << thread_count << " sharded workers." << std::endl;
    }

    ~ShardedThreadPool() {
        stop_ = true;
        // 停止所有线程
        for (auto& worker : workers_) {
            if (worker->thread.joinable()) {
                worker->thread.join();
            }
        }
    }


    void submit(int key, Task task) {
        // 1. 计算分片索引
        size_t index = static_cast<size_t>(key) % workers_.size();
        workers_[index]->queue.enqueue(std::move(task));
    }

    // 获取当前线程数
    size_t thread_count() const {
        return workers_.size();
    }

private:
    struct Worker {
        ShardedThreadPool* pool;
        int id;
        moodycamel::BlockingConcurrentQueue<Task> queue;
        std::thread thread;

        Worker(ShardedThreadPool* p, int idx) : pool(p), id(idx) {
            thread = std::thread([this]() {
                this->run();
            });
        }

        void run() {
            while (!pool->stop_) {
                Task task;
                if (queue.wait_dequeue_timed(task, std::chrono::milliseconds(100))) {
                    try {
                        if (task) {
                            task();
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "[ShardedThreadPool] Worker " << id << " exception: " << e.what() << std::endl;
                    } catch (...) {
                        std::cerr << "[ShardedThreadPool] Worker " << id << " unknown exception." << std::endl;
                    }
                }
            }
        }
    };

    std::vector<std::unique_ptr<Worker>> workers_;
    std::atomic<bool> stop_;
};

#endif // SHAREDTHREADPOOL_H
