#include "MYMQ_Client.h"
#include "ClientProtocol.h"
#include "Serialize.h"

namespace MYMQ {
namespace Client {

using namespace MYMQ_Public;

    bool ClientBase::send(MYMQ::EventType event_type, const Mybyte& msg_body, std::vector<MYMQ::Client::SparseCallback> cbs_)
    {
        // 1. 检查飞行请求数 (保持不变)
        size_t curr_fly = SIZE_MAX;
        cmc_.get_curr_flying_request_num(curr_fly);
        if (curr_fly >= max_in_flight_requests_num) {
            return 0;
        }

        // 2. 发送消息并挂载回调
        auto succ = cmc_.send_msg(static_cast<short>(event_type), msg_body,
                                  [this, saved_cbs = std::move(cbs_)] // 捕获稀疏列表
                                  (uint16_t event_type_responce, MYMQ::OwnedBytes msg_body_responce) mutable
                                  {
                                      // 解析响应
                                      auto resp = handle_response(static_cast<Eve>(event_type_responce), msg_body_responce);

                                      // 3. 遍历稀疏回调列表
                                      // saved_cbs 中只包含需要回调的消息，非需要回调的已被跳过
                                      for (auto& sparse_item : saved_cbs) {

                                          uint32_t msg_idx = sparse_item.relative_index; // [关键] 获取该消息在 Batch 中的相对位置
                                          auto& current_cb = sparse_item.cb;             // 获取对应的回调函数 variant

                                          std::visit([&](auto&& specific_cb) {
                                              using CBType = std::decay_t<decltype(specific_cb)>;

                                              // --- Push 响应处理 (需要用到 msg_idx 计算 offset) ---
                                              if constexpr (std::is_same_v<CBType, MYMQ_Public::PushResponceCallback>)
                                              {
                                                  if (auto* data = std::get_if<MYMQ_Public::PushResponce>(&resp)) {
                                                      MYMQ_Public::PushResponce individual_resp = *data;

                                                      // [核心修改]: Offset = Batch基准Offset + 消息相对索引
                                                      individual_resp.offset = data->offset + msg_idx;

                                                      specific_cb(individual_resp);
                                                  }
                                              }
                                              // --- Commit 响应处理 (不需要索引，直接透传) ---
                                              else if constexpr (std::is_same_v<CBType, MYMQ_Public::CommitAsyncResponceCallback>)
                                              {
                                                  if (auto* data = std::get_if<MYMQ_Public::CommitAsyncResponce>(&resp)) {
                                                      specific_cb(*data);
                                                  }
                                              }
                                              // --- Noop/Error 处理 (不需要索引，直接透传) ---
                                              else if constexpr (std::is_same_v<CBType, MYMQ_Public::CallbackNoop>)
                                              {
                                                  if (auto* err = std::get_if<MYMQ_Public::CommonErrorCode>(&resp)) {
                                                      specific_cb(*err);
                                                  } else {
                                                      specific_cb(MYMQ_Public::CommonErrorCode::Success);
                                                  }
                                              }
                                              else
                                              {
                                                  static_assert(MYMQ_Public::always_false_v<CBType>, "Unknown callback type");
                                              }

                                          }, current_cb);
                                      }
                                  }
                                  );
        return succ;
    }
    
