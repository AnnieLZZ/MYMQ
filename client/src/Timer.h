#ifndef TIMER_H
#define TIMER_H
#include "ThreadPool.h"
#include <iostream>
#include <chrono>
#include <atomic>
#include <memory>
#include <list>
#include <map>
#include <queue>
#include <mutex>
#include <future>
#include <functional>
#include <tuple>

class Timer {

    enum  Time_unit{ms,s};

public:
    Timer() : m_stop(false), m_thread([this]() { worker_thread(); }) {}

    ~Timer() {
        stop();
        if (m_thread.joinable()) {
            m_thread.join();
        }
        {
            std::unique_lock<std::mutex> ulock(m_mutex);
            std::priority_queue<std::shared_ptr<Task>, std::vector<std::shared_ptr<Task>>, Compare> empty_pq;
            m_tasks.swap(empty_pq);
            m_task_references.clear();
        }
    }

    void start(){
        m_stop.store(false);
    }

    void reset(){
        stop();
        if (m_thread.joinable()) {
            m_thread.join();
        }

        {
            std::unique_lock<std::mutex> ulock(m_mutex);
            std::priority_queue<std::shared_ptr<Task>, std::vector<std::shared_ptr<Task>>, Compare> empty_pq;
            m_tasks.swap(empty_pq);
            m_task_references.clear();
        }

        m_stop.store(false);
        m_thread = std::thread([this]() { worker_thread(); });
    }

