#ifndef REQUEST_TIMEOUT_QUEUE_H
#define REQUEST_TIMEOUT_QUEUE_H

#include "concurrentqueue.h"
#include "tbb/concurrent_hash_map.h"
#include <deque>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <iostream>

#include"MYMQ_innercodes.h"
using Mybyte = std::vector<unsigned char>;
using Eve=MYMQ::EventType;
using ResponseCallback=MYMQ::ResponseCallback;
struct TimeoutNode {
    uint32_t coid;
    std::chrono::steady_clock::time_point expire_time;
};

class RequestTimeoutQueue {
public:
    // 构造函数：传入业务 Map、计数器引用、以及超时时间（毫秒）
    RequestTimeoutQueue(tbb::concurrent_hash_map<uint32_t, ResponseCallback>& map_wait_responces,std::atomic<size_t>& flying_count_ref, size_t timeout_ms)
        :m_map_wait_responses(map_wait_responces),
          m_curr_flying_request_num(flying_count_ref),
          m_timeout_duration(std::chrono::milliseconds(timeout_ms)),
          m_running(true) 
    {
        m_thread = std::thread([this] { worker_thread(); });
    }

    ~RequestTimeoutQueue() {
        m_running = false;
        if (m_thread.joinable()) m_thread.join();
    }

    // ==========================================================
    // 发送端调用：极致的 O(1) 无锁入队
    // ==========================================================
    void add(uint32_t coid) {
        // 计算过期时间
        auto expire_at = std::chrono::steady_clock::now() + m_timeout_duration;
        
        // moodycamel 的 enqueue 是 wait-free/lock-free 的，几乎没有竞争开销
        m_concurrent_queue.enqueue(TimeoutNode{coid, expire_at});
    }

private:
    // 引用业务数据
    tbb::concurrent_hash_map<uint32_t, ResponseCallback>& m_map_wait_responses;
    std::atomic<size_t>& m_curr_flying_request_num;
    std::chrono::milliseconds m_timeout_duration;

    // 核心数据结构
    moodycamel::ConcurrentQueue<TimeoutNode> m_concurrent_queue;
    std::thread m_thread;
    std::atomic<bool> m_running;

    // ==========================================================
    // 后台检测线程
    // ==========================================================
    void worker_thread() {
        // 线程本地的有序队列 (Local Cache)
        // 只有这个线程访问它，所以完全不需要锁
        std::deque<TimeoutNode> local_pending_queue;
        
        // 批量拉取缓冲区
        const size_t BULK_SIZE = 128;
        TimeoutNode bulk_buffer[BULK_SIZE];

        while (m_running) {
            // 1. 控制检测频率 (例如 5ms - 10ms)
            // 不需要太频繁，因为网络超时不需要微秒级精度
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            // 2. 从无锁队列批量搬运数据到本地队列
            size_t count = 0;
            // try_dequeue_bulk 极大减少 CAS 操作次数，吞吐量极高
            while ((count = m_concurrent_queue.try_dequeue_bulk(bulk_buffer, BULK_SIZE)) > 0) {
                for (size_t i = 0; i < count; ++i) {
                    local_pending_queue.push_back(bulk_buffer[i]);
                }
            }

            // 3. 检查本地队列头部 (时间序检查)
            check_local_queue(local_pending_queue);
        }
    }

    void check_local_queue(std::deque<TimeoutNode>& queue) {
        if (queue.empty()) return;

        auto now = std::chrono::steady_clock::now();
        std::vector<uint32_t> timeout_coids;

        // 因为是按时间顺序入队的（大致有序），只要队头没过期，后面的肯定也没过期
        while (!queue.empty()) {
            if (now >= queue.front().expire_time) {
                // 已过期
                timeout_coids.push_back(queue.front().coid);
                queue.pop_front();
            } else {
                // 队头未过期，后续无需检查，直接退出循环
                break;
            }
        }

        // 4. 处理这一批超时的 ID (惰性删除检查)
        if (!timeout_coids.empty()) {
            handle_timeouts(timeout_coids);
        }
    }

    void handle_timeouts(const std::vector<uint32_t>& coids) {
        tbb::concurrent_hash_map<uint32_t, ResponseCallback>::accessor acc;
        
        for (uint32_t coid : coids) {
            // 去业务 Map 里查查看还在不在
            if (m_map_wait_responses.find(acc, coid)) {
                // Case A: 真的超时了 (Map 里还有)
                
                // 1. 取出回调
                auto cb = std::move(acc->second);
                
                // 2. 从 Map 移除
                m_map_wait_responses.erase(acc);
                m_curr_flying_request_num--;

                // 3. 执行回调 (通知上层 Timeout)
                try {
                    // 假设你的 Mybyte 和 Event 定义
                    cb(static_cast<uint16_t>(Eve::EVENTTYPE_NULL), Mybyte{}); 
                } catch (...) {
                    // 吞掉用户回调的异常，保护检测线程
                    std::cerr << "Exception in timeout callback for coid: " << coid << std::endl;
                }
            }
            else {
                // Case B: 已经被 handle_event 处理掉了
                // 这是一个 "Ghost Timer"，什么都不做，消耗极小
            }
        }
    }
};

#endif
