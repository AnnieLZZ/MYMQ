#ifndef TIMER_H
#define TIMER_H

#include "ThreadPool.h"
#include <iostream>
#include <chrono>
#include <atomic>
#include <memory>
#include <map>
#include <queue>
#include <mutex>
#include <functional>
#include <tuple>

class Timer {
    enum Time_unit { ms, s };

public:
    Timer() : m_stop(false), m_thread([this]() { worker_thread(); }) {}

    ~Timer() {
        stop();
        if (m_thread.joinable()) {
            m_thread.join();
        }
        // 析构时清理队列
        std::unique_lock<std::mutex> ulock(m_mutex);
        while (!m_tasks.empty()) m_tasks.pop();
        m_task_references.clear();
    }

    void start() {
        m_stop.store(false);
    }

    // 重置定时器
    void reset() {
        stop();
        if (m_thread.joinable()) {
            m_thread.join();
        }

        {
            std::unique_lock<std::mutex> ulock(m_mutex);
            // 清空优先队列的 hack 写法（或者循环 pop）
            std::priority_queue<std::shared_ptr<Task>, std::vector<std::shared_ptr<Task>>, Compare> empty_pq;
            m_tasks.swap(empty_pq);
            m_task_references.clear();
        }

        m_stop.store(false);
        m_thread = std::thread([this]() { worker_thread(); });
    }

    // 提交毫秒级任务
    template <class F, class... Args>
    size_t commit_ms(F&& f, int delay_ms, int period_ms, int exec_cnt = -1, Args&&... args) {
        // 参数校验
        if (exec_cnt < -1 || exec_cnt == 0) {
            throw std::runtime_error("invalid exec_count: exec_count must be equal to '-1' or other positive integer.");
        }
        // exec_cnt 处理逻辑保持原样
        if (exec_cnt != -1) {
            exec_cnt--;
        }

        if (period_ms <= 0) {
            throw std::runtime_error("invalid period_ms: period_ms must be a positive integer.");
        }

        // 封装任务
        auto&& task_func = [f = std::forward<F>(f), args = std::make_tuple(std::forward<Args>(args)...)]() mutable {
            std::apply(f, args);
        };

        auto now = std::chrono::steady_clock::now();
        auto exec_time = now + std::chrono::milliseconds(delay_ms); // 第一次执行由 delay 决定
        Time_unit unit = Time_unit::ms;
        auto taskid = cnt.load();

        auto newTask = std::make_shared<Task>(Task{
            exec_time,
            period_ms,
            unit,
            exec_cnt,
            taskid,
            task_func,
            false
        });

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_tasks.size() >= m_max_queue_size.load()) {
                return 0;
            }
            m_tasks.push(newTask);
            m_task_references[taskid] = newTask;
            cnt++;
            // 只有当新任务比当前堆顶更早执行时，才需要唤醒线程
            // 但为了简单，直接 notify 也没问题
            m_cv.notify_one();
        }
        return taskid;
    }

    // 提交秒级任务
    template <class F, class... Args>
    size_t commit_s(F&& f, int delay_s, int period_s, int exec_cnt = -1, Args&&... args) {
        if (exec_cnt < -1 || exec_cnt == 0) {
            throw std::runtime_error("invalid exec_count: exec_count must be equal to '-1' or other positive integer.");
        }
        if (exec_cnt != -1) {
            exec_cnt--;
        }

        if (period_s <= 0) {
            throw std::runtime_error("invalid period_s: period_s must be a positive integer.");
        }

        auto&& task_func = [f = std::forward<F>(f), args = std::make_tuple(std::forward<Args>(args)...)]() mutable {
            std::apply(f, args);
        };

        auto now = std::chrono::steady_clock::now();
        auto exec_time = now + std::chrono::seconds(delay_s);
        Time_unit unit = Time_unit::s;
        auto taskid = cnt.load();

        auto newTask = std::make_shared<Task>(Task{
            exec_time,
            period_s,
            unit,
            exec_cnt,
            taskid,
            task_func,
            false
        });

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_tasks.size() >= m_max_queue_size.load()) {
                return 0;
            }
            m_tasks.push(newTask);
            m_task_references[taskid] = newTask;
            cnt++;
        }
        m_cv.notify_one();
        return taskid;
    }

    void cancel_task(size_t& task_id) {
        if (task_id == 0) return;
        std::unique_lock<std::mutex> lock(m_mutex);
        auto it = m_task_references.find(task_id);
        if (it != m_task_references.end()) {
            it->second->is_cancelled = true; // 标记取消
            m_task_references.erase(it);     // 移除引用
            task_id = 0;
        }
    }

    void stop() {
        m_stop.store(true);
        m_cv.notify_one();
    }

    void set_max_queue_size(size_t size) {
        m_max_queue_size.store(size);
    }

