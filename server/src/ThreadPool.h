#ifndef THREADPOOL_H
#define THREADPOOL_H

#include "blockingconcurrentqueue.h"
#include <vector>
#include <thread>
#include <future>
#include <functional>
#include <memory>
#include <atomic>
#include <chrono>


using Task = std::function<void()>;

class ThreadPool {
    ThreadPool& operator= (const ThreadPool&) = delete;
    ThreadPool(const ThreadPool&) = delete;

public:
    ~ThreadPool() {
        stop();
    }

    static ThreadPool& instance(unsigned int thread_num = std::thread::hardware_concurrency()) {
        static ThreadPool ins(thread_num);
        return ins;
    }

    void start() {
        if (is_running) return;
        is_running = true;
        b_stop.store(false);

        for (int i = 0; i < left_thread_num; i++) {
            thread_pool.emplace_back([this] {
                while (true) {
                    Task task;

                    // 核心改动：使用超时阻塞出队
                    // 1. 如果有任务，立刻取走执行，高性能
                    // 2. 如果没任务，等待 20ms，然后检查一次 b_stop
                    // 这样避免了为了响应 stop 而空转，也避免了死等无法退出
                    if (task_queue.wait_dequeue_timed(task, std::chrono::milliseconds(20))) {
                        task();
                    }

                    // 退出检查：
                    // 如果收到停止信号，且队列里大概率没任务了，就退出
                    // size_approx() 是近似值，在并发队列里足够用来做退出判断
                    if (b_stop.load() && task_queue.size_approx() == 0) {
                        return;
                    }
                }
            });
        }
    }

    void stop() {
        if (!is_running || b_stop.load()) return;

        b_stop.store(true);

        for (auto& threadit : thread_pool) {
            if (threadit.joinable()) {
                threadit.join();
            }
        }
        thread_pool.clear();
        is_running = false;
    }

    template<class F, class ... Args>
    auto commit(F&& f, Args&&... args) -> std::future<decltype(std::forward<F>(f)(std::forward<Args>(args)...))> {

        using Rettype = decltype(std::forward<F>(f)(std::forward<Args>(args)...));

        if (b_stop.load()) {
            return std::future<Rettype>{};
        }

        auto task = std::make_shared<std::packaged_task<Rettype()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
            );

        std::future<Rettype> ret = task->get_future();

        task_queue.enqueue([task] {
            (*task)();
        });

        return ret;
    }

private:
    ThreadPool(unsigned int thread_num) : b_stop(false), is_running(false) {
        if (thread_num <= 1) {
            left_thread_num = 2;
        } else if (thread_num > std::thread::hardware_concurrency()) {
            left_thread_num = std::thread::hardware_concurrency();
        } else {
            left_thread_num = thread_num;
        }
        start();
    }

private:
    std::vector<std::thread> thread_pool;
    moodycamel::BlockingConcurrentQueue<Task> task_queue;

    std::atomic<bool> b_stop;
    bool is_running;
    int left_thread_num;
};

#endif // THREADPOOL_H