    template <class F, class... Args>
    size_t commit_ms(F&& f, int delay_ms, int period_ms ,int exec_cnt=-1, Args&&... args) {


        if(exec_cnt<-1||exec_cnt==0){
            throw std::runtime_error("invalid exec_count: exec_count must be equal to '-1' or other positive integer .");
        }
        else{
            if(exec_cnt!=-1){
                exec_cnt--;
            }
        }
        if(period_ms<=0){
            throw std::runtime_error("invalid period_ms: period_ms must be a positive integer .");
        }
        auto&& task_func = [f = std::forward<F>(f), args = std::make_tuple(std::forward<Args>(args)...)]() mutable {
            std::apply(f, args);
        };

        auto now = std::chrono::steady_clock::now();
        auto exec_time = now + std::chrono::milliseconds(delay_ms);
        Time_unit unit=Time_unit::ms;
        auto taskid=cnt.load();

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

            if(m_tasks.size() >= m_max_queue_size.load()){
                return 0;
            }

            m_tasks.push(newTask);
            m_task_references[taskid] = newTask;
            cnt++;
            m_cv.notify_one();
        }
        return taskid;
    }


    template <class F, class... Args>
    size_t commit_s(F&& f, int delay_s, int period_s ,int exec_cnt=-1, Args&&... args) {


        if(exec_cnt<-1||exec_cnt==0){
            throw std::runtime_error("invalid exec_count: exec_count must be equal to '-1' or other positive integer .");
        }
        else{
            if(exec_cnt!=-1){
                exec_cnt--;
            }
        }
        if(period_s<=0){
            throw std::runtime_error("invalid period_s: period_s must be a positive integer .");
        }
        auto&& task_func = [f = std::forward<F>(f), args = std::make_tuple(std::forward<Args>(args)...)]() mutable {
            std::apply(f, args);
        };

        auto now = std::chrono::steady_clock::now();
        auto exec_time = now + std::chrono::seconds(delay_s);
        Time_unit unit=Time_unit::s;

        auto taskid=cnt.load();
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

            if(m_tasks.size() >= m_max_queue_size.load()){
                return 0;
            }

            m_tasks.push(newTask);
            m_task_references[taskid] = newTask;
            cnt++;
        }
        m_cv.notify_one();

        return  taskid;
    }

    void cancel_task(size_t& task_id) {
        if(task_id==0){
            return;
        }
        std::unique_lock<std::mutex> lock(m_mutex);
        auto it = m_task_references.find(task_id);
        if (it != m_task_references.end()) {
            it->second->is_cancelled = true;
            m_task_references.erase(it);
            task_id=0;
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
        int exec_leftcnt;
        size_t id;
        std::function<void()> func;
        bool is_cancelled;

    };

    struct Compare {
        bool operator()(const std::shared_ptr<Task>& a, const std::shared_ptr<Task>& b) {
            return a->exec_time > b->exec_time;
        }
    };

    std::atomic<size_t> cnt{1};
    std::atomic<bool> m_stop;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::priority_queue<std::shared_ptr<Task>, std::vector<std::shared_ptr<Task>>, Compare> m_tasks;
    std::thread m_thread;

    std::map<size_t, std::shared_ptr<Task>> m_task_references;

    std::mutex m_active_futures_mutex;
    std::list<std::future<void>> m_active_futures;
    std::atomic<size_t> m_max_queue_size{100000};



    void cleanup_active_futures() {
        std::lock_guard<std::mutex> futures_lock(m_active_futures_mutex);
        for (auto& f : m_active_futures) {
            if (f.valid()) {
                f.wait();
            }
        }
        m_active_futures.clear();
    }

    void worker_thread() {
        while (true) {
            std::shared_ptr<Task> task_to_execute;

            {
                std::unique_lock<std::mutex> lock(m_mutex);

                while (!m_stop && (m_tasks.empty() ||
                                   m_tasks.top()->exec_time > std::chrono::steady_clock::now() ||
                                   m_tasks.top()->is_cancelled)) {
                    if (m_stop) break;

                    if (m_tasks.empty() || m_tasks.top()->is_cancelled) {
                        if (!m_tasks.empty() && m_tasks.top()->is_cancelled) {
                            m_tasks.pop();
                            continue;
                        }
                        m_cv.wait(lock);
                    } else {
                        m_cv.wait_until(lock, m_tasks.top()->exec_time);
                    }
                }

                if (m_stop) {
                    lock.unlock();
                    cleanup_active_futures();
                    return;
                }

                task_to_execute = m_tasks.top();
                m_tasks.pop();
            }

            bool is_last_exec = (task_to_execute->exec_leftcnt == 0);

            std::future<void> current_task_future;
            if (is_last_exec) {
                current_task_future = ThreadPool::instance().commit(std::move(task_to_execute->func));
            } else {
                current_task_future = ThreadPool::instance().commit(task_to_execute->func); // 必须拷贝
            }

            // 保留了原有的 active_futures 逻辑
            {
                std::lock_guard<std::mutex> futures_lock(m_active_futures_mutex);
                m_active_futures.remove_if([](std::future<void>& f){
                    return f.valid() && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                });
                m_active_futures.push_back(std::move(current_task_future));
            }

            {
                std::unique_lock<std::mutex> lock(m_mutex);

                if (!task_to_execute->is_cancelled) {
                    auto new_exec_time = std::chrono::steady_clock::now(); // 保留了原有逻辑（会有时间漂移）
                    if(task_to_execute->unit==Time_unit::ms){
                        new_exec_time+= std::chrono::milliseconds(task_to_execute->period);
                    }
                    else if(task_to_execute->unit==Time_unit::s){
                        new_exec_time+=std::chrono::seconds(task_to_execute->period);
                    }

                    if(task_to_execute->exec_leftcnt > 0){
                        task_to_execute->exec_leftcnt--;
                        task_to_execute->exec_time = new_exec_time;
                        m_tasks.push(task_to_execute);
                        m_cv.notify_one();
                    }
                    else if(task_to_execute->exec_leftcnt == -1){
                        task_to_execute->exec_time = new_exec_time;
                        m_tasks.push(task_to_execute);
                        m_cv.notify_one();
                    } else {
                        m_task_references.erase(task_to_execute->id);
                    }
                } else {
                    // 确保 cancel 的任务也能清理 map
                    auto it = m_task_references.find(task_to_execute->id);
                    if (it != m_task_references.end()) {
                        m_task_references.erase(it);
                    }
                }
            }
        }
    }
};

#endif
