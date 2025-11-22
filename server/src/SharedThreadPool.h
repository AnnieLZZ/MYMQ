#ifndef SHAREDTHREADPOOL_H
#define SHAREDTHREADPOOL_H

#pragma once

#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <functional>
#include <iostream>

// 也就是你刚下载的那个库
#include "blockingconcurrentqueue.h"

class ShardedThreadPool {
public:
    using Task = std::function<void()>;

    // 单例模式获取实例 (可选，看你架构需求)
    static ShardedThreadPool& instance(size_t thread_count = std::thread::hardware_concurrency()) {
        static ShardedThreadPool pool(thread_count);
        return pool;
    }

    // 构造函数：初始化 N 个 Worker，每个 Worker 带一个独立队列
    explicit ShardedThreadPool(size_t thread_count) : stop_(false) {
        if (thread_count == 0) thread_count = 1; // 兜底防呆

        workers_.reserve(thread_count);
        for (size_t i = 0; i < thread_count; ++i) {
            // 创建并启动 Worker
            workers_.emplace_back(std::make_unique<Worker>(this, i));
        }
        std::cout << "[ThreadPool] Initialized with " << thread_count << " sharded workers." << std::endl;
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

    // ----------------------------------------------------------------------
    // 【核心接口】提交任务
    // key: 通常传入 client_fd。
    //      算法保证同一个 key 永远分配给同一个线程。
    // ----------------------------------------------------------------------
    void submit(int key, Task task) {
        // 1. 计算分片索引 (简单取余，效率极高)
        // 注意：key 可能是负数吗？通常 fd > 0。如果是业务 ID 可能为负，需转 size_t。
        size_t index = static_cast<size_t>(key) % workers_.size();

        // 2. 定向投递到对应 Worker 的队列
        // 使用 std::move 减少 task 拷贝开销
        workers_[index]->queue.enqueue(std::move(task));
    }

    // 获取当前线程数
    size_t thread_count() const {
        return workers_.size();
    }

private:
    // 内部 Worker 结构体
    struct Worker {
        ShardedThreadPool* pool;
        int id;
        // 关键：每个 Worker 独占一个阻塞队列
        moodycamel::BlockingConcurrentQueue<Task> queue;
        std::thread thread;

        Worker(ShardedThreadPool* p, int idx) : pool(p), id(idx) {
            // 启动线程
            thread = std::thread([this]() {
                this->run();
            });
        }

        void run() {
            while (!pool->stop_) {
                Task task;
                // 使用 wait_dequeue_timed
                // 参数：任务引用，超时时间(微秒)
                // 作用：如果队列为空，线程挂起休眠。超时后返回 false，检查 stop_ 标志。
                // 这样既保证了响应速度，又支持优雅退出。
                if (queue.wait_dequeue_timed(task, std::chrono::milliseconds(100))) {
                    try {
                        if (task) {
                            task(); // 执行业务逻辑
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "[ThreadPool] Worker " << id << " exception: " << e.what() << std::endl;
                    } catch (...) {
                        std::cerr << "[ThreadPool] Worker " << id << " unknown exception." << std::endl;
                    }
                }
            }
        }
    };

    std::vector<std::unique_ptr<Worker>> workers_;
    std::atomic<bool> stop_;
};

#endif // SHAREDTHREADPOOL_H
