#include"MYMQ_Client.h"
MYMQ_clientuse::MYMQ_clientuse(const std::string& clientid,uint8_t ack_level):path_(MYMQ::run_directory_DEFAULT),cmc_(MYMQ::run_directory_DEFAULT,MYMQ::REQUEST_TIMEOUT_MS_DEFAULT),tbb_dctx_pool([]() {
        // 初始化函数：当新线程第一次访问时调用
        return ZSTD_createDCtx();
    }){

    Config_manager::ensure_path_existed(MYMQ::run_directory_DEFAULT);
    init(clientid,ack_level);
     cmc_.init();
}
MYMQ_clientuse::~MYMQ_clientuse(){

     out_group_reset();
    cv_commit_ready.notify_all();
    cv_poll_ready.notify_all();

    for (ZSTD_DCtx* ctx : tbb_dctx_pool) {
        ZSTD_freeDCtx(ctx);
    }

}



    void MYMQ_clientuse::exit_rebalance(){

        is_leader=0;
        timer.commit_ms([this]{
            timer.cancel_task(join_collect_timeout_taskid);
            timer.cancel_task(rebalance_timeout_taskid);
        },10,10,1);

    }

    void MYMQ_clientuse::heartbeat_start(){
        heartbeat_taskid= timer.commit_ms([this]{
            heartbeat();
        },heartbeat_interval_ms,heartbeat_interval_ms);
    }

    void MYMQ_clientuse::heartbeat_stop(){
        timer.commit_ms([this]{
            timer.cancel_task(heartbeat_taskid);
        },10,10,1);
    }

    void MYMQ_clientuse::push_perioric_start(){
        push_perioric_taskid= timer.commit_ms([this]{
            push_timer_send();
        },autopush_perior_ms,autopush_perior_ms);
    }
    void MYMQ_clientuse::push_perioric_stop(){
        timer.commit_ms([this]{
            timer.cancel_task(push_perioric_taskid);
        },10,10,1);
    }



    void MYMQ_clientuse::autocommit_start(){
        autocommit_taskid= timer.commit_ms([this]{
            timer_commit_async();
        },autocommit_perior_ms,autocommit_perior_ms);
    }
    void MYMQ_clientuse::autocommit_stop(){
        timer.commit_ms([this]{
            timer.cancel_task(autocommit_taskid);
        },10,10,1);
    }

    Err_Client MYMQ_clientuse::seek(const MYMQ_Public::TopicPartition& tp,size_t offset_next_to_consume){


            Pollqueuemap::accessor ac;
            auto it=map_poll_queue.find(ac,tp);
            if(!it){
                cerr("ERROR :Invalid topic or partition in 'commit_inter' function");
                return MYMQ_Public::ClientErrorCode::INVALID_PARTITION_OR_TOPIC;
            }
            else{
                ac->second.clear_for_seek(offset_next_to_consume);
            }
            ac.release();
              out("[Seek offset] Current offset : "+std::to_string(offset_next_to_consume));
             return Err_Client::NULL_ERROR;


    }



    Err_Client MYMQ_clientuse::push(const MYMQ_Public::TopicPartition& tp, const std::string& key, const std::string& value, MYMQ_Public::PushResponceCallback cb) {

        // 1. 基础状态检查


        size_t curr_fly = SIZE_MAX;
        cmc_.get_curr_flying_request_num(curr_fly);
        if (curr_fly >= max_in_flight_requests_num) {
            cerr("[PUSH] FLYING REQUEST GOT TO LIMIT");
            return Err_Client::REACHED_MAX_FLYING_REQUEST;
        }


        auto it = map_push_queue.find(tp);
        if (it == map_push_queue.end()) {
            return Err_Client::INVALID_PARTITION_OR_TOPIC;
        }

        auto& push_queue = it->second;
        std::unique_lock<std::mutex> ulock(push_queue.mtx);

        // 3. 检查压缩上下文
        if (!push_queue.cctx) {
            cerr("ZSTD ERROR : CCTX Unavailable . Push Interrupt");
            return Err_Client::ZSTD_UNAVAILABLE;
        }

        bool has_callback = (bool)cb;

        // 检查：如果用户传了有效回调，但客户端配置为“不响应”，则报错
        if (has_callback && ack_level_ == MYMQ::ACK_Level::ACK_NORESPONCE) {
            cerr("WARNING: Callback provided but ignored due to ACK_NORESPONCE level.");
            return Err_Client::INVALID_OPRATION;
        }

        while (true) {
            // A. 尝试直接写入 Active Buffer
            bool success = push_queue.active_buf->append_record(key, value);

            if (success) {
                // [核心逻辑修正]
                // 只有当回调对象“非空” 且 配置允许响应时，才入队保存
                if (has_callback && ack_level_ != MYMQ::ACK_Level::ACK_NORESPONCE) {
                    push_queue.active_cbs->push_back({
                        static_cast<uint32_t>(push_queue.current_batch_count),
                        std::move(cb) // 移动语义，存入后外部cb失效
                    });
                }

                push_queue.current_batch_count++;

                return Err_Client::NULL_ERROR;
            }

            // 1. 背压检查
            while (push_queue.is_flushing) {
                push_queue.cv_full.wait(ulock);
            }

            // 2. 交换双缓冲
            std::swap(push_queue.active_buf, push_queue.flushing_buf);
            std::swap(push_queue.active_cbs, push_queue.flushing_cbs);

            // 3. 重置计数器
            push_queue.current_batch_count = 0;

            // 4. 标记状态
            push_queue.is_flushing = true;

            // 5. 提交 Flush 任务
            auto push_queue_key = tp.topic + "_" + std::to_string(tp.partition);
            uint32_t shard_id = MurmurHash2::hash(push_queue_key);
            pool_.submit(shard_id, [this, &push_queue]() {
                this->flush_batch_task(push_queue);
            });

            // 6. 循环继续，重试 append_record
        }

        return Err_Client::NULL_ERROR;
    }







    void MYMQ_clientuse:: set_local_pull_bytes_once(size_t bytes){

        local_pull_bytes_once.store(bytes);
    }


    void MYMQ_clientuse::flush_batch_task(MYMQ::MYMQ_Client::Push_queue& pq) {
        MYMQ::MSG_serial::BatchBuffer* src_buf = pq.flushing_buf;
        auto* src_cbs = pq.flushing_cbs;

        if (src_buf->size() == 0) {
            finish_flush(pq);
            return;
        }

        // --- 准备内存 ---
        size_t zstd_bound = ZSTD_compressBound(src_buf->size());
        std::vector<unsigned char> final_packet;
        // 预估大小：Header + Meta + ZstdBound
        final_packet.resize(1024 + zstd_bound);

        unsigned char* ptr = final_packet.data();
        unsigned char* packet_start = ptr;

        // =========================================================
        // 第一层：网络包头 (Topic, Partition, BatchCRC, GlobalBodyLength)
        // =========================================================

        // 1. Topic (Length + Data)
        uint32_t topic_len = static_cast<uint32_t>(pq.tp.topic.size());
        uint32_t n_topic_len = htonl(topic_len); // [Network Order]
        std::memcpy(ptr, &n_topic_len, sizeof(uint32_t)); ptr += sizeof(uint32_t);
        std::memcpy(ptr, pq.tp.topic.data(), topic_len);  ptr += topic_len;

        // 2. Partition (8 bytes)
        uint64_t part_val = static_cast<uint64_t>(pq.tp.partition);
        uint64_t n_part = htonll(part_val);      // [Network Order]
        std::memcpy(ptr, &n_part, sizeof(uint64_t)); ptr += sizeof(uint64_t);

        // 3. Batch CRC 占位 (4 bytes)
        unsigned char* crc_ptr = ptr;
        ptr += sizeof(uint32_t);

        // 4. Global Body Length 占位 (4 bytes)
        unsigned char* global_len_ptr = ptr;
        ptr += sizeof(uint32_t);

        // =========================================================
        // 第二层：Batch Body (CRC 计算范围)
        // =========================================================
        unsigned char* body_start = ptr;

        // 5. Batch Base Offset (8 bytes) - Outer
        uint64_t base_offset = 0; // 实际逻辑中应填入真实 Offset
        uint64_t n_base_outer = htonll(base_offset); // [Network Order]
        std::memcpy(ptr, &n_base_outer, sizeof(uint64_t)); ptr += sizeof(uint64_t);

        // 6. Batch Internal Length 占位 (4 bytes)
        unsigned char* batch_internal_len_ptr = ptr;
        ptr += sizeof(uint32_t);

        // --- 开始 Payload (Consumer 内层解压范围) ---
        unsigned char* payload_start = ptr;

        // 7. Inner Base Offset (8 bytes)
        // 为了方便 Consumer 内部逻辑，重复写入 BaseOffset
        uint64_t n_base_inner = htonll(base_offset); // [Network Order]
        std::memcpy(ptr, &n_base_inner, sizeof(uint64_t)); ptr += sizeof(uint64_t);

        // 8. Record Count (8 bytes)
        uint64_t record_count = static_cast<uint64_t>(src_buf->record_count_);
        uint64_t n_record_count = htonll(record_count); // [Network Order]
        std::memcpy(ptr, &n_record_count, sizeof(uint64_t)); ptr += sizeof(uint64_t);

        // 9. ZSTD Length 占位 (4 bytes)
        unsigned char* zstd_len_ptr = ptr;
        ptr += sizeof(uint32_t);

        // 10. ZSTD Data (压缩写入)
        size_t capacity_left = final_packet.size() - (ptr - packet_start);
        size_t compressed_size = ZSTD_compressCCtx(
            pq.cctx, ptr, capacity_left,
            src_buf->data_ptr(), src_buf->size(),
            zstd_level
            );

        if (ZSTD_isError(compressed_size)) { finish_flush(pq); return; }
        ptr += compressed_size;

        // =========================================================
        // 回填阶段 (Backfill)
        // =========================================================

        // A. 回填 ZSTD Length
        uint32_t n_zstd_len = htonl(static_cast<uint32_t>(compressed_size)); // [Network Order]
        std::memcpy(zstd_len_ptr, &n_zstd_len, sizeof(uint32_t));

        // B. 回填 Batch Internal Length
        // Length = (InnerBase + InnerCount + ZstdLen + ZstdData)
        size_t internal_len = ptr - payload_start;
        uint32_t n_internal_len = htonl(static_cast<uint32_t>(internal_len)); // [Network Order]
        std::memcpy(batch_internal_len_ptr, &n_internal_len, sizeof(uint32_t));

        // C. 回填 Global Body Length
        size_t global_body_len = ptr - body_start;
        uint32_t n_global_len = htonl(static_cast<uint32_t>(global_body_len)); // [Network Order]
        std::memcpy(global_len_ptr, &n_global_len, sizeof(uint32_t));

        // D. 计算并回填 CRC
        uint32_t crc = MYMQ::Crc32::calculate_crc32(body_start, global_body_len);
        uint32_t n_crc = htonl(crc); // [Network Order]
        std::memcpy(crc_ptr, &n_crc, sizeof(uint32_t));

        // E. 发送
        final_packet.resize(ptr - packet_start);
        if (ack_level_ != MYMQ::ACK_Level::ACK_NORESPONCE) {
            send(Eve::CLIENT_REQUEST_PUSH, final_packet, std::move(*src_cbs));
        } else {
            send(Eve::CLIENT_REQUEST_PUSH, final_packet);
        }
        finish_flush(pq);
    }

    // ----------------------------------------------------------------------
    // 2. 完整的 Consumer (解析)
    // ----------------------------------------------------------------------
    void MYMQ_clientuse::call_parse_impl(
        const std::vector<unsigned char>& raw_big_chunk,
        std::vector<MYMQ_Public::ConsumerRecord>& out_records,
        const MYMQ::MYMQ_Client::TopicPartition& tp,
        Err_Client& out_error
        ) {
        out_error = Err_Client::NULL_ERROR;

        // 如果数据极小，连一个 Header 都凑不齐，直接返回
        if (raw_big_chunk.empty()) return;

        // Batch 头部由 BaseOffset(8) + BatchLength(4) 组成
        // Payload 最小长度为 InnerBase(8) + Count(8) + ZstdLen(4) = 20 bytes
        const size_t MIN_PAYLOAD_SIZE = 20;

        size_t total_records_to_reserve = 0;
        const size_t chunk_size = raw_big_chunk.size();
        const unsigned char* chunk_data = raw_big_chunk.data();

        // =========================================================
        // Phase 1: Pre-scan (为了能够一口气 reserve)
        // =========================================================
        try {
            MessageParser scan_mp(chunk_data, chunk_size);

            while (!scan_mp.eof()) {
                // 1. 读取 Batch Header
                // 如果剩余数据不足以读取 Header (12 bytes)，MP 会抛出异常，跳出循环
                if (scan_mp.remaining() < 12) break;

                scan_mp.skip(8); // 跳过 BaseOffset (8 bytes)
                uint32_t batch_len = scan_mp.read_uint32(); // BatchLength

                if (batch_len == 0) continue;

                // 2. 检查 Payload 完整性
                // 此时 scan_mp.offset 指向 Payload 起始位置
                // 我们只需要读取里面的 Count 字段，不需要解压
                if (scan_mp.remaining() < batch_len) {
                    // 数据被截断，停止预扫描
                    break;
                }

                // 如果 Payload 甚至不足以存放 Meta 信息，跳过该 Batch
                if (batch_len < MIN_PAYLOAD_SIZE) {
                    scan_mp.skip(batch_len);
                    continue;
                }

                // 3. 读取 Meta (InnerBase + Count)
                scan_mp.skip(8); // 跳过 InnerBase
                uint64_t count = scan_mp.read_uint64();
                total_records_to_reserve += count;

                // 4. 跳过剩余的 Payload (ZstdLen + ZstdData)
                // 也就是跳过 (batch_len - 16) 字节
                scan_mp.skip(batch_len - 16);
            }
        } catch (const std::exception& e) {
            // Pre-scan 阶段的异常可以忽略（或者是数据截断），
            // 我们只利用已经扫到的数量进行 reserve，真正的错误由 Phase 2 捕捉
        }


        if (total_records_to_reserve > 0) {
            out_records.reserve(out_records.size() + total_records_to_reserve);
        }

        // =========================================================
        // Phase 2: Actual Parsing
        // =========================================================
        ZSTD_DCtx* dctx = tbb_dctx_pool.local();

        try {
            MessageParser mp(chunk_data, chunk_size);

            while (!mp.eof()) {
                // --- A. 读取 Batch Header ---
                if (mp.remaining() < 12) break;

                // BaseOffset (虽然这里没用到，但协议里有)
                uint64_t batch_base_offset = mp.read_uint64();
                // BatchLength
                uint32_t batch_len = mp.read_uint32();

                if (batch_len == 0) continue;

                // --- B. 获取 Payload 视图 ---
                // 这里我们手动构造一个指向 Payload 的视图/子解析器，或者直接校验
                if (mp.remaining() < batch_len) {
                    out_error = Err_Client::UNKNOWN_ERROR; // Data Truncated
                    return;
                }

                // 如果 batch_len < 20，属于异常数据，直接跳过整个 Payload
                if (batch_len < MIN_PAYLOAD_SIZE) {
                    mp.skip(batch_len);
                    continue;
                }

                // 标记 Payload 起始点，方便后续计算跳过

                // 1. Inner Base
                uint64_t inner_base = mp.read_uint64();
                // 2. Count
                uint64_t record_num = mp.read_uint64();

                // 3. ZSTD Block (Length + Data)
                // read_bytes_view 读取一个 uint32_t 长度，然后返回后续数据的指针和长度
                // 这正好对应 Batch 结构里的 [ZstdLen][ZstdData...]
                auto [comp_ptr, comp_len] = mp.read_bytes_view();

                // 此时 mp 已经越过了 ZstdData，指向了下一个 Batch 的开头 (如果计算正确的话)
                // 校验一下：我们读了 8+8=16 字节，read_bytes_view 读了 4+comp_len 字节
                // 所以总共消耗了 20 + comp_len。应该等于 batch_len。
                // 如果协议允许 padding，这里可能需要 skip 剩余字节，但通常 read_bytes_view 就是结尾。
                // 为了严谨（防止 BatchLength 比实际内容大），计算实际消耗并修正：
                size_t consumed_payload = 16 + 4 + comp_len;
                if (batch_len > consumed_payload) {
                    mp.skip(batch_len - consumed_payload);
                }

                // --- C. 解压 ---

                auto records_nozstd_vec = MYMQ::ZSTD::zstd_decompress_using_view(dctx, comp_ptr, comp_len);

                if (records_nozstd_vec.empty() && record_num > 0) continue;

                auto shared_buffer = std::make_shared<std::vector<unsigned char>>(std::move(records_nozstd_vec));
                MessageParser inner_mp(shared_buffer->data(), shared_buffer->size());

                for (size_t i = 0; i < record_num; ++i) {
                    if (inner_mp.eof()) break;

                    // read_bytes_view 返回的是指向 shared_buffer 内部的指针
                    auto [rec_body_ptr, rec_body_len] = inner_mp.read_bytes_view();

                    MessageParser rec_parser(rec_body_ptr, rec_body_len);

                    try {
                        std::string_view key_view = rec_parser.read_string_view();
                        std::string_view val_view = rec_parser.read_string_view();
                        int64_t timestamp = rec_parser.read_int64();

                        out_records.emplace_back(
                            tp.topic,
                            tp.partition,
                            key_view,       // 传入 view
                            val_view,       // 传入 view
                            timestamp,
                            inner_base + i,
                            shared_buffer   // 传入 shared_ptr，引用计数 +1
                            );
                    } catch (const std::exception&) {
                        continue;
                    }
                }
            }

        } catch (const std::exception& e) {
            std::cerr << "Error parsing record batch: " << e.what() << std::endl;
            out_error = Err_Client::UNKNOWN_ERROR;
        }
    }
    // 辅助函数：清理现场
    void MYMQ_clientuse::finish_flush(MYMQ::MYMQ_Client::Push_queue& pq) {
        std::unique_lock<std::mutex> ulock(pq.mtx);

        // 1. 复用内存：只是重置指针，不释放 heap 内存
        pq.flushing_buf->clear();

        // 2. 清空回调 vector
        pq.flushing_cbs->clear();

        // 3. 归还控制权
        pq.is_flushing = false;

        // 4. 唤醒生产者
        pq.cv_full.notify_all();
    }




    size_t MYMQ_clientuse::get_position_consumed(const MYMQ_Public::TopicPartition& tp){
        Endoffsetmap::const_accessor cac;
        auto it=map_end_offset.find(cac,tp);
        if(!it){
            return SIZE_MAX;
        }
        return cac->second.off.load(std::memory_order_relaxed);
    }

    Err_Client MYMQ_clientuse::pull(std::vector<MYMQ_Public::ConsumerRecord>& record_batch,size_t poll_wait_timeout_ms) {
        if (!record_batch.empty()) return Err_Client::INVALID_OPRATION;

        // --- 1. 数据收集阶段 (Accumulate Phase) ---
        size_t total_payload_bytes = 0;

        // 计算绝对截止时间
        auto start_time = std::chrono::steady_clock::now();
        auto deadline = start_time + std::chrono::milliseconds(poll_wait_timeout_ms);

        size_t active_item_count = 0;
        while (true) {
            bool gained_new_data_this_round = false;

            for (auto& [key, val_queue] : map_poll_queue) {
                std::vector<unsigned char> raw_chunk;

                if (val_queue.try_pop(raw_chunk)) {
                    size_t chunk_size = raw_chunk.size();

                    if (active_item_count < m_todo_cache.size()) {
                        auto& item = m_todo_cache[active_item_count];

                        item.index = active_item_count;
                        item.tp = key;
                        item.raw_big_chunk = std::move(raw_chunk);
                        item.err = Err_Client::NULL_ERROR;

                        item.parsed_records.clear();
                    }
                    else {
                        m_todo_cache.emplace_back(active_item_count, key, std::move(raw_chunk));
                    }

                    // 指针后移，指向下一个可用槽位
                    active_item_count++;

                    // --- 【核心修改结束】 ---

                    total_payload_bytes += chunk_size;
                    gained_new_data_this_round = true;

                    if (total_payload_bytes >= local_pull_bytes_once) {
                        goto PROCESS_PHASE;
                    }
                }
            }
            // B. 检查超时
            if (std::chrono::steady_clock::now() >= deadline) {
                break;
            }

            // C. 等待逻辑
            // 只有当“这一整轮轮询”都没有拿到任何数据时，才进入休眠等待
            // 如果拿到了一些数据但不够 local_pull_bytes，会立即进入下一轮轮询，不休眠，加快收集速度
            if (!gained_new_data_this_round) {
                std::unique_lock<std::mutex> ulock(mtx_poll_ready);


                bool signaled = cv_poll_ready.wait_until(ulock, deadline, [this] {
                    return poll_ready.load();
                });

                if (signaled) {
                    poll_ready.store(false);

                } else {

                    break;
                }

            }
        }

    PROCESS_PHASE:

        if (m_todo_cache.empty()) {
            return Err_Client::PULL_TIMEOUT;
        }

        // --- 2. 并行解析 (Parallel Parse) ---
        tbb::parallel_for_each(m_todo_cache.begin(), m_todo_cache.begin() + active_item_count,
                               [this](Workitem& item) {
                                   // 这里的 parsed_records 已经有很大的 capacity 了，push_back 不会触发 malloc
                                   this->call_parse_impl(item.raw_big_chunk, item.parsed_records, item.tp, item.err);
                               });

        // 标记是否发生了部分错误（可选，用于返回警告而非错误）
        bool has_partial_error = false;

        for (size_t i = 0; i < active_item_count; ++i) {
            auto& item = m_todo_cache[i];
            // 策略：如果出错，仅记录日志并跳过，不影响其他分区的正常数据
            if (item.err != Err_Client::NULL_ERROR) {
                has_partial_error = true;

                continue;
            }

            // 合并有效数据
            if (!item.parsed_records.empty()) {
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


        if (record_batch.empty() && has_partial_error) {
            return Err_Client::PARTIAL_PARASE_FAILED;
        }

        return record_batch.empty() ? Err_Client::EMPTY_RECORD : Err_Client::NULL_ERROR;
    }

    // 建议单位使用微秒 (us) 以获得更高精度，如果需要毫秒改为 milliseconds 即可
    Err_Client MYMQ_clientuse::pull(std::vector<MYMQ_Public::ConsumerRecord>& record_batch,
                                    size_t poll_wait_timeout_ms,
                                    int64_t& out_latency_us) { // [修改1] 新增引用参数

        // [修改2] 函数入口立即开始计时
        auto start_time = std::chrono::steady_clock::now();

        // [修改3] 定义一个简单的 lambda 用于在返回前更新时间
        // 这样做的好处是不用在每个 return 前都写一遍 duration_cast
        auto update_latency = [&]() {
            auto now = std::chrono::steady_clock::now();
            out_latency_us = std::chrono::duration_cast<std::chrono::microseconds>(now - start_time).count();
        };

        if (!record_batch.empty()) {
            update_latency(); // 返回前更新
            return Err_Client::INVALID_OPRATION;
        }

        // --- 1. 数据收集阶段 (Accumulate Phase) ---
        size_t total_payload_bytes = 0;

        // 计算绝对截止时间
        // 注意：这里可以直接复用上面的 start_time，无需再次调用 now()，减少一次系统调用开销
        auto deadline = start_time + std::chrono::milliseconds(poll_wait_timeout_ms);

        size_t active_item_count = 0;
        while (true) {
            bool gained_new_data_this_round = false;

            for (auto& [key, val_queue] : map_poll_queue) {
                std::vector<unsigned char> raw_chunk;

                if (val_queue.try_pop(raw_chunk)) {
                    size_t chunk_size = raw_chunk.size();

                    if (active_item_count < m_todo_cache.size()) {
                        auto& item = m_todo_cache[active_item_count];

                        item.index = active_item_count;
                        item.tp = key;
                        item.raw_big_chunk = std::move(raw_chunk);
                        item.err = Err_Client::NULL_ERROR;

                        item.parsed_records.clear();
                    }
                    else {
                        m_todo_cache.emplace_back(active_item_count, key, std::move(raw_chunk));
                    }

                    // 指针后移，指向下一个可用槽位
                    active_item_count++;

                    total_payload_bytes += chunk_size;
                    gained_new_data_this_round = true;

                    if (total_payload_bytes >= local_pull_bytes_once) {
                        goto PROCESS_PHASE;
                    }
                }
            }
            // B. 检查超时
            if (std::chrono::steady_clock::now() >= deadline) {
                break;
            }

            // C. 等待逻辑
            if (!gained_new_data_this_round) {
                std::unique_lock<std::mutex> ulock(mtx_poll_ready);

                // 这里的 deadline 逻辑保持不变
                bool signaled = cv_poll_ready.wait_until(ulock, deadline, [this] {
                    return poll_ready.load();
                });

                if (signaled) {
                    poll_ready.store(false);
                } else {
                    break;
                }
            }
        }

    PROCESS_PHASE:

        if (m_todo_cache.empty()) {
            update_latency(); // [修改4] 返回前更新 (超时返回)
            return Err_Client::PULL_TIMEOUT;
        }

        // --- 2. 并行解析 (Parallel Parse) ---
        tbb::parallel_for_each(m_todo_cache.begin(), m_todo_cache.begin() + active_item_count,
                               [this](Workitem& item) {
                                   this->call_parse_impl(item.raw_big_chunk, item.parsed_records, item.tp, item.err);
                               });

        // 标记是否发生了部分错误
        bool has_partial_error = false;

        for (size_t i = 0; i < active_item_count; ++i) {
            auto& item = m_todo_cache[i];

            if (item.err != Err_Client::NULL_ERROR) {
                has_partial_error = true;
                continue;
            }

            if (!item.parsed_records.empty()) {
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

        if (record_batch.empty() && has_partial_error) {
            update_latency(); // [修改5] 返回前更新 (错误返回)
            return Err_Client::PARTIAL_PARASE_FAILED;
        }

        update_latency(); // [修改6] 返回前更新 (正常返回)
        return record_batch.empty() ? Err_Client::EMPTY_RECORD : Err_Client::NULL_ERROR;
    }




    void MYMQ_clientuse::create_topic(const std::string& topicname,size_t parti_num){
        MessageBuilder mb;
        mb.append_string(topicname);
        mb.append_size_t(parti_num);
        auto req=mb.data;

        send(Eve::CLIENT_REQUEST_CREATE_TOPIC,req);

    }
    void MYMQ_clientuse::set_pull_bytes(size_t bytes){
        if(bytes>1&&bytes<=MYMQ::pull_bytes_max){
            pull_bytes_once_of_request.store(bytes);
        }
        else{
            cerr("Set pull bytes : OUT OF LIMITATION");
        }

    }

    void MYMQ_clientuse::subscribe_topic(const std::string& topicname){
        std::unique_lock<std::shared_mutex> ulock(mtx_consumerinfo);
        info_consumer.subscribed_topics.insert(topicname);
    }

    void MYMQ_clientuse::unsubscribe_topic(const std::string& topicname){
        std::unique_lock<std::shared_mutex> ulock(mtx_consumerinfo);
        info_consumer.subscribed_topics.erase(topicname);
    }
    Err_Client MYMQ_clientuse::commit_sync(const MYMQ_Public::TopicPartition& tp,size_t next_offset_to_consume) {
        if(is_auto_commit){
            return Err_Client::AUTOCOMMIT_ENABLE;
        }
        if(!is_ingroup.load()){
            return Err_Client::NOT_IN_GROUP;
        }
        auto res= commit_inter(tp,next_offset_to_consume,MYMQ_Public::CommitAsyncResponceCallback());
        if(res!=Err_Client::NULL_ERROR){
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

        return Err_Client::NULL_ERROR;

    }



    Err_Client MYMQ_clientuse::commit_async(const MYMQ_Public::TopicPartition& tp,size_t next_offset_to_consume,MYMQ_Public::CommitAsyncResponceCallback cb) {
        if(is_auto_commit){
            return Err_Client::AUTOCOMMIT_ENABLE;
        }
        if(!is_ingroup.load()){
            return Err_Client::NOT_IN_GROUP;
        }
        return commit_inter(tp,next_offset_to_consume,cb);

    }




    Err_Client MYMQ_clientuse::join_group(const std::string& groupid){

        if(groupid.length()==0&&groupid.length()>30){
           return Err_Client::INVALID_GROUPID;
        }



        bool new_to_this_group=0;
        std::string clientid;
        {
            std::shared_lock<std::shared_mutex> basicinf_lock(info_basic.mtx);

            new_to_this_group=(groupid!= info_basic.groupid) ;
            clientid= info_basic.clientid;
        }


        std::string memberid{""};
        if(!new_to_this_group){

            {
                std::shared_lock<std::shared_mutex> slock(mtx_consumerinfo);
                memberid=info_consumer.memberid;
            }//旧成员因其他组员的重平衡而重入组
        }
        else{
            {
                std::unique_lock<std::shared_mutex> slock(mtx_consumerinfo);
                info_consumer.memberid=std::move(std::string{});
            }//新成员入组
        }



        ConsumerInfo consumerinf;
        {
            std::shared_lock<std::shared_mutex> slock(mtx_consumerinfo);
            if(new_to_this_group){
                info_consumer.memberid=std::move(std::string{});
                consumerinf=info_consumer;
            }

        }

         MessageBuilder mb;
        mb.append(groupid,memberid,consumerinf.generation_id,clientid);


        MessageBuilder mb2;
        size_t topicnum= consumerinf.subscribed_topics.size();
        mb2.append_size_t(topicnum);
        for(const auto& topic:consumerinf.subscribed_topics){
            mb2.append_string(topic);
        }
        mb.append_uchar_vector(mb2.data);

        send(Eve::CLIENT_REQUEST_JOIN_GROUP,mb.data);

        join_collect_timeout_taskid= timer.commit_ms([this]{
            if(is_ingroup.load()){
                return ;
            }
            out_group_reset();
            cerr("JoinCollect time out. Rebalance failed ");
        },join_collect_timeout_ms,join_collect_timeout_ms,1);

        rebalance_timeout_taskid= timer.commit_ms([this]{
            if(is_ingroup.load()){
                return ;
            }
            out_group_reset();
             cerr("Rebalance time out. Rebalance failed ");
        },rebalance_timeout_ms,rebalance_timeout_ms,1);


        return Err_Client::NULL_ERROR;

    }

    Err_Client MYMQ_clientuse::leave_group(const std::string& groupid){
        if(!is_ingroup.load()){
            return Err_Client::NOT_IN_GROUP;
        }
        bool valid=0;
        {
            std::shared_lock<std::shared_mutex>  slock(info_basic.mtx);
            valid=(groupid== info_basic.groupid) ;
        }
        if(!valid){
            return Err_Client::NOT_IN_GROUP;
        }
        std::string memberid;
        {
            std::shared_lock<std::shared_mutex>  slock1(mtx_consumerinfo);
            memberid= info_consumer.memberid;
        }
        MB mb;
        mb.append(groupid,memberid);
        send(Eve::CLIENT_REQUEST_LEAVE_GROUP,mb.data);
        return Err_Client::NULL_ERROR;

    }

    void MYMQ_clientuse::sync_group() {
        std::string group_id;
        std::string member_id;
        int generation_id;
        {
            std::shared_lock<std::shared_mutex>  slock(info_basic.mtx);
            group_id= info_basic.groupid;
        }
        {
            std::shared_lock<std::shared_mutex>  slock1(mtx_consumerinfo);
            member_id= info_consumer.memberid;
            generation_id=info_consumer.generation_id;
        }



        MB mb;
        mb.append(group_id,member_id,generation_id);
        if(is_leader){
            mb.append_string(group_assign_str_retry);
        }
        send(Eve::CLIENT_REQUEST_SYNC_GROUP,mb.data);
        cerr("Sync retry");

    }



    void MYMQ_clientuse::heartbeat() {

        if(!is_ingroup.load()){
            return ;
        }

        std::string groupid;
        std::string memberid;
        {
            std::shared_lock<std::shared_mutex>  slock(info_basic.mtx);
            groupid=info_basic.groupid;
        }
        {
            std::shared_lock<std::shared_mutex> slock(mtx_consumerinfo);
            memberid=info_consumer.memberid;
        }
        std::shared_lock<std::shared_mutex>  slock(info_basic.mtx);
        if(groupid.empty()||memberid.empty()){
            throw std::runtime_error("[Heartbeat] Empty groupID or memberid");

        }

        MessageBuilder mb;
        mb.append(groupid,memberid);
        send(MYMQ::EventType::CLIENT_REQUEST_HEARTBEAT,mb.data);
    }



    std::unordered_set<MYMQ_Public::TopicPartition> MYMQ_clientuse::get_assigned_partition(){
        std::unordered_set<MYMQ_Public::TopicPartition> res{};
        if(!is_ingroup.load()){
            return res;
        }
        for(const auto& [tp_str,tp_set]:map_final_assign){
            res.insert(tp_set.begin(),tp_set.end());
        }
        return res;
    }


    void MYMQ_clientuse::init(const std::string& clientid,uint8_t ack_level){

        {

            Config_manager cm_sys(path_+"\\config\\sys.ini");

            auto thread_corenum= cm_sys.getint("max_threadnum_client");

            ThreadPool::instance(thread_corenum).start();


            auto zstd_level_tmp= cm_sys.getint("zstd_level");
            if(!inrange(zstd_level_tmp,1,22)){
                zstd_level=MYMQ::zstd_level_DEFAULT;
            }


            zstd_level=zstd_level_tmp;
            dctx=ZSTD_createDCtx();
            heartbeat_interval_ms=MYMQ::HEARTBEAT_MS_CLIENT;
            auto tmp_clientid=clientid;
            if(!inrange(tmp_clientid.size(),1,30)){
                tmp_clientid=MYMQ::CLIENTID_DEFAULT;
                cerr("Initialization : Invaild 'clientid' in config . Use default 'clientid' : "+MYMQ::CLIENTID_DEFAULT);
            }
            cmc_.set_clientid(tmp_clientid);
            {
                std::unique_lock<std::shared_mutex> ulock(mtx_consumerinfo);
                info_consumer.userinfo.clientid=tmp_clientid;
            }
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
            ack_level_=static_cast<MYMQ::ACK_Level>(tmp_ack_level);


            size_t max_in_flight_requests_num_tmp= cm_sys.get_size_t("max_in_flight_requests_num");
            if(!inrange(zstd_level_tmp,1,5000)){
                max_in_flight_requests_num_tmp=MYMQ:: MAX_IN_FLIGHT_REQUEST_NUM_DEFAULT;
                cerr("Initialization : Invaild 'max_in_flight_requests_num' in config . Use default 'max_in_flight_requests_num' : "+std::to_string(MYMQ:: MAX_IN_FLIGHT_REQUEST_NUM_DEFAULT));
            }
            max_in_flight_requests_num=max_in_flight_requests_num_tmp;
        }


        {

            Config_manager cm_business(path_+"\\config\\business.ini");
            is_auto_commit=cm_business.getbool("autocommit");
            pull_start_location=static_cast<MYMQ::PullSet>( cm_business.get_uint16("pull_start_loc_option"));

            rebalance_timeout_ms=MYMQ::rebalance_timeout_ms;
            join_collect_timeout_ms=MYMQ::join_collect_timeout_ms;
            memberid_wait_timeout_s=MYMQ::memberid_ready_timeout_s;
            commit_wait_timeout_s=MYMQ::commit_ready_timeout_s;
            pull_bytes_once_of_request=cm_business.get_size_t("pull_bytes_once_of_request");
            batch_size=cm_business.get_size_t("batch_size");
            autopush_perior_ms=cm_business.get_size_t ("autopush_perior_ms");
            autocommit_perior_ms=cm_business.get_size_t ("autocommit_perior_ms");
            local_pull_bytes_once=cm_business.get_size_t("local_pull_bytes_once");



        }

    }



    void MYMQ_clientuse::timer_commit_async(){
        if(!is_ingroup.load()){
            return ;
        }

        std::vector<TopicPartition> all_keys;
        for(auto& [key,val]:map_end_offset){
            all_keys.emplace_back(key);

        }

        for(const auto& key:all_keys){
            Endoffsetmap::const_accessor cac;
            auto it=map_end_offset.find(cac,key);
            if(!it){
                cerr("[Auto commit] : Perioric commit error :"+MYMQ_Public::to_string(MYMQ_Public::CommonErrorCode::INTERNAL_ERROR));
                    continue;
            }
            const auto& val=cac->second;
             commit_async(val.tp,val.off.load(std::memory_order_relaxed));
        }
    }


    // 将第三个参数的类型从 std::deque 改为 std::vector
    bool MYMQ_clientuse::send(MYMQ::EventType event_type, const Mybyte& msg_body, std::vector<MYMQ::MYMQ_Client::SparseCallback> cbs_)
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
                                  (uint16_t event_type_responce, const Mybyte& msg_body_responce) mutable
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
                                                      specific_cb(MYMQ_Public::CommonErrorCode::NULL_ERROR);
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
    std::map<std::string, std::map<std::string, std::set<size_t>>> MYMQ_clientuse::assign_leaderdo(
        const std::unordered_map<std::string, std::set<std::string>>& member_to_topics,
        const std::unordered_map<std::string, size_t>& topic_num_map) {

        std::map<std::string, std::map<std::string, std::set<size_t>>> assignresult;

        std::map<std::string, std::vector<std::string>> topic_to_members;

        for (const auto& [member_id, topics] : member_to_topics) {
            for (const auto& topic_id : topics) {
                topic_to_members[topic_id].emplace_back(member_id);
            }
        }

        for (const auto& [topic_id, interested_members_raw] : topic_to_members) {

            auto it_parti_num = topic_num_map.find(topic_id);
            if (it_parti_num == topic_num_map.end()) {

                cerr("Warning: Topic '" + topic_id + "' not found in topic_num_map. Skipping assignment." ) ;
                continue;
            }
            size_t total_partitions = it_parti_num->second;


            if (total_partitions <= 0) {
                continue;
            }
            if (interested_members_raw.empty()) {
                continue;
            }

            std::vector<std::string> sorted_members = interested_members_raw;
            std::sort(sorted_members.begin(), sorted_members.end());

            size_t current_member_index = 0;


            for (size_t partition_id = 0; partition_id < total_partitions; ++partition_id) {

                const std::string& assigned_member_id = sorted_members[current_member_index];

                assignresult[assigned_member_id][topic_id].insert(partition_id);

                current_member_index = (current_member_index + 1) % sorted_members.size();
            }
        }

        return assignresult;
    }

    std::string MYMQ_clientuse::weave_assignments_message(const std::map<std::string, std::map<std::string, std::set<size_t>>>& assignments) {
        MB mb;
        mb.append_size_t(assignments.size());

        for (const auto& member_entry : assignments) {
            const std::string& member_id = member_entry.first;
            const std::map<std::string, std::set<size_t>>& topic_assignments = member_entry.second;


            mb.append(member_id,topic_assignments.size());

            for (const auto& topic_entry : topic_assignments) {
                const std::string& topic_name = topic_entry.first;
                const std::set<size_t>& partitions = topic_entry.second;


                mb.append(topic_name,static_cast<size_t>(partitions.size()));
                for (size_t partition_id : partitions) {

                    mb.append_size_t(partition_id);
                }
            }
        }

        return mb.dump();
    }

    Err_Client MYMQ_clientuse::commit_inter(const MYMQ_Public::TopicPartition& tp,size_t next_offset_to_consume,MYMQ_Public::CommitAsyncResponceCallback cb){

        Endoffsetmap::const_accessor cac;
        auto it=map_end_offset.find(cac,tp);
        if(!it){
            cerr("ERROR :Invalid topic or partition in 'commit_inter' function");
            return MYMQ_Public::ClientErrorCode::INVALID_PARTITION_OR_TOPIC;
        }

        size_t now_off =cac->second.off.load(std::memory_order_relaxed);
        cac.release();
          out("[Commit offset] Current offset : "+std::to_string(now_off));
        if(now_off>next_offset_to_consume){
            cerr("Warning : Attempt to commit a older offset : "+std::to_string(next_offset_to_consume));
        }



        std::string groupid;
        {
            std::shared_lock<std::shared_mutex> slock(info_basic.mtx);
            groupid =info_basic.groupid  ;
        }

        std::string key_off;
        {
            MB mb;
            mb.append(groupid,tp.topic,tp.partition);
            key_off=mb.dump();
        }
        auto parid_hash=MurmurHash2::hash(key_off);

        MB mb;
        mb.append(groupid,tp.topic,tp.partition,parid_hash,key_off,next_offset_to_consume);

        bool has_callback = (bool)cb;
        if(has_callback){
            std::vector<MYMQ::MYMQ_Client::SparseCallback> cbs_;
            cbs_.reserve(1);
            cbs_.push_back({static_cast<uint32_t>(0), std::move(cb)});
            send(Eve::CLIENT_REQUEST_COMMIT_OFFSET,mb.data,std::move(cbs_));

        }
        else{
            send(Eve::CLIENT_REQUEST_COMMIT_OFFSET,mb.data);
        }


        return MYMQ_Public::ClientErrorCode::NULL_ERROR;
    }

    MYMQ_Public::ResultVariant MYMQ_clientuse::handle_response(Eve event_type,const Mybyte& msg_body){

        MessageParser mp(msg_body.data(),msg_body.size());
        if(event_type==MYMQ::EventType::SERVER_RESPONSE_PUSH_ACK){
            auto topicname=mp.read_string();
            auto partition=mp.read_size_t();
            auto error=static_cast<Err>(mp.read_uint16());
            size_t base=SIZE_MAX;
            if(error==Err::NULL_ERROR){
                base=mp.read_size_t();
            }

            return  MYMQ_Public::PushResponce(std::move(topicname),partition,error,base);
        }
        else if(event_type==Eve::SERVER_RESPONSE_JOIN_REQUEST_HANDLED){
            auto error=static_cast<Err>( mp.read_uint16());
            auto groupid=mp.read_string();
            auto generationid=mp.read_int();
            auto memberid=mp.read_string();

            {
                std::unique_lock<std::shared_mutex> ulock(mtx_consumerinfo);
                info_consumer.generation_id=generationid;
            }

            if(error!=Err::NULL_ERROR){
                exit_rebalance();
                cerr("[Join Group] : Join request to '"+groupid+"' be refused : "+MYMQ_Public::to_string(error));
            }
            else{
                cerr("[Join Group] : Join request to '"+groupid+"' be accepted .");
                cerr("Join collect over .");


                {
                    auto assignment_inf=mp.read_uchar_vector();
                    MP mp_assignment_inf(assignment_inf.data(),assignment_inf.size());
                    auto leaderid=mp_assignment_inf.read_string();


                    cerr("Checking Leader ...");
                    if(leaderid==memberid){

                        cerr("You are leader");
                        cerr("Processing assignment ...");
                        is_leader=1;

                        std::unordered_map<std::string,std::set<std::string>>  member_to_topics;

                        auto topicnum=mp_assignment_inf.read_size_t();

                        member_to_topics.reserve(topicnum);
                        for(int i=0;i<topicnum;i++){
                            auto memberid=mp_assignment_inf.read_string();
                            auto subscribed_topicnum=  mp_assignment_inf.read_size_t();
                            for(int i=0;i<subscribed_topicnum;i++){
                                member_to_topics[memberid].insert(mp_assignment_inf.read_string() ) ;
                            }
                        }


                        std::unordered_map<std::string,size_t> topic_num_map;
                        auto topiclist_size= mp_assignment_inf.read_size_t();
                        topic_num_map.reserve(topiclist_size);
                        for(int i=0;i<topiclist_size;i++){
                            auto topicname=mp_assignment_inf.read_string();
                            auto parti_num=mp_assignment_inf.read_size_t();
                            topic_num_map[topicname]=parti_num;
                        }

                        auto group_assign= assign_leaderdo(member_to_topics,topic_num_map);

                        auto group_assign_str =  weave_assignments_message(group_assign);

                        MB mb;
                        mb.append(groupid,leaderid,generationid,static_cast<uint16_t>(pull_start_location), group_assign_str);
                        group_assign_str_retry=std::move(group_assign_str);

                        send(Eve::CLIENT_REQUEST_SYNC_GROUP,mb.data);
                        cerr("Processing assignment over. Assignment submitted now");



                    }
                    else {
                        cerr("You are follower");
                        cerr("Leader is "+leaderid+" . Yours is "+memberid );
                        MB mb;
                        mb.append(groupid,memberid,generationid,static_cast<uint16_t>(pull_start_location));

                        cerr("Waiting for assignment ...");


                    }

                }
            }


        }
        else if(event_type==Eve::SERVER_RESPONSE_SYNC_GROUP_ACK){
            auto error=static_cast<Err>(mp.read_short()) ;
            auto groupid=mp.read_string();
            auto memberid=mp.read_string();



            if(error==Err::NULL_ERROR){

                {
                    std::unique_lock<std::shared_mutex> ulock(mtx_consumerinfo) ;
                    info_consumer.memberid=memberid;
                }

                std::queue<std::pair<std::pair<std::string,size_t>,size_t> > tmp_queue_endoffset;

                {
                    std::unordered_map<std::string,std::unordered_set<MYMQ_Public::TopicPartition> > map_final_assign_tmp;
                    auto topic_num=mp.read_size_t();
                    map_final_assign_tmp.reserve(topic_num);
                    for(int i=0;i<topic_num;i++){
                        auto topicname=mp.read_string();
                        auto parti_assigned_num=mp.read_size_t();
                        map_final_assign_tmp[topicname].reserve(parti_assigned_num);
                        for(int j=0;j<parti_assigned_num;j++){

                            auto parti_id= mp.read_size_t();
                            auto end_offset=mp.read_size_t();
                            map_final_assign_tmp[topicname].emplace(topicname,parti_id);
                            tmp_queue_endoffset.emplace(std::make_pair(topicname, parti_id),end_offset);

                        }
                    }


                    std::unique_lock<std::shared_mutex> ulock(mtx_map_final_assign);
                    map_final_assign=std::move(map_final_assign_tmp);

                }

                {
                    Endoffsetmap tmp_map_endoffset;
                    Pollqueuemap tmp_map_poll_queue;
                    std::unordered_map<TopicPartition,MYMQ::MYMQ_Client::Push_queue> tmp_map_push_queue;
                    auto size_tmp=tmp_queue_endoffset.size();
                    tmp_map_push_queue.reserve(size_tmp);


                    while(!tmp_queue_endoffset.empty()){
                        auto& [par,off] =tmp_queue_endoffset.front();
                        TopicPartition tp(par.first,par.second);
                        tmp_queue_endoffset.pop();
                        {
                            Endoffsetmap::accessor ac;
                            tmp_map_endoffset.emplace(
                                ac,
                                std::piecewise_construct,
                                std::make_tuple(tp),
                                std::make_tuple(off, par.first, par.second)
                                );
                        }
                        {
                            Pollqueuemap::accessor ac;
                            tmp_map_poll_queue.emplace(
                                ac,
                                std::piecewise_construct,
                                std::make_tuple(tp),
                                std::make_tuple(400000000,6000000000)
                                );
                            ac->second.local_consume_offset.store(off);
                        }


                        tmp_map_push_queue.emplace(std::piecewise_construct,
                                                   std::forward_as_tuple(tp),
                                                   std::forward_as_tuple(tp,batch_size));
                    }

                    map_end_offset=std::move(tmp_map_endoffset);
                    map_push_queue=std::move(tmp_map_push_queue);
                    map_poll_queue=std::move(tmp_map_poll_queue);

               }

                cerr("[Sync group] Sync success." );
                cerr("[Sync group] JoinGroup success");


                is_ingroup.store(1);
                trigger_poll_for_low_cap_pollbuffer();
                heartbeat_start();
                push_perioric_start();

                if(is_auto_commit){
                    autocommit_start();
                }
                {
                    std::unique_lock<std::shared_mutex>  slock(info_basic.mtx);
                    info_basic.groupid=groupid;
                }
            }
            else{
                cerr("[Sync group] Sync failed."+MYMQ_Public::to_string(error) );
            }

        }
        else if(event_type==Eve::SERVER_RESPONCE_LEAVE_GROUP){
            auto error=static_cast<Err>( mp.read_uint16());
            auto groupid=mp.read_string();
            if(error==Err::NULL_ERROR){
                out_group_reset();
                out("Leave Group : Leaved Group '"+groupid+"'");
            }
            else{
                cerr("Leave Group : Leaving Group '"+groupid+"' : "+MYMQ_Public::to_string(error)) ;
            }

        }
        else if(event_type == Eve::SERVER_RESPONSE_PULL_DATA) {

            // 1. 读取响应头元数据 (这部分数据很小，保持原有逻辑)
            auto pull_inf_additional_view = mp.read_bytes_view();
            MessageParser mp_pull_inf_additional(pull_inf_additional_view.first,pull_inf_additional_view.second);
            auto topicname_ = mp_pull_inf_additional.read_string();
            auto partition_ = mp_pull_inf_additional.read_size_t();
            auto error = static_cast<Err>(mp_pull_inf_additional.read_uint16());
             auto offset_next_to_consume = mp_pull_inf_additional.read_size_t();
            out("[PULL] Messages batch from (TOPIC '" + topicname_ + "' PARTITION '" + std::to_string(partition_) + ") responce reached. ");
            out(std::string{} + "[PULL] Result : " + " State : " + MYMQ_Public::to_string(error));

            bool is_no_record=(error==Err::NO_RECORD);
            if(is_no_record){
            }
            if(error != Err::NULL_ERROR) {
                return error;
            }


             auto message_collection= mp.read_uchar_vector();
            bool need_poll=0;
            // 2. 找到对应的队列
            Pollqueuemap::accessor ac;
            if(map_poll_queue.find(ac, TopicPartition(topicname_, partition_))){
                auto& pollqueue = ac->second;

                pollqueue.local_consume_offset=offset_next_to_consume;
                // 3. 将整个大包直接推入队列
                pollqueue.push(message_collection);


                // 4. 【关键】Notify 的位置在这里！
                // 只要推入了数据，就设置标志位并唤醒等待的 pull 线程
                {
                    std::lock_guard<std::mutex> lock(mtx_poll_ready);
                    poll_ready.store(true);
                }
                cv_poll_ready.notify_one(); // 唤醒 pull 函数中的 wait
                need_poll =pollqueue.need_poll();
            }

            ac.release();

                if( need_poll&&!is_no_record){


                    std::string groupid;
                    {
                        std::shared_lock<std::shared_mutex> slock(mtx_consumerinfo);
                        groupid= info_basic.groupid;

                    }
                    if(!is_ingroup.load()){

                        return Err::CLIENT_NOT_IN_GROUP;
                    }


                    size_t bytes= pull_bytes_once_of_request.load();
                    MB mb;
                    mb.append(groupid,topicname_,partition_,offset_next_to_consume,bytes);
                    send(Eve:: CLIENT_REQUEST_PULL,mb.data);



                }





        }
        else if(event_type==Eve::SERVER_RESPONCE_HEARTBEAT){
            int generation_id=mp.read_int();
            bool need_to_join=0;
            auto groupstate_digit=mp.read_short();//mapto {0,1,2,3} enum GroupState { STABLE, JOIN_COLLECTING, AWAITING_SYNC,EMPTY};

            int generation_id_local=-1;
            {
                std::shared_lock<std::shared_mutex> slock(mtx_consumerinfo);
                generation_id_local=info_consumer.generation_id;
            }

            if(generation_id!=generation_id_local){
                need_to_join=1;
            }

            if(need_to_join){
                is_ingroup.store(0);
                std::string groupid;
                {
                std::shared_lock<std::shared_mutex> basicinf_lock(info_basic.mtx);
                groupid= info_basic.groupid ;
                }
                join_group(groupid);
            }

        }
        else if(event_type==Eve::SERVER_RESPONSE_CREATE_TOPIC){
            auto res= mp.read_bool();
            if(res){
                cerr("CREATE TOPIC RESULT : Topic created successfully");
            }
            else{
                cerr("CREATE TOPIC RESULT : Topic created failed");

            }
        }
        else if(event_type==Eve::SERVER_RESPONCE_COMMIT_OFFSET){


            auto groupid=mp.read_string();
            auto topicname=mp.read_string();
            auto partition=mp.read_size_t();
            auto errorcode=static_cast<Err>(mp.read_uint16()) ;
            auto offset=mp.read_size_t();

            if(!get_is_ingroup()){
                cerr("Get commit respose but now not in group");
                return MYMQ_Public::CommonErrorCode::COMMIT_OFFSET_TIMEOUT;
            }


            std::shared_lock<std::shared_mutex> slock(info_basic.mtx);
            auto groupid_local=info_basic.groupid;
            slock.unlock();
            if(groupid_local!=groupid){
                cerr("[Commit offset] Warning : Get commit respose but not in Group '"+groupid_local+"'");
                return MYMQ_Public::CommonErrorCode::COMMIT_OFFSET_TIMEOUT;
            }
            if(errorcode==Err::NULL_ERROR){
                {
                    auto key= topicname+"_"+std::to_string(partition);


                    Endoffsetmap::const_accessor cac;
                    auto it=map_end_offset.find(cac,TopicPartition(key,partition));
                    if(!it){
                        cerr("Error : Failed to update committed offset : Invalid topic or partition .");
                        return MYMQ_Public::CommonErrorCode::INTERNAL_ERROR;
                    }
                    auto& point=cac->second;
                    point.off.store(offset,std::memory_order_release) ;


                }


                cerr("Commit SYNC result : Success to commit '"+std::to_string(offset) +"'");
            }
            else{
                cerr("Commit SYNC result : Failed to commit '"+std::to_string(offset) +"'");
            }

            commit_ready.store(1);
            cv_commit_ready.notify_all();
        }
        else if(event_type==Eve::EVENTTYPE_NULL){
            return Err::REQUEST_TIMEOUT;
        }


        return MYMQ_Public::CommonErrorCode::NULL_ERROR;
    }

    void MYMQ_clientuse::push_timer_send() {

        for (auto& it : map_push_queue) {
            // key 是 partition string, value 是 Push_queue
            const TopicPartition& pq_key = it.first;
            auto& pq = it.second;

            // 【优化】无锁预检查 (Dirty Check)
            // active_buf 是指针，读取指针指向的 size 是相对安全的（哪怕读到旧值也没事，下次再发）
            // 如果 size 为 0，直接跳过，绝不抢锁
            if (pq.active_buf->size() == 0) {
                continue;
            }

            // ==========================================
            // 进入临界区
            // ==========================================
            std::unique_lock<std::mutex> ulock(pq.mtx);

            // A. Double Check: 再次确认是否有数据
            if (pq.active_buf->size() == 0) {
                continue;
            }

            // B. 【关键】检查后台是否忙碌 (Backpressure)
            // 如果 is_flushing 为 true，说明上一次的 Batch 还没压完/发完。
            // 此时定时器不能强制 Swap，否则会覆盖 flushing_buf 里的数据！
            // 策略：跳过本次，让数据继续积攒，等后台空闲了再说。
            if (pq.is_flushing) {
                continue;
            }

            // ==========================================
            // C. 执行交换 (Swap) - 极速操作
            // ==========================================
            // 既然后台不忙，且 active 有数据，说明因为没填满所以没触发 push 里的 flush
            // 定时器强制触发它！
            std::swap(pq.active_buf, pq.flushing_buf);
            std::swap(pq.active_cbs, pq.flushing_cbs);

            // 标记后台忙
            pq.is_flushing = true;

            // ==========================================
            // D. 派发给线程池 (非阻塞)
            // ==========================================

            auto key_hash=pq_key.topic+"_"+std::to_string( pq_key.partition);
            uint32_t shard_id = MurmurHash2::hash(key_hash);

            pool_.submit(shard_id, [this, &pq]() {
                this->flush_batch_task(pq);
            });


        }
    }
    void MYMQ_clientuse::out_group_reset(){
        is_leader=0;
        is_ingroup.store(0);
        exit_rebalance();
        {
            std::string tmp{};
            std::unordered_map<std::string,std::unordered_set<MYMQ_Public::TopicPartition> > tmp1{};
            map_final_assign=std::move(tmp1);
            group_assign_str_retry=std::move(tmp);
        }
        heartbeat_stop();
        autocommit_stop();
        push_perioric_stop();
        // poll_perioric_stop();
    }




    void MYMQ_clientuse::trigger_poll_for_low_cap_pollbuffer(){
        if(!is_ingroup.load()){
            return ;
        }
        std::string groupid;
        {
            std::shared_lock<std::shared_mutex> slock(mtx_consumerinfo);
            groupid= info_basic.groupid;

        }

        for(const auto& [tp,pollqueue]:map_poll_queue){


                 size_t now_off= pollqueue.local_consume_offset.load();
                 size_t bytes= pull_bytes_once_of_request.load();
                    MB mb;
                    mb.append(groupid,tp.topic,tp.partition,now_off,bytes);
                    send(Eve:: CLIENT_REQUEST_PULL,mb.data);


        }




    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    //Producer


    MYMQ_Produceruse::MYMQ_Produceruse(const std::string& clientid,uint8_t ack_level):path_(MYMQ::run_directory_DEFAULT),cmc_(MYMQ::run_directory_DEFAULT,MYMQ::REQUEST_TIMEOUT_MS_DEFAULT),tbb_dctx_pool([]() {
            // 初始化函数：当新线程第一次访问时调用
            return ZSTD_createDCtx();
        }){

        Config_manager::ensure_path_existed(MYMQ::run_directory_DEFAULT);
        init(clientid,ack_level);
        cmc_.init();
    }
    MYMQ_Produceruse::~MYMQ_Produceruse(){

        for (ZSTD_DCtx* ctx : tbb_dctx_pool) {
            ZSTD_freeDCtx(ctx);
        }

    }
    Err_Client MYMQ_Produceruse::push(const MYMQ_Public::TopicPartition& tp, const std::string& key, const std::string& value, MYMQ_Public::PushResponceCallback cb) {

        // 1. 基础状态检查


        size_t curr_fly = SIZE_MAX;
        cmc_.get_curr_flying_request_num(curr_fly);
        if (curr_fly >= max_in_flight_requests_num) {
            cerr("[PUSH] FLYING REQUEST GOT TO LIMIT");
            return Err_Client::REACHED_MAX_FLYING_REQUEST;
        }


        auto it = map_push_queue.find(tp);
        if (it == map_push_queue.end()) {
            map_push_queue.emplace(std::piecewise_construct,
                                       std::forward_as_tuple(tp),
                                       std::forward_as_tuple(tp,batch_size));
        }

        auto& push_queue = it->second;
        std::unique_lock<std::mutex> ulock(push_queue.mtx);

        // 3. 检查压缩上下文
        if (!push_queue.cctx) {
            cerr("ZSTD ERROR : CCTX Unavailable . Push Interrupt");
            return Err_Client::ZSTD_UNAVAILABLE;
        }

        bool has_callback = (bool)cb;

        // 检查：如果用户传了有效回调，但客户端配置为“不响应”，则报错
        if (has_callback && ack_level_ == MYMQ::ACK_Level::ACK_NORESPONCE) {
            cerr("WARNING: Callback provided but ignored due to ACK_NORESPONCE level.");
            return Err_Client::INVALID_OPRATION;
        }

        while (true) {
            // A. 尝试直接写入 Active Buffer
            bool success = push_queue.active_buf->append_record(key, value);

            if (success) {
                // [核心逻辑修正]
                // 只有当回调对象“非空” 且 配置允许响应时，才入队保存
                if (has_callback && ack_level_ != MYMQ::ACK_Level::ACK_NORESPONCE) {
                    push_queue.active_cbs->push_back({
                        static_cast<uint32_t>(push_queue.current_batch_count),
                        std::move(cb) // 移动语义，存入后外部cb失效
                    });
                }

                push_queue.current_batch_count++;

                return Err_Client::NULL_ERROR;
            }

            // 1. 背压检查
            while (push_queue.is_flushing) {
                push_queue.cv_full.wait(ulock);
            }

            // 2. 交换双缓冲
            std::swap(push_queue.active_buf, push_queue.flushing_buf);
            std::swap(push_queue.active_cbs, push_queue.flushing_cbs);

            // 3. 重置计数器
            push_queue.current_batch_count = 0;

            // 4. 标记状态
            push_queue.is_flushing = true;

            // 5. 提交 Flush 任务
            auto push_queue_key = tp.topic + "_" + std::to_string(tp.partition);
            uint32_t shard_id = MurmurHash2::hash(push_queue_key);
            pool_.submit(shard_id, [this, &push_queue]() {
                this->flush_batch_task(push_queue);
            });

            // 6. 循环继续，重试 append_record
        }

        return Err_Client::NULL_ERROR;
    }


    void MYMQ_Produceruse::create_topic(const std::string& topicname,size_t parti_num){
        MessageBuilder mb;
        mb.append_string(topicname);
        mb.append_size_t(parti_num);
        auto req=mb.data;

        send(Eve::CLIENT_REQUEST_CREATE_TOPIC,req);

    }

    void MYMQ_Produceruse::init(const std::string& clientid,uint8_t ack_level){

        {

            Config_manager cm_sys(path_+"\\config\\sys.ini");

            auto thread_corenum= cm_sys.getint("max_threadnum_client");

            ThreadPool::instance(thread_corenum).start();


            auto zstd_level_tmp= cm_sys.getint("zstd_level");
            if(!inrange(zstd_level_tmp,1,22)){
                zstd_level=MYMQ::zstd_level_DEFAULT;
            }


            zstd_level=zstd_level_tmp;
            dctx=ZSTD_createDCtx();
            auto tmp_clientid=clientid;
            if(!inrange(tmp_clientid.size(),1,30)){
                tmp_clientid=MYMQ::CLIENTID_DEFAULT;
                cerr("Initialization : Invaild 'clientid' in config . Use default 'clientid' : "+MYMQ::CLIENTID_DEFAULT);
            }
            cmc_.set_clientid(tmp_clientid);
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
            ack_level_=static_cast<MYMQ::ACK_Level>(tmp_ack_level);


            size_t max_in_flight_requests_num_tmp= cm_sys.get_size_t("max_in_flight_requests_num");
            if(!inrange(zstd_level_tmp,1,5000)){
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
        }

    }



    void MYMQ_Produceruse::flush_batch_task(MYMQ::MYMQ_Client::Push_queue& pq) {
        MYMQ::MSG_serial::BatchBuffer* src_buf = pq.flushing_buf;
        auto* src_cbs = pq.flushing_cbs;

        if (src_buf->size() == 0) {
            finish_flush(pq);
            return;
        }

        // --- 准备内存 ---
        size_t zstd_bound = ZSTD_compressBound(src_buf->size());
        std::vector<unsigned char> final_packet;
        // 预估大小：Header + Meta + ZstdBound
        final_packet.resize(1024 + zstd_bound);

        unsigned char* ptr = final_packet.data();
        unsigned char* packet_start = ptr;

        // =========================================================
        // 第一层：网络包头 (Topic, Partition, BatchCRC, GlobalBodyLength)
        // =========================================================

        // 1. Topic (Length + Data)
        uint32_t topic_len = static_cast<uint32_t>(pq.tp.topic.size());
        uint32_t n_topic_len = htonl(topic_len); // [Network Order]
        std::memcpy(ptr, &n_topic_len, sizeof(uint32_t)); ptr += sizeof(uint32_t);
        std::memcpy(ptr, pq.tp.topic.data(), topic_len);  ptr += topic_len;

        // 2. Partition (8 bytes)
        uint64_t part_val = static_cast<uint64_t>(pq.tp.partition);
        uint64_t n_part = htonll(part_val);      // [Network Order]
        std::memcpy(ptr, &n_part, sizeof(uint64_t)); ptr += sizeof(uint64_t);

        // 3. Batch CRC 占位 (4 bytes)
        unsigned char* crc_ptr = ptr;
        ptr += sizeof(uint32_t);

        // 4. Global Body Length 占位 (4 bytes)
        unsigned char* global_len_ptr = ptr;
        ptr += sizeof(uint32_t);

        // =========================================================
        // 第二层：Batch Body (CRC 计算范围)
        // =========================================================
        unsigned char* body_start = ptr;

        // 5. Batch Base Offset (8 bytes) - Outer
        uint64_t base_offset = 0; // 实际逻辑中应填入真实 Offset
        uint64_t n_base_outer = htonll(base_offset); // [Network Order]
        std::memcpy(ptr, &n_base_outer, sizeof(uint64_t)); ptr += sizeof(uint64_t);

        // 6. Batch Internal Length 占位 (4 bytes)
        unsigned char* batch_internal_len_ptr = ptr;
        ptr += sizeof(uint32_t);

        // --- 开始 Payload (Consumer 内层解压范围) ---
        unsigned char* payload_start = ptr;

        // 7. Inner Base Offset (8 bytes)
        // 为了方便 Consumer 内部逻辑，重复写入 BaseOffset
        uint64_t n_base_inner = htonll(base_offset); // [Network Order]
        std::memcpy(ptr, &n_base_inner, sizeof(uint64_t)); ptr += sizeof(uint64_t);

        // 8. Record Count (8 bytes)
        uint64_t record_count = static_cast<uint64_t>(src_buf->record_count_);
        uint64_t n_record_count = htonll(record_count); // [Network Order]
        std::memcpy(ptr, &n_record_count, sizeof(uint64_t)); ptr += sizeof(uint64_t);

        // 9. ZSTD Length 占位 (4 bytes)
        unsigned char* zstd_len_ptr = ptr;
        ptr += sizeof(uint32_t);

        // 10. ZSTD Data (压缩写入)
        size_t capacity_left = final_packet.size() - (ptr - packet_start);
        size_t compressed_size = ZSTD_compressCCtx(
            pq.cctx, ptr, capacity_left,
            src_buf->data_ptr(), src_buf->size(),
            zstd_level
            );

        if (ZSTD_isError(compressed_size)) { finish_flush(pq); return; }
        ptr += compressed_size;

        // =========================================================
        // 回填阶段 (Backfill)
        // =========================================================

        // A. 回填 ZSTD Length
        uint32_t n_zstd_len = htonl(static_cast<uint32_t>(compressed_size)); // [Network Order]
        std::memcpy(zstd_len_ptr, &n_zstd_len, sizeof(uint32_t));

        // B. 回填 Batch Internal Length
        // Length = (InnerBase + InnerCount + ZstdLen + ZstdData)
        size_t internal_len = ptr - payload_start;
        uint32_t n_internal_len = htonl(static_cast<uint32_t>(internal_len)); // [Network Order]
        std::memcpy(batch_internal_len_ptr, &n_internal_len, sizeof(uint32_t));

        // C. 回填 Global Body Length
        size_t global_body_len = ptr - body_start;
        uint32_t n_global_len = htonl(static_cast<uint32_t>(global_body_len)); // [Network Order]
        std::memcpy(global_len_ptr, &n_global_len, sizeof(uint32_t));

        // D. 计算并回填 CRC
        uint32_t crc = MYMQ::Crc32::calculate_crc32(body_start, global_body_len);
        uint32_t n_crc = htonl(crc); // [Network Order]
        std::memcpy(crc_ptr, &n_crc, sizeof(uint32_t));

        // E. 发送
        final_packet.resize(ptr - packet_start);
        if (ack_level_ != MYMQ::ACK_Level::ACK_NORESPONCE) {
            send(Eve::CLIENT_REQUEST_PUSH, final_packet, std::move(*src_cbs));
        } else {
            send(Eve::CLIENT_REQUEST_PUSH, final_packet);
        }
        finish_flush(pq);
    }


    void MYMQ_Produceruse::finish_flush(MYMQ::MYMQ_Client::Push_queue& pq) {
        std::unique_lock<std::mutex> ulock(pq.mtx);

        // 1. 复用内存：只是重置指针，不释放 heap 内存
        pq.flushing_buf->clear();

        // 2. 清空回调 vector
        pq.flushing_cbs->clear();

        // 3. 归还控制权
        pq.is_flushing = false;

        // 4. 唤醒生产者
        pq.cv_full.notify_all();
    }

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


    bool MYMQ_Produceruse::send(MYMQ::EventType event_type, const Mybyte& msg_body, std::vector<MYMQ::MYMQ_Client::SparseCallback> cbs_)
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
                                  (uint16_t event_type_responce, const Mybyte& msg_body_responce) mutable
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
                                                      specific_cb(MYMQ_Public::CommonErrorCode::NULL_ERROR);
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


    MYMQ_Public::ResultVariant MYMQ_Produceruse::handle_response(Eve event_type,const Mybyte& msg_body){

        MessageParser mp(msg_body.data(),msg_body.size());
        if(event_type==MYMQ::EventType::SERVER_RESPONSE_PUSH_ACK){
            auto topicname=mp.read_string();
            auto partition=mp.read_size_t();
            auto error=static_cast<Err>(mp.read_uint16());
            size_t base=SIZE_MAX;
            if(error==Err::NULL_ERROR){
                base=mp.read_size_t();
            }

            return  MYMQ_Public::PushResponce(std::move(topicname),partition,error,base);
        }  
        else if(event_type==Eve::SERVER_RESPONSE_CREATE_TOPIC){
            auto res= mp.read_bool();
            if(res){
                cerr("CREATE TOPIC RESULT : Topic created successfully");
            }
            else{
                cerr("CREATE TOPIC RESULT : Topic created failed");

            }
        }
        else if(event_type==Eve::EVENTTYPE_NULL){
            return Err::REQUEST_TIMEOUT;
        }


        return MYMQ_Public::CommonErrorCode::NULL_ERROR;
    }

    void MYMQ_Produceruse::push_timer_send() {

        // 2. 遍历所有分区队列
        // 假设 map_push_queue 是 tbb::concurrent_hash_map 或 std::unordered_map
        // 如果是 TBB map，遍历通常是线程安全的，但要注意锁粒度
        for (auto& it : map_push_queue) {
            // key 是 partition string, value 是 Push_queue
            const TopicPartition& pq_key = it.first;
            auto& pq = it.second;

            // 【优化】无锁预检查 (Dirty Check)
            // active_buf 是指针，读取指针指向的 size 是相对安全的（哪怕读到旧值也没事，下次再发）
            // 如果 size 为 0，直接跳过，绝不抢锁
            if (pq.active_buf->size() == 0) {
                continue;
            }

            // ==========================================
            // 进入临界区
            // ==========================================
            std::unique_lock<std::mutex> ulock(pq.mtx);

            // A. Double Check: 再次确认是否有数据
            if (pq.active_buf->size() == 0) {
                continue;
            }

            // B. 【关键】检查后台是否忙碌 (Backpressure)
            // 如果 is_flushing 为 true，说明上一次的 Batch 还没压完/发完。
            // 此时定时器不能强制 Swap，否则会覆盖 flushing_buf 里的数据！
            // 策略：跳过本次，让数据继续积攒，等后台空闲了再说。
            if (pq.is_flushing) {
                continue;
            }

            // ==========================================
            // C. 执行交换 (Swap) - 极速操作
            // ==========================================
            // 既然后台不忙，且 active 有数据，说明因为没填满所以没触发 push 里的 flush
            // 定时器强制触发它！
            std::swap(pq.active_buf, pq.flushing_buf);
            std::swap(pq.active_cbs, pq.flushing_cbs);

            // 标记后台忙
            pq.is_flushing = true;

            // ==========================================
            // D. 派发给线程池 (非阻塞)
            // ==========================================

            auto key_hash=pq_key.topic+"_"+std::to_string( pq_key.partition);
            uint32_t shard_id = MurmurHash2::hash(key_hash);

            pool_.submit(shard_id, [this, &pq]() {
                this->flush_batch_task(pq);
            });


        }
    }