private:
    struct Task {
        std::chrono::steady_clock::time_point exec_time;
        int period;
        Time_unit unit;
        int exec_leftcnt; // -1 表示无限循环，0 表示最后一次，>0 表示剩余次数
        size_t id;
        std::function<void()> func;
        bool is_cancelled;
    };

    struct Compare {
        bool operator()(const std::shared_ptr<Task>& a, const std::shared_ptr<Task>& b) {
            return a->exec_time > b->exec_time; // 最小堆
        }
    };

    std::atomic<size_t> cnt{1};
    std::atomic<bool> m_stop;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::priority_queue<std::shared_ptr<Task>, std::vector<std::shared_ptr<Task>>, Compare> m_tasks;
    std::thread m_thread;
    std::map<size_t, std::shared_ptr<Task>> m_task_references;
    std::atomic<size_t> m_max_queue_size{100000};

    // --- Worker Thread (优化核心) ---
    void worker_thread() {
        while (!m_stop) {
            std::shared_ptr<Task> task_to_execute;

            {
                std::unique_lock<std::mutex> lock(m_mutex);

                // 1. 等待逻辑：队列为空时等待
                while (!m_stop && m_tasks.empty()) {
                    m_cv.wait(lock);
                }
                if (m_stop) break;

                // 2. 检查任务是否到期
                // 如果堆顶任务还没到时间，继续睡（可被唤醒）
                if (m_tasks.top()->exec_time > std::chrono::steady_clock::now()) {
                    m_cv.wait_until(lock, m_tasks.top()->exec_time);
                    continue; // 醒来后重新检查，可能有新任务插队
                }

                // 3. 取出任务
                task_to_execute = m_tasks.top();
                m_tasks.pop();

                // 4. 检查是否被取消 (惰性删除)
                if (task_to_execute->is_cancelled) {
                    // map 中的引用已经在 cancel_task 里删了，这里直接丢弃即可
                    continue;
                }
            }
            // === 关键优化：解锁！===
            // 在提交任务到线程池和计算下次时间时，不需要持有锁

            // 5. 提交到线程池 (Fire and Forget)
            bool is_last_exec = (task_to_execute->exec_leftcnt == 0);
            if (is_last_exec) {
                // 如果是最后一次，直接 move，减少一次拷贝
                ThreadPool::instance().commit(std::move(task_to_execute->func));
            } else {
                // 还要重复执行，必须拷贝副本
                ThreadPool::instance().commit(task_to_execute->func);
            }

            // 6. 处理周期性任务 (Time Drift 修正)
            // 只有需要再次执行的任务，才需要计算下一次时间
            if (task_to_execute->exec_leftcnt > 0 || task_to_execute->exec_leftcnt == -1) {

                // 修正漂移：基于【上一次计划时间】+ Period
                if (task_to_execute->unit == Time_unit::ms) {
                    task_to_execute->exec_time += std::chrono::milliseconds(task_to_execute->period);
                } else {
                    task_to_execute->exec_time += std::chrono::seconds(task_to_execute->period);
                }

                if (task_to_execute->exec_leftcnt > 0) {
                    task_to_execute->exec_leftcnt--;
                }

                // 重新加锁入队
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    if (!m_stop && !task_to_execute->is_cancelled) {
                        m_tasks.push(task_to_execute);
                        m_cv.notify_one(); // 通知自己（其实是通知 worker 循环）有新任务入队
                    } else {
                        // 如果在执行期间被取消了，清理引用
                        m_task_references.erase(task_to_execute->id);
                    }
                }
            } else {
                // 任务彻底结束 (exec_leftcnt == 0)，清理引用
                std::unique_lock<std::mutex> lock(m_mutex);
                m_task_references.erase(task_to_execute->id);
            }
        }
    }
};

#endif