    bool ClientBase::send_via(MYMQ::Network::Communication_client& channel, MYMQ::EventType event_type, const Mybyte& msg_body, std::vector<MYMQ::Client::SparseCallback> cbs_)
    {
        size_t curr_fly = SIZE_MAX;
        channel.get_curr_flying_request_num(curr_fly);
        if (curr_fly >= max_in_flight_requests_num) {
            return 0;
        }

        auto succ = channel.send_msg(static_cast<short>(event_type), msg_body,
                                  [this, saved_cbs = std::move(cbs_)]
                                  (uint16_t event_type_responce, MYMQ::OwnedBytes msg_body_responce) mutable
                                  {
                                      auto resp = handle_response(static_cast<Eve>(event_type_responce), msg_body_responce);

                                      for (auto& sparse_item : saved_cbs) {
                                          uint32_t msg_idx = sparse_item.relative_index;
                                          auto& current_cb = sparse_item.cb;

                                          std::visit([&](auto&& specific_cb) {
                                              using CBType = std::decay_t<decltype(specific_cb)>;

                                              if constexpr (std::is_same_v<CBType, MYMQ_Public::PushResponceCallback>)
                                              {
                                                  if (auto* data = std::get_if<MYMQ_Public::PushResponce>(&resp)) {
                                                      MYMQ_Public::PushResponce individual_resp = *data;
                                                      individual_resp.offset = data->offset + msg_idx;
                                                      specific_cb(individual_resp);
                                                  }
                                              }
                                              else if constexpr (std::is_same_v<CBType, MYMQ_Public::CommitAsyncResponceCallback>)
                                              {
                                                  if (auto* data = std::get_if<MYMQ_Public::CommitAsyncResponce>(&resp)) {
                                                      specific_cb(*data);
                                                  }
                                              }
                                              else if constexpr (std::is_same_v<CBType, MYMQ_Public::CallbackNoop>)
                                              {
                                                  if (auto* err = std::get_if<MYMQ_Public::CommonErrorCode>(&resp)) {
                                                      specific_cb(*err);
                                                  } else {
                                                      specific_cb(MYMQ_Public::CommonErrorCode::Success);
                                                  }
                                              }
                                              else
                                              {
                                                  static_assert(MYMQ_Public::always_false_v<CBType>, "Unknown callback type");
                                              }

                                          }, current_cb);
                                      }
                                  }
                                  );
        return succ;
    }

MYMQ_Consumeruse::MYMQ_Consumeruse(const std::string& clientid,uint8_t ack_level):ClientBase(MYMQ::run_directory_DEFAULT),path_(MYMQ::run_directory_DEFAULT),tbb_dctx_pool([]() {
        // 初始化函数：当新线程第一次访问时调用
        return ZSTD_createDCtx();
    }){

    Config_manager::ensure_path_existed(MYMQ::run_directory_DEFAULT);
    init(clientid,ack_level);
     cmc_.init();
     cmc_fetch_.init();
}
MYMQ_Consumeruse::~MYMQ_Consumeruse(){
    stop();

    for (ZSTD_DCtx* ctx : tbb_dctx_pool) {
        ZSTD_freeDCtx(ctx);
    }

}

void MYMQ_Consumeruse::stop() {
    bool expected = false;
    if (!stopped_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    out_group_reset();
    {
        std::lock_guard<std::mutex> lock(mtx_poll_ready);
        poll_ready.store(true);
    }
    cv_poll_ready.notify_all();
    commit_ready.store(true);
    cv_commit_ready.notify_all();
    cmc_fetch_.stop();
    cmc_.stop();
}


    void MYMQ_Consumeruse::heartbeat_start(){
        heartbeat_taskid= timer.commit_ms([this]{
            heartbeat();
        },heartbeat_interval_ms,heartbeat_interval_ms);
    }

    void MYMQ_Consumeruse::heartbeat_stop(){
        timer.commit_ms([this]{
            timer.cancel_task(heartbeat_taskid);
        },10,10,1);
    }




    void MYMQ_Consumeruse::autocommit_start(){
        autocommit_taskid= timer.commit_ms([this]{
            timer_commit_async();
        },autocommit_perior_ms,autocommit_perior_ms);
    }
    void MYMQ_Consumeruse::autocommit_stop(){
        timer.commit_ms([this]{
            timer.cancel_task(autocommit_taskid);
        },10,10,1);
    }

    Err_Client MYMQ_Consumeruse::seek(const MYMQ_Public::TopicPartition& tp,size_t offset_next_to_consume){


        TP_PointMap::const_accessor cac;
        if(!map_final_assign.find(cac,tp)){
            return Err_Client::INVALID_TOPIC_PARTITION;
        }
        auto pollqueue_ptr=cac->second.pollqueue_ptr;
        pollqueue_ptr->clear_for_seek(offset_next_to_consume);
        out("[Seek offset] Current offset : "+std::to_string(offset_next_to_consume));
       return Err_Client::Success;
    }



    void MYMQ_Consumeruse:: set_pull_max_record_num_local(size_t num){

        if(num==0){
            num=1;
            cerr("[SET MAX RECORD NUM]Warning : max record num must be 1 at least");
        }
        pull_max_record_num_local.store(num);
    }


    // ----------------------------------------------------------------------
    // 2. 完整的 Consumer (解析)
    // ----------------------------------------------------------------------
    void MYMQ_Consumeruse::call_parse_impl(
        const MYMQ::OwnedBytes& raw_big_chunk_owner,
        std::vector<MYMQ_Public::ConsumerRecord>& out_records,
        const MYMQ::Client::TopicPartition& tp,
        Err_Client& out_error
        ) {
        out_error = Err_Client::Success;

        if (raw_big_chunk_owner.data == nullptr || raw_big_chunk_owner.size == 0) return;

        ZSTD_DCtx* dctx = tbb_dctx_pool.local();

        auto ret = ClientProtocol::parse_record_batch(raw_big_chunk_owner, dctx, out_records, tp);

        if (ret != MYMQ_Public::CommonErrorCode::Success) {
            out_error = Err_Client::UNKNOWN_ERROR; // 简单映射错误
            return;
        }
    }





    Err_Client MYMQ_Consumeruse::get_local_consumed_position(const MYMQ_Public::TopicPartition& tp,size_t& pos){
        TP_PointMap::const_accessor cac;
        if(!map_final_assign.find(cac,tp)){
            return Err_Client::INVALID_TOPIC_PARTITION;
        }

        pos= cac->second.endoffset_ptr->off.load(std::memory_order_relaxed);
        return Err_Client::Success;
    }

    Err_Client MYMQ_Consumeruse::pull(std::vector<MYMQ_Public::ConsumerRecord>& record_batch, size_t poll_wait_timeout_ms) {
        // max_record_nums 即原 local_pull_bytes_once 的新语义，建议通过参数传入或作为成员变量

        if (!record_batch.empty()) return Err_Client::INVALID_OPRATION;

        // --- 1. 数据收集阶段 (Accumulate Phase) ---
        size_t accumulated_record_count = 0; // 新增：累计收集的记录条数

        // 计算绝对截止时间
        auto start_time = std::chrono::steady_clock::now();
        auto deadline = start_time + std::chrono::milliseconds(poll_wait_timeout_ms);

        size_t active_item_count = 0;

        while (true) {
            bool gained_new_data_this_round = false;

            for (auto& [tp, tp_point] : map_final_assign) {

                // 变更点 4: 适配 try_pop 返回 pair<size_t, vector>
                // first: 记录条数, second: 二进制数据块
                std::pair<size_t, MYMQ::OwnedBytes> popped_data;

                if (tp_point.pollqueue_ptr->try_pop(popped_data)) {
                    size_t batch_rec_num = popped_data.first;
                    MYMQ::OwnedBytes raw_chunk = std::move(popped_data.second);

                    // 只有 vector 非空才处理（防御性编程）
                    if (raw_chunk.data != nullptr && raw_chunk.size > 0) {
                        if (active_item_count < m_todo_cache.size()) {
                            auto& item = m_todo_cache[active_item_count];
                            item.index = active_item_count;
                            item.tp = tp;
                            item.raw_big_chunk_owner = std::move(raw_chunk);
                            item.err = Err_Client::Success;
                            item.parsed_records.clear();
                        }
                        else {
                            m_todo_cache.emplace_back(active_item_count, tp, std::move(raw_chunk));
                        }

                        // 指针后移
                        active_item_count++;

                        // 变更点 1: 累加条数而不是字节
                        accumulated_record_count += batch_rec_num;
                        gained_new_data_this_round = true;

                        // 变更点 3 (部分): 软限制检查
                        // 只要达到或超过最大条数，立即停止收集，进入解析
                        if (accumulated_record_count >= pull_max_record_num_local) {
                            goto PROCESS_PHASE;
                        }
                    }
                }
            }

            // --- 循环控制核心逻辑 ---

            // 情况 A: 这一轮循环拿到了数据，但还未达到 max_record_nums
            if (gained_new_data_this_round) {
                // 变更点 3: "掏空缓冲区" 策略
                // 不休眠，立即 continue 进行下一轮轮询，试图获取更多数据直到缓冲区变空
                continue;
            }

            // 情况 B: 这一轮循环完全没有拿到任何新数据 (所有队列都空了)
            else {
                // 变更点 3: 只要手头有数据，立刻返回，不等待
                if (accumulated_record_count > 0) {
                    goto PROCESS_PHASE;
                }

                // 变更点 2: 只有在 "手中无数据" 且 "队列为空" 时，才检查超时或进入等待

                // B.1 检查超时
                if (std::chrono::steady_clock::now() >= deadline) {
                    break; // 超时，跳出循环去处理（如果是空的则会在最后返回 EMPTY）
                }

                // B.2 等待逻辑 (Wait)
                {
                    std::unique_lock<std::mutex> ulock(mtx_poll_ready);
                    // 此时肯定没有数据，安心等待唤醒或超时
                    bool signaled = cv_poll_ready.wait_until(ulock, deadline, [this] {
                        return poll_ready.load();
                    });

                    if (signaled) {
                        poll_ready.store(false);
                        // 唤醒后 loop 继续，重新去 try_pop
                    } else {
                        break; // 等待超时
                    }
                }
            }
        }

    PROCESS_PHASE:

        if (m_todo_cache.empty()) {
            // 如果这里为空，说明是纯超时且未拿到任何数据
            return Err_Client::PULL_TIMEOUT;
        }

        // --- 2. 并行解析 (Parallel Parse) ---
        // 代码保持不变，解析逻辑通常不依赖于前面的计数方式
        tbb::parallel_for_each(m_todo_cache.begin(), m_todo_cache.begin() + active_item_count,
                               [this](Workitem& item) {
                                   this->call_parse_impl(item.raw_big_chunk_owner, item.parsed_records, item.tp, item.err);
                               });

        bool has_partial_error = false;

        // 预分配内存优化 (可选)
        // if (accumulated_record_count > 0) record_batch.reserve(record_batch.size() + accumulated_record_count);

        for (size_t i = 0; i < active_item_count; ++i) {
            auto& item = m_todo_cache[i];

            if (item.err != Err_Client::Success) {
                has_partial_error = true;
                continue;
            }

            if (!item.parsed_records.empty()) {
                // 这里的 insert 逻辑保持不变
                size_t needed = record_batch.size() + item.parsed_records.size();
                if (record_batch.capacity() < needed) {
                    record_batch.reserve(needed);
                }

                record_batch.insert(
                    record_batch.end(),
                    std::make_move_iterator(item.parsed_records.begin()),
                    std::make_move_iterator(item.parsed_records.end())
                    );
            }
        }

        // 清理缓存中的大对象，防止占用内存（视具体 Workitem 实现而定，如果是 swap 进去的则需要清理）
        // 通常建议在这里重置 active_item_count 为 0，或者在该方法入口处重置，
        // 但原代码逻辑似乎是复用 vector capacity，所以这里不需要析构，只需下一次覆盖即可。

        if (record_batch.empty() && has_partial_error) {
            return Err_Client::PARTIAL_PARASE_FAILED;
        }

        return record_batch.empty() ? Err_Client::EMPTY_RECORD : Err_Client::Success;
    }

    // 建议单位使用微秒 (us) 以获得更高精度，如果需要毫秒改为 milliseconds 即可
    Err_Client MYMQ_Consumeruse::pull(std::vector<MYMQ_Public::ConsumerRecord>& record_batch,
                                      size_t poll_wait_timeout_ms,
                                      int64_t& out_latency_us) {

        // --- 0. 初始化计时器 ---
        auto start_time = std::chrono::steady_clock::now();
        // 用于累计纯等待（Sleep/Wait）的时间
        std::chrono::microseconds total_wait_duration(0);

        // 定义计算纯净耗时的 Lambda
        auto update_pure_latency = [&]() {
            auto now = std::chrono::steady_clock::now();
            auto total_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - start_time);
            // 纯净耗时 = 总自然流逝时间 - 睡大觉的时间
            out_latency_us = (total_elapsed - total_wait_duration).count();
        };

        if (!record_batch.empty()) {
            out_latency_us = 0;
            return Err_Client::INVALID_OPRATION;
        }

        // --- 1. 数据收集阶段 (Accumulate Phase) ---
        size_t accumulated_record_count = 0; // [同步更新] 使用条数计数

        auto deadline = start_time + std::chrono::milliseconds(poll_wait_timeout_ms);

        size_t active_item_count = 0;

        while (true) {
            bool gained_new_data_this_round = false;

            for (auto& [tp, tp_point] : map_final_assign) {

                // [同步更新] 适配 try_pop 返回 pair
                std::pair<size_t, MYMQ::OwnedBytes> popped_data;

                // try_pop 是内存/锁操作，属于有效工作时间
                if (tp_point.pollqueue_ptr->try_pop(popped_data)) {
                    size_t batch_rec_num = popped_data.first;
                    MYMQ::OwnedBytes raw_chunk = std::move(popped_data.second);

                    if (raw_chunk.data != nullptr && raw_chunk.size > 0) {
                        // 缓存复用逻辑
                        if (active_item_count < m_todo_cache.size()) {
                            auto& item = m_todo_cache[active_item_count];
                            item.index = active_item_count;
                            item.tp = tp;
                            item.raw_big_chunk_owner = std::move(raw_chunk);
                            item.err = Err_Client::Success;
                            item.parsed_records.clear();
                        }
                        else {
                            m_todo_cache.emplace_back(active_item_count, tp, std::move(raw_chunk));
                        }

                        active_item_count++;

                        // [同步更新] 累加条数
                        accumulated_record_count += batch_rec_num;
                        gained_new_data_this_round = true;

                        // [同步更新] 软限制检查
                        if (accumulated_record_count >= pull_max_record_num_local) {
                            goto PROCESS_PHASE;
                        }
                    }
                }
            }

            // --- 循环控制核心逻辑 ---

            // 情况 A: 这一轮循环拿到了数据 -> "掏空缓冲区" 策略
            if (gained_new_data_this_round) {
                // 不休眠，不计算等待时间，立即进行下一轮
                continue;
            }
            // 情况 B: 这一轮循环完全没有拿到任何新数据
            else {
                // [同步更新] 只要手头有数据，立刻返回处理，不等待
                if (accumulated_record_count > 0) {
                    goto PROCESS_PHASE;
                }

                // B.1 检查超时
                if (std::chrono::steady_clock::now() >= deadline) {
                    break;
                }

                // B.2 等待逻辑 (Wait) -> 只有这里需要计入 wait_duration
                {
                    std::unique_lock<std::mutex> ulock(mtx_poll_ready);

                    // === [计时] 开始等待 ===
                    auto wait_start = std::chrono::steady_clock::now();

                    bool signaled = cv_poll_ready.wait_until(ulock, deadline, [this] {
                        return poll_ready.load();
                    });

                    auto wait_end = std::chrono::steady_clock::now();
                    // === [计时] 结束等待，累加无效时间 ===
                    total_wait_duration += std::chrono::duration_cast<std::chrono::microseconds>(wait_end - wait_start);

                    if (signaled) {
                        poll_ready.store(false);
                        // 唤醒后 loop 继续
                    } else {
                        break; // 等待超时
                    }
                }
            }
        }

    PROCESS_PHASE:

        if (m_todo_cache.empty()) {
            update_pure_latency(); // 计算最终耗时
            return Err_Client::PULL_TIMEOUT;
        }

        // --- 2. 并行解析 (Parallel Parse) ---
        // CPU 密集型操作，属于有效工作时间
        tbb::parallel_for_each(m_todo_cache.begin(), m_todo_cache.begin() + active_item_count,
                               [this](Workitem& item) {
                                   this->call_parse_impl(item.raw_big_chunk_owner, item.parsed_records, item.tp, item.err);
                               });

        bool has_partial_error = false;

        for (size_t i = 0; i < active_item_count; ++i) {
            auto& item = m_todo_cache[i];

            if (item.err != Err_Client::Success) {
                has_partial_error = true;
                continue;
            }

            if (!item.parsed_records.empty()) {
                size_t needed = record_batch.size() + item.parsed_records.size();
                if (record_batch.capacity() < needed) {
                    record_batch.reserve(needed);
                }

                // 内存拷贝，属于有效工作时间
                record_batch.insert(
                    record_batch.end(),
                    std::make_move_iterator(item.parsed_records.begin()),
                    std::make_move_iterator(item.parsed_records.end())
                    );
            }
        }

        // --- 3. 最终耗时计算 ---
        update_pure_latency();

        if (record_batch.empty() && has_partial_error) {
            return Err_Client::PARTIAL_PARASE_FAILED;
        }

        return record_batch.empty() ? Err_Client::EMPTY_RECORD : Err_Client::Success;
    }
    void MYMQ_Consumeruse::create_topic(const std::string& topicname,size_t parti_num){
        auto req = ClientProtocol::build_create_topic_packet(topicname, parti_num);
        send(Eve::CLIENT_REQUEST_CREATE_TOPIC, req);
    }
    void MYMQ_Consumeruse::set_pull_fetch_min_bytes(size_t bytes){
        if(bytes>1&&bytes<=MYMQ::pull_bytes_max){
            pull_fetch_min_bytes.store(bytes);
        }
        else{
            cerr("Set pull bytes : OUT OF LIMITATION");
        }

    }

    void MYMQ_Consumeruse::subscribe_topic(const std::string& topicname){
        std::unique_lock<std::shared_mutex> ulock(info_basic.mtx);
        info_basic.subscribed_topics.insert(topicname);
    }

    void MYMQ_Consumeruse::unsubscribe_topic(const std::string& topicname){
         std::unique_lock<std::shared_mutex> ulock(info_basic.mtx);
        info_basic.subscribed_topics.erase(topicname);
    }
    Err_Client MYMQ_Consumeruse::commit_sync(const MYMQ_Public::TopicPartition& tp,size_t next_offset_to_consume) {
        if(is_auto_commit){
            return Err_Client::AUTOCOMMIT_ENABLE;
        }
        if(!is_ingroup.load()){
            return Err_Client::NOT_IN_GROUP;
        }
        auto res= commit_inter(tp,next_offset_to_consume,MYMQ_Public::CommitAsyncResponceCallback());
        if(res!=Err_Client::Success){
            return res;
        }
        out("[Commit offset] Offset '"+std::to_string( next_offset_to_consume)+"' waitting committment over .");


        bool commit_responced=0;
        {
            std::unique_lock<std::mutex> ulock(mtx_commit_ready);
            commit_responced = cv_commit_ready.wait_for(ulock,std::chrono::seconds(commit_wait_timeout_s),[this]{
                return commit_ready.load();
            });

        }
        commit_ready.store(0);

        if(!commit_responced){
            cerr("Commit SYNC failed : Responce timeout");
            return Err_Client::COMMIT_SYNC_TIMEOUT;
        }

        return Err_Client::Success;

    }



    Err_Client MYMQ_Consumeruse::commit_async(const MYMQ_Public::TopicPartition& tp,size_t next_offset_to_consume,MYMQ_Public::CommitAsyncResponceCallback cb) {
        if(is_auto_commit){
            return Err_Client::AUTOCOMMIT_ENABLE;
        }
        if(!is_ingroup.load()){
            return Err_Client::NOT_IN_GROUP;
        }
        return commit_inter(tp,next_offset_to_consume,cb);

    }




    Err_Client MYMQ_Consumeruse::join_group(const std::string& groupid){


        if(groupid.empty()&&groupid.length()>30){
           return Err_Client::INVALID_GROUPID;
        }

        heartbeat(1,groupid);

        return Err_Client::Success;

    }

    Err_Client MYMQ_Consumeruse::leave_group(){
        if(!is_ingroup.load()){
            return Err_Client::NOT_IN_GROUP;
        }

        std::string groupid;
        std::string memberid;
        {
            std::shared_lock<std::shared_mutex>  slock(info_basic.mtx);
            groupid=info_basic.groupid;
            memberid=info_basic.memberid;
        }

        auto req = ClientProtocol::build_leave_group_packet(groupid, memberid);
        send(Eve::CLIENT_REQUEST_LEAVE_GROUP, req);
        return Err_Client::Success;

    }


    void MYMQ_Consumeruse::heartbeat(bool topics_updated,std::string groupid) {



        std::string memberid{""};
        size_t gen_id=0;
        std::set<std::string> topics;
        std::string clientid;

        bool is_join=0;


            if(groupid.empty()){//非入组
                 std::shared_lock<std::shared_mutex>  slock(info_basic.mtx);
             groupid=info_basic.groupid;
             memberid=info_basic.memberid;
             gen_id=info_basic.generation_id;
            }
            else{//入组
                is_join=1;
                 std::shared_lock<std::shared_mutex>  slock(info_basic.mtx);
                clientid=info_basic.clientid;
            }
            if(topics_updated){
            topics=info_basic.subscribed_topics;
        }

        auto req = ClientProtocol::build_heartbeat_packet(
            groupid, memberid, gen_id, static_cast<uint16_t>(pull_start_location), is_join, clientid, topics_updated, topics
        );
        send(MYMQ::EventType::CLIENT_REQUEST_HEARTBEAT, req);
    }



    std::unordered_set<MYMQ_Public::TopicPartition> MYMQ_Consumeruse::get_assigned_partition(){
        std::unordered_set<MYMQ_Public::TopicPartition> res{};
        if(!is_ingroup.load()){
            return res;
        }
        for(const auto& [tp,tp_point]:map_final_assign){
            res.insert(tp);
        }
        return res;
    }


    void MYMQ_Consumeruse::init(const std::string& clientid,uint8_t ack_level){

        {

            Config_manager cm_sys(path_+"\\config\\sys.ini");

            auto thread_corenum= cm_sys.getint("max_threadnum_client");

            ThreadPool::instance(thread_corenum).start();


            auto zstd_level_tmp= cm_sys.getint("zstd_level");
            if(!inrange(zstd_level_tmp,0,22)){
                zstd_level=MYMQ::zstd_level_DEFAULT;
            } else {
                zstd_level=zstd_level_tmp;
            }
            dctx=ZSTD_createDCtx();
            heartbeat_interval_ms=MYMQ::HEARTBEAT_MS_CLIENT;
            auto tmp_clientid=clientid;
            if(!inrange(tmp_clientid.size(),1,30)){
                tmp_clientid=MYMQ::CLIENTID_DEFAULT;
                cerr("Initialization : Invaild 'clientid' in config . Use default 'clientid' : "+MYMQ::CLIENTID_DEFAULT);
            }
            auto make_role_clientid = [](const std::string& base, const std::string& suffix) {
                constexpr size_t kMaxClientIdLen = 30;
                if (suffix.size() >= kMaxClientIdLen) {
                    return suffix.substr(0, kMaxClientIdLen);
                }
                if (base.size() + suffix.size() <= kMaxClientIdLen) {
                    return base + suffix;
                }
                return base.substr(0, kMaxClientIdLen - suffix.size()) + suffix;
            };
            cmc_.set_clientid(tmp_clientid);
            cmc_.set_channel_role(MYMQ_Public::ChannelRole::CONTROL);
            cmc_fetch_.set_clientid(make_role_clientid(tmp_clientid, "#fetch"));
            cmc_fetch_.set_channel_role(MYMQ_Public::ChannelRole::FETCH);
            cmc_fetch_.set_request_timeout_ms(60000);
            {
                std::unique_lock<std::shared_mutex> ulock(info_basic.mtx);
                info_basic.clientid=tmp_clientid;
            }

            auto tmp_ack_level=ack_level;
            if(!inrange(tmp_ack_level,0,1)){
                tmp_ack_level=MYMQ::ack_level_DEFAULT;
                 cerr("Initialization : Invaild 'ack_level' in config . Use default 'ack_level' : "+std::to_string(MYMQ::ack_level_DEFAULT));
            }
            cmc_.set_ACK_level(static_cast<MYMQ::ACK_Level>(tmp_ack_level));
            cmc_fetch_.set_ACK_level(static_cast<MYMQ::ACK_Level>(tmp_ack_level));
            ack_level_=static_cast<MYMQ::ACK_Level>(tmp_ack_level);


            size_t max_in_flight_requests_num_tmp= cm_sys.get_size_t("max_in_flight_requests_num");
            if(!inrange(max_in_flight_requests_num_tmp,1,5000000)){
                max_in_flight_requests_num_tmp=MYMQ:: MAX_IN_FLIGHT_REQUEST_NUM_DEFAULT;
                cerr("Initialization : Invaild 'max_in_flight_requests_num' in config . Use default 'max_in_flight_requests_num' : "+std::to_string(MYMQ:: MAX_IN_FLIGHT_REQUEST_NUM_DEFAULT));
            }
            max_in_flight_requests_num=max_in_flight_requests_num_tmp;

            size_t local_pollqueue_size_cfg = 20000000;
            try {
                local_pollqueue_size_cfg = cm_sys.get_size_t("local_pollqueue_size");
            } catch (...) {
                local_pollqueue_size_cfg = 20000000;
            }

            size_t high_bytes = local_pollqueue_size_cfg;
            if (high_bytes > 0 && high_bytes < 1024 * 1024) {
                high_bytes *= 1024;
            }
            if (!inrange(high_bytes, 1024 * 1024, 2ULL * 1024 * 1024 * 1024)) {
                high_bytes = 20000000;
            }
            local_pollqueue_high_bytes = high_bytes;
            local_pollqueue_low_bytes = high_bytes / 2;
            if (local_pollqueue_low_bytes < 1024 * 1024) {
                local_pollqueue_low_bytes = 1024 * 1024;
            }
        }


        {

            Config_manager cm_business(path_+"\\config\\business.ini");
            is_auto_commit=cm_business.getbool("autocommit");
            pull_start_location=static_cast<MYMQ::PullSet>( cm_business.get_uint16("pull_start_loc_option"));

            rebalance_timeout_ms=MYMQ::rebalance_timeout_ms;
            join_collect_timeout_ms=MYMQ::join_collect_timeout_ms;
            memberid_wait_timeout_s=MYMQ::memberid_ready_timeout_s;
            commit_wait_timeout_s=MYMQ::commit_ready_timeout_s;
            size_t pull_fetch_min_bytes_cfg = cm_business.get_size_t("pull_fetch_min_bytes");
            if (pull_fetch_min_bytes_cfg <= 1 || pull_fetch_min_bytes_cfg > MYMQ::pull_bytes_max) {
                pull_fetch_min_bytes_cfg = MYMQ::pull_bytes_max;
                cerr("Initialization : Invaild 'pull_fetch_min_bytes' in config . Clamp to " + std::to_string(MYMQ::pull_bytes_max));
            }
            pull_fetch_min_bytes = pull_fetch_min_bytes_cfg;
            batch_size=cm_business.get_size_t("batch_size");
            autopush_perior_ms=cm_business.get_size_t ("autopush_perior_ms");
            autocommit_perior_ms=cm_business.get_size_t ("autocommit_perior_ms");
            pull_max_record_num_local=cm_business.get_size_t("pull_max_record_num_local");

        }

    }



    void MYMQ_Consumeruse::timer_commit_async(){
        if(!is_ingroup.load()){
            return ;
        }


        for(const auto&[tp, tp_point]:map_final_assign){
             commit_async(tp,tp_point.endoffset_ptr->off.load(std::memory_order_relaxed));
        }
    }



    Err_Client MYMQ_Consumeruse::commit_inter(const MYMQ_Public::TopicPartition& tp,size_t next_offset_to_consume,MYMQ_Public::CommitAsyncResponceCallback cb){
        if(!is_ingroup.load()){
            return Err_Client::NOT_IN_GROUP;
        }

        TP_PointMap::const_accessor cac;
        auto it=map_final_assign.find(cac,tp);
        if(!it){
            cerr("ERROR :Invalid topic or partition in 'commit_inter' function");
            return MYMQ_Public::ClientErrorCode::INVALID_TOPIC_PARTITION;
        }

        size_t now_off =cac->second.endoffset_ptr->off.load(std::memory_order_relaxed);
        cac.release();
          out("[Commit offset] Current offset : "+std::to_string(now_off));
        if(now_off>next_offset_to_consume){
            cerr("Warning : Attempt to commit an offset older than local record: "+std::to_string(next_offset_to_consume));
        }



        std::string groupid;
        size_t genid;
        std::string memberid;
        {
            std::shared_lock<std::shared_mutex> slock(info_basic.mtx);
            groupid =info_basic.groupid  ;
            genid=info_basic.generation_id;
            memberid=info_basic.memberid;
        }

        auto req = ClientProtocol::build_commit_offset_packet(
            groupid, memberid, genid, tp.topic, tp.partition, next_offset_to_consume
        );

        bool has_callback = (bool)cb;
        if(has_callback){
            std::vector<MYMQ::Client::SparseCallback> cbs_;
            cbs_.reserve(1);
            cbs_.push_back({static_cast<uint32_t>(0), std::move(cb)});
            send(Eve::CLIENT_REQUEST_COMMIT_OFFSET, req, std::move(cbs_));

        }
        else{
            send(Eve::CLIENT_REQUEST_COMMIT_OFFSET, req);
        }


        return MYMQ_Public::ClientErrorCode::Success;
    }

    MYMQ_Public::ResultVariant MYMQ_Consumeruse::handle_response(Eve event_type,const MYMQ::OwnedBytes& msg_body){
        auto protocol_resp = ClientProtocol::parse_response(event_type, msg_body);

        return std::visit([this](auto&& arg) -> MYMQ_Public::ResultVariant {
            using T = std::decay_t<decltype(arg)>;

            if constexpr (std::is_same_v<T, MYMQ_Public::PushResponce>) {
                return arg;
            }
            else if constexpr (std::is_same_v<T, MYMQ_Public::CommitAsyncResponce>) {
                return arg;
            }
            else if constexpr (std::is_same_v<T, MYMQ_Public::CommonErrorCode>) {
                if (arg == MYMQ_Public::CommonErrorCode::REQUEST_TIMEOUT) {
                     return arg;
                }
                return arg;
            }
            else if constexpr (std::is_same_v<T, PullResponseData>) {
                auto& resp = arg;
                out("[PULL] Messages batch from (TOPIC '" + resp.topic + "' PARTITION '" + std::to_string(resp.partition) + ") responce reached. ");
                out(std::string{} + "[PULL] Result : " + " State : " + MYMQ_Public::to_string(resp.error));

                if (resp.error == MYMQ_Public::CommonErrorCode::NO_RECORD) {
                    if (!is_ingroup.load()) {
                        return MYMQ_Public::CommonErrorCode::CLIENT_NOT_IN_GROUP;
                    }
                    TP_PointMap::accessor cac;
                    if (!map_final_assign.find(cac, TopicPartition(resp.topic, resp.partition))) {
                        return MYMQ_Public::CommonErrorCode::Success;
                    }
                    auto pollqueue_ptr = cac->second.pollqueue_ptr;
                    cac.release();
                    if (!pollqueue_ptr->need_poll()) {
                        return MYMQ_Public::CommonErrorCode::Success;
                    }
                    size_t bytes = pull_fetch_min_bytes.load();
                    std::string group_id;
                    {
                        std::shared_lock<std::shared_mutex> slock(info_basic.mtx);
                        group_id = info_basic.groupid;
                    }
                    auto req = ClientProtocol::build_pull_packet(group_id, resp.topic, resp.partition, resp.next_offset, bytes);
                    send_via(cmc_fetch_, Eve::CLIENT_REQUEST_PULL, req);
                    return MYMQ_Public::CommonErrorCode::Success;
                }

                if (resp.error != MYMQ_Public::CommonErrorCode::Success) {
                    return resp.error;
                }

                bool need_poll = false;
                if (resp.error == MYMQ_Public::CommonErrorCode::Success) {
                    TP_PointMap::accessor cac;
                    if (map_final_assign.find(cac, TopicPartition(resp.topic, resp.partition))) {
                        auto pollqueue_ptr = cac->second.pollqueue_ptr;
                        cac.release();

                        pollqueue_ptr->local_consume_offset = resp.next_offset;
                        pollqueue_ptr->push(resp.message_batch, resp.record_num);

                        {
                            std::lock_guard<std::mutex> lock(mtx_poll_ready);
                            poll_ready.store(true);
                        }
                        cv_poll_ready.notify_one();
                        need_poll = pollqueue_ptr->need_poll();
                    }

                    if (need_poll) {
                         if (!is_ingroup.load()) {
                            return MYMQ_Public::CommonErrorCode::CLIENT_NOT_IN_GROUP;
                        }
                        size_t bytes = pull_fetch_min_bytes.load();

                        std::string group_id;
                        {
                            std::shared_lock<std::shared_mutex> slock(info_basic.mtx);
                            group_id = info_basic.groupid;
                        }
                        
                        auto req = ClientProtocol::build_pull_packet(group_id, resp.topic, resp.partition, resp.next_offset, bytes);
                        send_via(cmc_fetch_, Eve::CLIENT_REQUEST_PULL, req);
                    }
                }
                return MYMQ_Public::CommonErrorCode::Success;
            }
            else if constexpr (std::is_same_v<T, LeaveGroupResponse>) {
                const auto& resp = arg;
                if (resp.error == MYMQ_Public::CommonErrorCode::Success) {
                    out_group_reset();
                    out("Leave Group : Leaved Group '" + resp.group_id + "'");
                } else {
                    cerr("Leave Group : Leaving Group '" + resp.group_id + "' : " + MYMQ_Public::to_string(resp.error));
                }
                return MYMQ_Public::CommonErrorCode::Success;
            }
            else if constexpr (std::is_same_v<T, HeartbeatResponse>) {
                const auto& resp = arg;
                 if (resp.error == MYMQ_Public::CommonErrorCode::UPDATE_GENERATION) {
                    std::unordered_map<TopicPartition, size_t> new_assignment_map;
                    new_assignment_map.reserve(resp.assignments.size());

                    for (const auto& assignment : resp.assignments) {
                        TopicPartition tp(assignment.topic, assignment.partition);
                        new_assignment_map[tp] = assignment.end_offset;
                    }

                    std::vector<TopicPartition> to_remove;
                    for (auto it = map_final_assign.begin(); it != map_final_assign.end(); ++it) {
                        if (new_assignment_map.find(it->first) == new_assignment_map.end()) {
                            to_remove.push_back(it->first);
                        }
                    }
                    for (const auto& tp : to_remove) {
                        map_final_assign.erase(tp);
                    }

                    for (const auto& [tp, server_offset] : new_assignment_map) {
                        TP_PointMap::accessor ac;
                        if (map_final_assign.insert(ac, tp)) {
                            ac->second.endoffset_ptr = std::make_shared<MYMQ::Client::Commitedoffset_point>(server_offset, tp);
                            ac->second.pollqueue_ptr = std::make_shared<MYMQ::Client::PollBuffer>(local_pollqueue_low_bytes, local_pollqueue_high_bytes);
                        }
                    }

                    {
                        std::unique_lock<std::shared_mutex> ulock(info_basic.mtx);
                        info_basic.groupid = resp.group_id;
                        info_basic.generation_id = resp.generation_id;
                        info_basic.memberid = resp.member_id;
                    }

                    cerr("[Heartbeat] Generation updated to " + std::to_string(resp.generation_id) + ". Partitions Rebalanced.");
                    is_ingroup.store(1);

                    heartbeat_stop();
                    heartbeat_start();
                    if (is_auto_commit) {
                        autocommit_stop();
                        autocommit_start();
                    }
                    trigger_poll_for_low_cap_pollbuffer();

                } else if (resp.error != MYMQ_Public::CommonErrorCode::Success) {
                    cerr("[Heartbeat] Error : " + MYMQ_Public::to_string(resp.error));
                }
                return MYMQ_Public::CommonErrorCode::Success;
            }
            else if constexpr (std::is_same_v<T, CreateTopicResponse>) {
                if (arg.success) {
                    cerr("CREATE TOPIC RESULT : Topic created successfully");
                } else {
                    cerr("CREATE TOPIC RESULT : Topic created failed");
                }
                return MYMQ_Public::CommonErrorCode::Success;
            }
            else if constexpr (std::is_same_v<T, CommitOffsetResponse>) {
                const auto& resp = arg;
                if (!get_is_ingroup()) {
                    cerr("Get commit respose but now not in group");
                    return (MYMQ_Public::ResultVariant)MYMQ_Public::CommonErrorCode::GENERATION_EXPIRED;
                }

                std::shared_lock<std::shared_mutex> slock(info_basic.mtx);
                bool expired = (resp.error == MYMQ_Public::CommonErrorCode::GENERATION_EXPIRED ||
                               !(resp.group_id == info_basic.groupid && resp.member_id == info_basic.memberid && resp.generation_id == info_basic.generation_id));
                slock.unlock();

                if (expired) {
                    out("[Commit offset] Get commit respose but it has already expired.");
                    heartbeat();
                    return MYMQ_Public::CommonErrorCode::GENERATION_EXPIRED;
                }

                if (resp.error == MYMQ_Public::CommonErrorCode::Success) {
                    TP_PointMap::const_accessor cac;
                    if (!map_final_assign.find(cac, TopicPartition(resp.topic, resp.partition))) {
                        cerr("Error : Failed to update committed offset : Invalid topic or partition .");
                        return MYMQ_Public::CommonErrorCode::INTERNAL_ERROR;
                    }
                    cac->second.endoffset_ptr->off.store(resp.offset, std::memory_order_release);
                    cerr("Commit SYNC result : Success to commit '" + std::to_string(resp.offset) + "'");
                } else {
                    cerr("Commit SYNC result : Failed to commit '" + std::to_string(resp.offset) + "'");
                }

                commit_ready.store(1);
                cv_commit_ready.notify_all();

                return MYMQ_Public::CommonErrorCode::Success;
            }
            else {
                return MYMQ_Public::CommonErrorCode::Success;
            }
        }, protocol_resp);
    }

    void MYMQ_Consumeruse::out_group_reset(){
        is_ingroup.store(0);
        {
            std::string tmp{};
            std::unordered_map<std::string,std::unordered_set<MYMQ_Public::TopicPartition> > tmp1{};
            map_final_assign.clear();
        }
        if(is_auto_commit){
                    autocommit_stop();
        }

        heartbeat_stop();

    }




    void MYMQ_Consumeruse::trigger_poll_for_low_cap_pollbuffer(){
        if(!is_ingroup.load()){
            return ;
        }
        std::string groupid;
        {
            std::shared_lock<std::shared_mutex> slock(info_basic.mtx);
            groupid= info_basic.groupid;

        }

        for(const auto& [tp,tp_point]:map_final_assign){
            auto ptr=tp_point.pollqueue_ptr;
            auto& pollqueue=*ptr;
            if(!pollqueue.need_poll()){
                continue;
            }
            size_t now_off= pollqueue.local_consume_offset.load();
            size_t bytes= pull_fetch_min_bytes.load();
            auto req = ClientProtocol::build_pull_packet(groupid, tp.topic, tp.partition, now_off, bytes);
            send_via(cmc_fetch_, Eve::CLIENT_REQUEST_PULL, req);

        }




    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    //Producer


    MYMQ_Produceruse::MYMQ_Produceruse(const std::string& clientid,uint8_t ack_level):ClientBase(MYMQ::run_directory_DEFAULT),path_(MYMQ::run_directory_DEFAULT),tbb_dctx_pool([]() {
            // 初始化函数：当新线程第一次访问时调用
            return ZSTD_createDCtx();
        }){

        Config_manager::ensure_path_existed(MYMQ::run_directory_DEFAULT);
        init(clientid,ack_level);
        cmc_.init();
        cmc_produce_.init();
    }
    MYMQ_Produceruse::~MYMQ_Produceruse(){
        stop();

        for (ZSTD_DCtx* ctx : tbb_dctx_pool) {
            ZSTD_freeDCtx(ctx);
        }

    }

    void MYMQ_Produceruse::stop() {
        bool expected = false;
        if (!stopped_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }

        push_perioric_stop();

        struct PendingCb {
            TopicPartition tp;
            uint32_t rel_index = 0;
            MYMQ_Public::SupportedCallbacks cb;
        };

        std::vector<PendingCb> pending;

        for (auto it = recordaccumulator.begin(); it != recordaccumulator.end(); ++it) {
            auto& pq = *((*it).second);

            std::unique_lock<std::mutex> ulock(pq.mtx);

            if (pq.active_buf) {
                for (auto& scb : pq.active_cbs) {
                    pending.push_back(PendingCb{pq.tp, scb.relative_index, std::move(scb.cb)});
                }
                pq.active_cbs.clear();
                pq.current_batch_count = 0;
                pq.active_buf->release();
            }

            while (!pq.ready_queue.empty()) {
                auto item = std::move(pq.ready_queue.front());
                pq.ready_queue.pop_front();
                if (item) {
                    for (auto& scb : item->callbacks) {
                        pending.push_back(PendingCb{pq.tp, scb.relative_index, std::move(scb.cb)});
                    }
                    if (item->buffer) {
                        item->buffer->release();
                        pq.free_pool.push_back(std::move(item->buffer));
                    }
                }
            }

            for (auto& buf : pq.free_pool) {
                if (buf) {
                    buf->release();
                }
            }

            pq.is_flushing = false;
            pq.cv_full.notify_all();
        }

        for (auto& p : pending) {
            std::visit([&](auto&& specific_cb) {
                using CBType = std::decay_t<decltype(specific_cb)>;
                if constexpr (std::is_same_v<CBType, MYMQ_Public::PushResponceCallback>) {
                    specific_cb(MYMQ_Public::PushResponce(p.tp.topic, p.tp.partition, MYMQ_Public::CommonErrorCode::PRODUCER_STOPPED, 0));
                } else if constexpr (std::is_same_v<CBType, MYMQ_Public::CommitAsyncResponceCallback>) {
                    specific_cb(MYMQ_Public::CommitAsyncResponce(std::string{}, p.tp, 0, MYMQ_Public::CommonErrorCode::PRODUCER_STOPPED));
                } else if constexpr (std::is_same_v<CBType, MYMQ_Public::CallbackNoop>) {
                    specific_cb(MYMQ_Public::CommonErrorCode::PRODUCER_STOPPED);
                } else {
                    static_assert(MYMQ_Public::always_false_v<CBType>, "Unknown callback type");
                }
            }, p.cb);
        }

        cmc_produce_.drain_inflight_with_error(MYMQ_Public::CommonErrorCode::PRODUCER_STOPPED);
        cmc_produce_.stop();
        cmc_.stop();
    }

    void MYMQ_Produceruse::inject_pending_callbacks_for_test(const MYMQ_Public::TopicPartition& tp, size_t active_count, size_t ready_batches, MYMQ_Public::PushResponceCallback cb) {
        auto pq_ptr = recordaccumulator.get_queue(tp, batch_size, push_max_queued_batches);
        auto& pq = *pq_ptr;
        std::unique_lock<std::mutex> ulock(pq.mtx);

        if (!pq.active_buf) {
            pq.active_buf = std::make_unique<MYMQ::MSG_serial::BatchBuffer>(pq.buffer_size_);
        }

        pq.active_cbs.clear();
        pq.active_cbs.reserve(pq.active_cbs.size() + active_count);
        for (size_t i = 0; i < active_count; ++i) {
            pq.active_cbs.push_back(MYMQ::Client::SparseCallback{static_cast<uint32_t>(i), cb});
        }
        pq.current_batch_count = active_count;

        for (size_t i = 0; i < ready_batches; ++i) {
            auto item = std::make_unique<MYMQ::Client::BatchItem>();
            item->buffer = std::make_unique<MYMQ::MSG_serial::BatchBuffer>(pq.buffer_size_);
            item->buffer->ensure_allocated_for(std::chrono::milliseconds(0));
            item->callbacks.push_back(MYMQ::Client::SparseCallback{0U, cb});
            item->batch_count = 1;
            pq.ready_queue.push_back(std::move(item));
        }
    }
    Err_Client MYMQ_Produceruse::push(const MYMQ_Public::TopicPartition& tp, const std::string& key, const std::string& value, MYMQ_Public::PushResponceCallback cb) {

        // 1. 基础状态检查

        if (stopped_.load(std::memory_order_acquire)) {
            if (cb && ack_level_ != MYMQ::ACK_Level::ACK_NORESPONCE) {
                cb(MYMQ_Public::PushResponce(tp.topic, tp.partition, MYMQ_Public::CommonErrorCode::PRODUCER_STOPPED, 0));
            }
            return Err_Client::PRODUCER_STOPPED;
        }

        if(!is_register()){
            if (cb) {
                cb(MYMQ_Public::PushResponce(tp.topic, tp.partition, MYMQ_Public::CommonErrorCode::CLIENT_LINK_NOT_FOUND, 0));
            }
            return Err_Client::NOT_REGISTER;
        }

        size_t curr_fly = SIZE_MAX;
        cmc_produce_.get_curr_flying_request_num(curr_fly);
        if (curr_fly >= max_in_flight_requests_num) {
            cerr("[PUSH] FLYING REQUEST GOT TO LIMIT");
            if (cb && ack_level_ != MYMQ::ACK_Level::ACK_NORESPONCE) {
                cb(MYMQ_Public::PushResponce(tp.topic, tp.partition, MYMQ_Public::CommonErrorCode::QUEUE_FULL, 0));
            }
            return Err_Client::REACHED_MAX_FLYING_REQUEST;
        }


        auto pq_ptr = recordaccumulator.get_queue(tp, batch_size, push_max_queued_batches);
        auto& push_queue = *pq_ptr;
        std::unique_lock<std::mutex> ulock(push_queue.mtx);

        // 3. 检查压缩上下文
        if (!push_queue.cctx) {
            cerr("ZSTD ERROR : CCTX Unavailable . Push Interrupt");
            if (cb) {
                cb(MYMQ_Public::PushResponce(tp.topic, tp.partition, MYMQ_Public::CommonErrorCode::INTERNAL_ERROR, 0));
            }
            return Err_Client::ZSTD_UNAVAILABLE;
        }

        bool has_callback = (bool)cb;

        // 检查：如果用户传了有效回调，但客户端配置为“不响应”，则报错
        if (has_callback && ack_level_ == MYMQ::ACK_Level::ACK_NORESPONCE) {
            cerr("WARNING: Callback provided but ignored due to ACK_NORESPONCE level.");
            cb(MYMQ_Public::PushResponce(tp.topic, tp.partition, MYMQ_Public::CommonErrorCode::INTERNAL_ERROR, 0));
            return Err_Client::INVALID_OPRATION;
        }

        constexpr auto kAllocTimeout = std::chrono::milliseconds(100);
        constexpr auto kQueueWaitTimeout = std::chrono::milliseconds(100);

        while (true) {
            if (!push_queue.active_buf->ensure_allocated_for(kAllocTimeout)) {
                if (has_callback && ack_level_ != MYMQ::ACK_Level::ACK_NORESPONCE) {
                    cb(MYMQ_Public::PushResponce(tp.topic, tp.partition, MYMQ_Public::CommonErrorCode::TIMEOUT, 0));
                }
                return Err_Client::TIMEOUT;
            }

            // A. 尝试直接写入 Active Buffer
            bool success = push_queue.active_buf->append_record(key, value);

            if (success) {
                // [核心逻辑修正]
                // 只有当回调对象“非空” 且 配置允许响应时，才入队保存
                if (has_callback && ack_level_ != MYMQ::ACK_Level::ACK_NORESPONCE) {
                    push_queue.active_cbs.push_back({
                        static_cast<uint32_t>(push_queue.current_batch_count),
                        std::move(cb) // 移动语义，存入后外部cb失效
                    });
                }

                push_queue.current_batch_count++;

                return Err_Client::Success;
            }

            // --- Active Buffer 已满，需要轮转 ---

            // 1. 背压检查 (Backpressure)
            // 如果就绪队列太长，说明发送端跟不上，阻塞生产者
            while (push_queue.ready_queue.size() >= push_queue.max_queued_batches_) {
                bool ok = push_queue.cv_full.wait_for(ulock, kQueueWaitTimeout, [&] {
                    return push_queue.ready_queue.size() < push_queue.max_queued_batches_;
                });
                if (!ok) {
                    if (has_callback && ack_level_ != MYMQ::ACK_Level::ACK_NORESPONCE) {
                        cb(MYMQ_Public::PushResponce(tp.topic, tp.partition, MYMQ_Public::CommonErrorCode::QUEUE_FULL, 0));
                    }
                    return Err_Client::QUEUE_FULL;
                }
            }

            // 2. 轮转：将 Active 移动到 Ready Queue
            auto item = std::make_unique<MYMQ::Client::BatchItem>();
            item->buffer = std::move(push_queue.active_buf);
            item->callbacks = std::move(push_queue.active_cbs); // 移动回调列表
            item->batch_count = push_queue.current_batch_count;

            push_queue.ready_queue.push_back(std::move(item));

            // 3. 获取新的 Active Buffer (从对象池或新建)
            if (!push_queue.free_pool.empty()) {
                push_queue.active_buf = std::move(push_queue.free_pool.back());
                push_queue.free_pool.pop_back();
            } else {
                push_queue.active_buf = std::make_unique<MYMQ::MSG_serial::BatchBuffer>(push_queue.buffer_size_);
            }
            
            // 重置计数器 (active_cbs 已经被 move 空了，不需要 clear)
            push_queue.current_batch_count = 0;

            // 4. 触发 Flush 任务 (如果还没运行)
            if (!push_queue.is_flushing) {
                push_queue.is_flushing = true;

                auto push_queue_key = tp.topic + "_" + std::to_string(tp.partition);
                uint32_t shard_id = MurmurHash2::hash(push_queue_key);

                pool_.submit(shard_id, [this, pq_ptr]() {
                    this->flush_batch_task(*pq_ptr);
                });
            }

            // 5. 循环继续，重试 append_record (现在 active_buf 是新的空 buffer，肯定能成功)
        }

        return Err_Client::Success;
    }


    void MYMQ_Produceruse::create_topic(const std::string& topicname,size_t parti_num){
        auto req = ClientProtocol::build_create_topic_packet(topicname, parti_num);
        send(Eve::CLIENT_REQUEST_CREATE_TOPIC, req);
    }

    void MYMQ_Produceruse::init(const std::string& clientid,uint8_t ack_level){

        {

            Config_manager cm_sys(path_+"\\config\\sys.ini");

            auto thread_corenum= cm_sys.getint("max_threadnum_client");

            ThreadPool::instance(thread_corenum).start();


            auto zstd_level_tmp= cm_sys.getint("zstd_level");
            if(!inrange(zstd_level_tmp,0,22)){
                zstd_level=MYMQ::zstd_level_DEFAULT;
            } else {
                zstd_level=zstd_level_tmp;
            }
            dctx=ZSTD_createDCtx();
            auto tmp_clientid=clientid;
            if(!inrange(tmp_clientid.size(),1,30)){
                tmp_clientid=MYMQ::CLIENTID_DEFAULT;
                cerr("Initialization : Invaild 'clientid' in config . Use default 'clientid' : "+MYMQ::CLIENTID_DEFAULT);
            }
            auto make_role_clientid = [](const std::string& base, const std::string& suffix) {
                constexpr size_t kMaxClientIdLen = 30;
                if (suffix.size() >= kMaxClientIdLen) {
                    return suffix.substr(0, kMaxClientIdLen);
                }
                if (base.size() + suffix.size() <= kMaxClientIdLen) {
                    return base + suffix;
                }
                return base.substr(0, kMaxClientIdLen - suffix.size()) + suffix;
            };
            cmc_.set_clientid(tmp_clientid);
            cmc_.set_channel_role(MYMQ_Public::ChannelRole::CONTROL);
            cmc_produce_.set_clientid(make_role_clientid(tmp_clientid, "#produce"));
            cmc_produce_.set_channel_role(MYMQ_Public::ChannelRole::PRODUCE);
            cmc_produce_.set_request_timeout_ms(60000);
            {
                std::unique_lock<std::shared_mutex> ulock(info_basic.mtx);
                info_basic.clientid=tmp_clientid;
            }

            auto tmp_ack_level=ack_level;
            if(!inrange(tmp_ack_level,0,1)){
                tmp_ack_level=MYMQ::ack_level_DEFAULT;
                cerr("Initialization : Invaild 'ack_level' in config . Use default 'ack_level' : "+std::to_string(MYMQ::ack_level_DEFAULT));
            }
            cmc_.set_ACK_level(static_cast<MYMQ::ACK_Level>(tmp_ack_level));
            cmc_produce_.set_ACK_level(static_cast<MYMQ::ACK_Level>(tmp_ack_level));
            ack_level_=static_cast<MYMQ::ACK_Level>(tmp_ack_level);


            size_t max_in_flight_requests_num_tmp= cm_sys.get_size_t("max_in_flight_requests_num");
            if(!inrange(max_in_flight_requests_num_tmp,1,5000000)){
                max_in_flight_requests_num_tmp=MYMQ:: MAX_IN_FLIGHT_REQUEST_NUM_DEFAULT;
                cerr("Initialization : Invaild 'max_in_flight_requests_num' in config . Use default 'max_in_flight_requests_num' : "+std::to_string(MYMQ:: MAX_IN_FLIGHT_REQUEST_NUM_DEFAULT));
            }
            max_in_flight_requests_num=max_in_flight_requests_num_tmp;
        }


        {

            Config_manager cm_business(path_+"\\config\\business.ini");
            batch_size=cm_business.get_size_t("batch_size");
            autopush_perior_ms=cm_business.get_size_t ("autopush_perior_ms");
            local_push_buffer_size=cm_business.get_size_t("local_push_buffer_size");
            size_t max_queued_batches_tmp = 5;
            try {
                max_queued_batches_tmp = cm_business.get_size_t("push_max_queued_batches");
            } catch (...) {
                max_queued_batches_tmp = 5;
            }
            if(!inrange(max_queued_batches_tmp,1,1024)){
                max_queued_batches_tmp = 5;
            }
            push_max_queued_batches = max_queued_batches_tmp;
        }

        push_perioric_start();
    }



    void MYMQ_Produceruse::flush_batch_task(MYMQ::Client::Push_queue& pq) {
        
        auto invoke_callbacks_with_error = [&](const TopicPartition& tp, std::vector<MYMQ::Client::SparseCallback>& callbacks, MYMQ_Public::CommonErrorCode err) {
            for (auto& sparse_item : callbacks) {
                auto& current_cb = sparse_item.cb;
                std::visit([&](auto&& specific_cb) {
                    using CBType = std::decay_t<decltype(specific_cb)>;
                    if constexpr (std::is_same_v<CBType, MYMQ_Public::PushResponceCallback>) {
                        specific_cb(MYMQ_Public::PushResponce(tp.topic, tp.partition, err, 0));
                    } else if constexpr (std::is_same_v<CBType, MYMQ_Public::CommitAsyncResponceCallback>) {
                        specific_cb(MYMQ_Public::CommitAsyncResponce(std::string{}, tp, 0, err));
                    } else if constexpr (std::is_same_v<CBType, MYMQ_Public::CallbackNoop>) {
                        specific_cb(err);
                    } else {
                        static_assert(MYMQ_Public::always_false_v<CBType>, "Unknown callback type");
                    }
                }, current_cb);
            }
        };

        while (true) {
            std::unique_ptr<MYMQ::Client::BatchItem> item;

            // 1. 获取任务
            {
                std::unique_lock<std::mutex> ulock(pq.mtx);
                
                if (pq.ready_queue.empty()) {
                    pq.is_flushing = false;
                    pq.cv_full.notify_all(); // 唤醒可能的等待者
                    return;
                }

                item = std::move(pq.ready_queue.front());
                pq.ready_queue.pop_front();

                // 如果队列稍微腾出空间了，可以唤醒生产者
                if (pq.ready_queue.size() < pq.max_queued_batches_) {
                    pq.cv_full.notify_all();
                }
            }

            if (stopped_.load(std::memory_order_acquire)) {
                if (ack_level_ != MYMQ::ACK_Level::ACK_NORESPONCE) {
                    auto callbacks = std::move(item->callbacks);
                    invoke_callbacks_with_error(pq.tp, callbacks, MYMQ_Public::CommonErrorCode::PRODUCER_STOPPED);
                }

                {
                    std::unique_lock<std::mutex> ulock(pq.mtx);
                    item->buffer->release();
                    pq.free_pool.push_back(std::move(item->buffer));
                }
                continue;
            }

            // 2. 处理任务 (无锁)
            if (item->buffer->size() > 0) {
                auto final_packet = ClientProtocol::build_push_packet(
                    pq.tp.topic,
                    pq.tp.partition,
                    item->buffer.get(),
                    pq.cctx,
                    zstd_level
                );

                if (!final_packet.empty()) {
                    if (ack_level_ != MYMQ::ACK_Level::ACK_NORESPONCE) {
                        TopicPartition tp_copy = pq.tp;
                        auto callbacks_ptr = std::make_shared<std::vector<MYMQ::Client::SparseCallback>>(std::move(item->callbacks));
                        MYMQ::Network::Communication_client::SendFailReason fail_reason = MYMQ::Network::Communication_client::SendFailReason::None;
                        auto succ = cmc_produce_.send_msg_blocking(
                            static_cast<uint16_t>(Eve::CLIENT_REQUEST_PUSH),
                            final_packet,
                            [this, callbacks_ptr, tp_copy](uint16_t event_type_responce, MYMQ::OwnedBytes msg_body_responce) mutable
                            {
                                auto resp = handle_response(static_cast<Eve>(event_type_responce), msg_body_responce);

                                for (auto& sparse_item : *callbacks_ptr) {
                                    uint32_t msg_idx = sparse_item.relative_index;
                                    auto& current_cb = sparse_item.cb;

                                    std::visit([&](auto&& specific_cb) {
                                        using CBType = std::decay_t<decltype(specific_cb)>;

                                        if constexpr (std::is_same_v<CBType, MYMQ_Public::PushResponceCallback>)
                                        {
                                            if (auto* data = std::get_if<MYMQ_Public::PushResponce>(&resp)) {
                                                MYMQ_Public::PushResponce individual_resp = *data;
                                                individual_resp.offset = data->offset + msg_idx;
                                                specific_cb(individual_resp);
                                            } else if (auto* err = std::get_if<MYMQ_Public::CommonErrorCode>(&resp)) {
                                                specific_cb(MYMQ_Public::PushResponce(tp_copy.topic, tp_copy.partition, *err, 0));
                                            }
                                        }
                                        else if constexpr (std::is_same_v<CBType, MYMQ_Public::CommitAsyncResponceCallback>)
                                        {
                                            if (auto* data = std::get_if<MYMQ_Public::CommitAsyncResponce>(&resp)) {
                                                specific_cb(*data);
                                            } else if (auto* err = std::get_if<MYMQ_Public::CommonErrorCode>(&resp)) {
                                                specific_cb(MYMQ_Public::CommitAsyncResponce(std::string{}, tp_copy, 0, *err));
                                            }
                                        }
                                        else if constexpr (std::is_same_v<CBType, MYMQ_Public::CallbackNoop>)
                                        {
                                            if (auto* err = std::get_if<MYMQ_Public::CommonErrorCode>(&resp)) {
                                                specific_cb(*err);
                                            } else {
                                                specific_cb(MYMQ_Public::CommonErrorCode::Success);
                                            }
                                        }
                                        else
                                        {
                                            static_assert(MYMQ_Public::always_false_v<CBType>, "Unknown callback type");
                                        }

                                    }, current_cb);
                                }
                            },
                            max_in_flight_requests_num,
                            std::chrono::milliseconds(60000),
                            &fail_reason
                        );

                        if (!succ) {
                            if (stopped_.load(std::memory_order_acquire) ||
                                fail_reason == MYMQ::Network::Communication_client::SendFailReason::Stopped) {
                                invoke_callbacks_with_error(tp_copy, *callbacks_ptr, MYMQ_Public::CommonErrorCode::PRODUCER_STOPPED);
                                {
                                    std::unique_lock<std::mutex> ulock(pq.mtx);
                                    item->buffer->release();
                                    pq.free_pool.push_back(std::move(item->buffer));
                                }
                                continue;
                            }
                            std::unique_lock<std::mutex> ulock(pq.mtx);
                            auto requeue_item = std::make_unique<MYMQ::Client::BatchItem>();
                            requeue_item->buffer = std::move(item->buffer);
                            requeue_item->callbacks = std::move(*callbacks_ptr);
                            requeue_item->batch_count = item->batch_count;
                            pq.ready_queue.push_front(std::move(requeue_item));
                            pq.is_flushing = false;
                            pq.cv_full.notify_all();
                            return;
                        }
                    } else {
                        MYMQ::Network::Communication_client::SendFailReason fail_reason = MYMQ::Network::Communication_client::SendFailReason::None;
                        auto succ = cmc_produce_.send_msg_blocking(
                            static_cast<uint16_t>(Eve::CLIENT_REQUEST_PUSH),
                            final_packet,
                            ResponseCallback{},
                            max_in_flight_requests_num,
                            std::chrono::milliseconds(60000),
                            &fail_reason
                        );

                        if (!succ) {
                            if (stopped_.load(std::memory_order_acquire) ||
                                fail_reason == MYMQ::Network::Communication_client::SendFailReason::Stopped) {
                                std::unique_lock<std::mutex> ulock(pq.mtx);
                                item->buffer->release();
                                pq.free_pool.push_back(std::move(item->buffer));
                                continue;
                            }
                            std::unique_lock<std::mutex> ulock(pq.mtx);
                            pq.ready_queue.push_front(std::move(item));
                            pq.is_flushing = false;
                            pq.cv_full.notify_all();
                            return;
                        }
                    }
                }
            }

            // 3. 归还 Buffer
            {
                std::unique_lock<std::mutex> ulock(pq.mtx);
                item->buffer->release();
                pq.free_pool.push_back(std::move(item->buffer));
            }
        }
    }


    // finish_flush is deprecated and removed


    void MYMQ_Produceruse::push_perioric_start(){
        push_perioric_taskid= timer.commit_ms([this]{
            push_timer_send();
        },autopush_perior_ms,autopush_perior_ms);
    }
    void MYMQ_Produceruse::push_perioric_stop(){
        timer.commit_ms([this]{
            timer.cancel_task(push_perioric_taskid);
        },10,10,1);
    }




    MYMQ_Public::ResultVariant MYMQ_Produceruse::handle_response(Eve event_type,const MYMQ::OwnedBytes& msg_body){
        auto protocol_resp = ClientProtocol::parse_response(event_type, msg_body);

        return std::visit([this](auto&& arg) -> MYMQ_Public::ResultVariant {
             using T = std::decay_t<decltype(arg)>;
             if constexpr (std::is_same_v<T, MYMQ_Public::PushResponce>) {
                 return arg;
             }
             else if constexpr (std::is_same_v<T, CreateTopicResponse>) {
                 if (arg.success) {
                    cerr("CREATE TOPIC RESULT : Topic created successfully");
                } else {
                    cerr("CREATE TOPIC RESULT : Topic created failed");
                }
                return MYMQ_Public::CommonErrorCode::Success;
            }
            else if constexpr (std::is_same_v<T, MYMQ_Public::CommonErrorCode>) {
                return arg;
            }
            else {
                return MYMQ_Public::CommonErrorCode::Success;
            }
        }, protocol_resp);
    }


    bool MYMQ_Produceruse::is_register(){
        return cmc_.get_is_register();
    }
    void MYMQ_Produceruse::push_timer_send() {
        if (stopped_.load(std::memory_order_acquire)) {
            return;
        }
        if(!is_register()){
            return ;
        }

        // 2. 遍历所有分区队列
        for (auto it = recordaccumulator.begin(); it != recordaccumulator.end(); ++it) {
            auto pq_ptr = (*it).second;
            auto& pq = *pq_ptr;

            // 【优化】无锁预检查 (Dirty Check)
            if (pq.active_buf->size() == 0 && pq.ready_queue.empty()) {
                continue;
            }

            // ==========================================
            // 进入临界区
            // ==========================================
            std::unique_lock<std::mutex> ulock(pq.mtx);

            // A. Double Check
            if (pq.active_buf->size() == 0) {
                // 如果 Active 为空，但 Ready 有东西且未 Flushing，则触发 Flush
                if (!pq.ready_queue.empty() && !pq.is_flushing) {
                    pq.is_flushing = true;
                    auto push_queue_key = pq.tp.topic + "_" + std::to_string(pq.tp.partition);
                    uint32_t shard_id = MurmurHash2::hash(push_queue_key);
                    pool_.submit(shard_id, [this, pq_ptr]() {
                        this->flush_batch_task(*pq_ptr);
                    });
                }
                continue;
            }

            // B. 强制轮转 Active Buffer (Force Rotate)
            // 只要 Active 有数据，就将其转入 Ready Queue
            auto item = std::make_unique<MYMQ::Client::BatchItem>();
            item->buffer = std::move(pq.active_buf);
            item->callbacks = std::move(pq.active_cbs);
            item->batch_count = pq.current_batch_count;

            pq.ready_queue.push_back(std::move(item));

            // C. 补充新的 Active Buffer
            if (!pq.free_pool.empty()) {
                pq.active_buf = std::move(pq.free_pool.back());
                pq.free_pool.pop_back();
            } else {
                pq.active_buf = std::make_unique<MYMQ::MSG_serial::BatchBuffer>(pq.buffer_size_);
            }
            pq.active_buf->clear();
            pq.current_batch_count = 0;

            // D. 触发 Flush 任务
            if (!pq.is_flushing) {
                pq.is_flushing = true;
                auto push_queue_key = pq.tp.topic + "_" + std::to_string(pq.tp.partition);
                uint32_t shard_id = MurmurHash2::hash(push_queue_key);
                pool_.submit(shard_id, [this, pq_ptr]() {
                    this->flush_batch_task(*pq_ptr);
                });
            }
        }
    }

}
}
