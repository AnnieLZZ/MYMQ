#include"MYMQ_Client.h"
MYMQ_clientuse::MYMQ_clientuse(const std::string& clientid,uint8_t ack_level):path_(MYMQ::run_directory_DEFAULT),cmc_(MYMQ::run_directory_DEFAULT){

    Config_manager::ensure_path_existed(MYMQ::run_directory_DEFAULT);
    init(clientid,ack_level);
     cmc_.init();
}
MYMQ_clientuse::~MYMQ_clientuse(){

     out_group_reset();
    cv_commit_ready.notify_all();
    cv_poll_ready.notify_all();


}



    void MYMQ_clientuse::exit_rebalance(){

        rebalance_ing.store(0);
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

            auto key=tp.topic+"_"+std::to_string(tp.partition);

            Endoffsetmap::accessor ac;
            auto it=map_end_offset.find(ac,key);
            if(!it){
                cerr("ERROR :Invalid topic or partition in 'commit_inter' function");
                return MYMQ_Public::ClientErrorCode::INVALID_PARTITION_OR_TOPIC;
            }
            else{
                ac->second.off=offset_next_to_consume;
            }
            ac.release();
              out("[Seek offset] Current offset : "+std::to_string(offset_next_to_consume));
             return Err_Client::NULL_ERROR;


    }



    Err_Client MYMQ_clientuse::push(const MYMQ_Public::TopicPartition& tp, const std::string& key, const std::string& value, MYMQ_Public::PushResponceCallback cb) {

        // 1. 基础状态检查
        if (!is_ingroup.load()) {

            return Err_Client::NOT_IN_GROUP;
        }

        size_t curr_fly = SIZE_MAX;
        cmc_.get_curr_flying_request_num(curr_fly);
        if (curr_fly >= max_in_flight_requests_num) {
            cerr("[PUSH] FLYING REQUEST GOT TO LIMIT");
            return Err_Client::REACHED_MAX_FLYING_REQUEST;
        }

        // 2. 查找对应的 Partition Queue
        auto push_queue_key = tp.topic + "_" + std::to_string(tp.partition);
        auto it = map_push_queue.find(push_queue_key);
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
            uint32_t shard_id = MurmurHash2::hash(push_queue_key);
            pool_.submit(shard_id, [this, &push_queue]() {
                this->flush_batch_task(push_queue);
            });

            // 6. 循环继续，重试 append_record
        }

        return Err_Client::NULL_ERROR;
    }

    void MYMQ_clientuse::flush_batch_task(MYMQ::MYMQ_Client:: Push_queue& pq) {
       MYMQ::MSG_serial:: BatchBuffer* src_buf = pq.flushing_buf;
        auto* src_cbs = pq.flushing_cbs;

        if (src_buf->size() == 0) {
            finish_flush(pq);
            return;
        }

        // 1. 准备内存
        size_t zstd_bound = ZSTD_compressBound(src_buf->size());
        // 预留足够头部空间
        std::vector<unsigned char> final_packet;
        final_packet.resize(1024 + zstd_bound);

        unsigned char* ptr = final_packet.data();
        unsigned char* packet_start = ptr;

        // =========================================================
        // 第一层：最外层包头 (Topic, Partition, CRC)
        // =========================================================

        // 1. Topic (Len + Data)
        uint32_t topic_len = static_cast<uint32_t>(pq.tp.topic.size());
        uint32_t n_topic_len = htonl(topic_len);
        std::memcpy(ptr, &n_topic_len, sizeof(uint32_t)); ptr += sizeof(uint32_t);
        std::memcpy(ptr, pq.tp.topic.data(), topic_len);  ptr += topic_len;

        // 2. Partition (对应 size_t, 8字节)
        uint64_t part_val = static_cast<uint64_t>(pq.tp.partition);
        uint64_t n_part = htonll(part_val); // 需要 #include <endian.h>
        std::memcpy(ptr, &n_part, sizeof(uint64_t)); ptr += sizeof(uint64_t);

        // 3. CRC 占位 (4字节)
        unsigned char* crc_ptr = ptr;
        ptr += sizeof(uint32_t);

        // 4. 【关键】Body Length 占位 (4字节)
        // 对应原始代码: mb_batch.append(..., records_with_header.data)
        // 这里代表 records_with_header 这个 vector 的总长度
        unsigned char* body_len_ptr = ptr;
        ptr += sizeof(uint32_t);

        // =========================================================
        // 第二层：records_with_header (CRC 计算范围)
        // =========================================================
        unsigned char* body_start = ptr; // CRC 计算起点

        // 5. BaseOffset (8字节)
        // 对应原始代码: placeholder_base_offset
        uint64_t base_offset = 0;
        std::memcpy(ptr, &base_offset, sizeof(uint64_t)); ptr += sizeof(uint64_t);

        // 6. RecordNum (8字节)
        // 对应原始代码: record_num (size_t)
        uint64_t record_count = static_cast<uint64_t>(src_buf->record_count_);
        uint64_t n_record_count = htonll(record_count);
        std::memcpy(ptr, &n_record_count, sizeof(uint64_t)); ptr += sizeof(uint64_t);

        // 7. 【关键】ZSTD Length 占位 (4字节)
        // 对应原始代码: records_with_header.append(..., zstd_records)
        // 这里代表 zstd_records 这个 vector 的长度
        unsigned char* zstd_len_ptr = ptr;
        ptr += sizeof(uint32_t);

        // 8. ZSTD 数据 (直接压缩写入)
        size_t capacity_left = final_packet.size() - (ptr - packet_start);
        size_t compressed_size = ZSTD_compressCCtx(
            pq.cctx, ptr, capacity_left,
            src_buf->data_ptr(), src_buf->size(),
            zstd_level
            );

        if (ZSTD_isError(compressed_size)) { finish_flush(pq); return; }

        // 更新指针
        ptr += compressed_size;

        // =========================================================
        // 回填阶段 (Backfill)
        // =========================================================

        // A. 回填 ZSTD Length
        uint32_t n_zstd_len = htonl(static_cast<uint32_t>(compressed_size));
        std::memcpy(zstd_len_ptr, &n_zstd_len, sizeof(uint32_t));

        // Resize vector 到实际大小
        size_t final_total_len = ptr - packet_start;
        final_packet.resize(final_total_len);

        // B. 回填 Body Length (records_with_header.data.size())
        size_t body_len = ptr - body_start;
        uint32_t n_body_len = htonl(static_cast<uint32_t>(body_len));
        std::memcpy(body_len_ptr, &n_body_len, sizeof(uint32_t));

        // C. 计算并回填 CRC
        // 原始代码: calculate_crc32(records_with_header.data...)
        // 所以计算范围就是 body_start 到 结束
        uint32_t crc = MYMQ::Crc32::calculate_crc32(body_start, body_len);
        uint32_t n_crc = htonl(crc);
        std::memcpy(crc_ptr, &n_crc, sizeof(uint32_t));

        // =========================================================
        // 发送
        // =========================================================
        if (ack_level_ != MYMQ::ACK_Level::ACK_NORESPONCE) {
            send(Eve::CLIENT_REQUEST_PUSH, final_packet, std::move(*src_cbs));
        } else {
            send(Eve::CLIENT_REQUEST_PUSH, final_packet);
        }

        finish_flush(pq);
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
         auto key=tp.topic+"_"+std::to_string(tp.partition);
        Endoffsetmap::const_accessor cac;
        auto it=map_end_offset.find(cac,key);
        if(!it){
            return SIZE_MAX;
        }
        return cac->second.off;
    }

    Err_Client MYMQ_clientuse::pull(const MYMQ_Public::TopicPartition& tp,std::vector< MYMQ_Public::ConsumerRecord>& record_batch) {

        if(!record_batch.empty()){
            return Err_Client::INVALID_OPRATION;
        }
        if(!is_ingroup.load()){

            return  Err_Client::NOT_IN_GROUP;
        }

        std::string groupid;
        {
            std::shared_lock<std::shared_mutex> slock(mtx_consumerinfo);
            groupid= info_basic.groupid;

        }

        auto key=tp.topic+"_"+std::to_string(tp.partition);

        Endoffsetmap::const_accessor cac;
        auto it=map_end_offset.find(cac,key);
        if(!it){
            cerr("ERROR :Invalid topic or partition in 'Pull' function");

            return Err_Client::INVALID_PARTITION;
        }
        size_t now_off=cac->second.off;

        cac.release();



        out("[Pull] Attempt to pull from Topic : '"+tp.topic+"' Partition : "+std::to_string(tp. partition)+" at Offset :"+std::to_string(now_off));
        size_t bytes= pull_bytes_once.load();
        MB mb;
        mb.append(groupid,tp.topic,tp.partition,now_off,bytes);
        send(Eve:: CLIENT_REQUEST_PULL,mb.data);




        bool poll_responced=0;
        {
            std::unique_lock<std::mutex> ulock(mtx_poll_ready);
            poll_responced = cv_poll_ready.wait_for(ulock,std::chrono::seconds(poll_wait_timeout_s),[this]{
                return poll_ready.load();
            });

        }
        poll_ready.store(0);

        if(poll_responced){
            if(poll_queue.try_dequeue(record_batch)){

                if(record_batch.empty()){
                    return Err_Client::EMPTY_RECORD;
                }
                auto&& rear_msg=record_batch.back();
                Endoffsetmap::accessor ac;
                 if(!map_end_offset.find(ac,rear_msg.getTopic()+"_"+std::to_string(rear_msg.getPartition()) )){
                    cerr("[PULL] Auto update local offset failed.");
                    return Err_Client::INVALID_PARTITION_OR_TOPIC;
                 }
                 ac->second.off=rear_msg.getOffset()+1;
                 out("[PULL] Auto update local offset '"+std::to_string(rear_msg.getOffset()) +"'");

                return Err_Client::NULL_ERROR;
            }
            else{
                return Err_Client::UNKNOWN_ERROR;
            }
        }
        return Err_Client::PULL_TIMEOUT;
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
            pull_bytes_once.store(bytes);
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

            auto pollqueue_size=cm_sys.get_size_t("local_pollqueue_size");
            if(!inrange(pollqueue_size,1024,16384)){
                pollqueue_size=MYMQ::pollqueue_size_DEFAULT;
            }

            {
                auto tmppollqueue=moodycamel::ReaderWriterQueue<std::vector< MYMQ_Public::ConsumerRecord>>(pollqueue_size);
                poll_queue=std::move(tmppollqueue);
            }

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
            poll_wait_timeout_s=cm_business.get_size_t("poll_wait_timeout_s");
            rebalance_timeout_ms=MYMQ::rebalance_timeout_ms;
            join_collect_timeout_ms=MYMQ::join_collect_timeout_ms;
            memberid_wait_timeout_s=MYMQ::memberid_ready_timeout_s;
            commit_wait_timeout_s=MYMQ::commit_ready_timeout_s;
            pull_bytes_once=cm_business.get_size_t("pull_bytes");
            batch_size=cm_business.get_size_t("batch_size");
            autopush_perior_ms=cm_business.get_size_t ("autopush_perior_ms");
            autocommit_perior_ms=cm_business.get_size_t ("autocommit_perior_ms");


        }

    }



    void MYMQ_clientuse::timer_commit_async(){
        if(!is_ingroup.load()){
            return ;
        }

        std::vector<std::string> all_keys;
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
             commit_async(val.tp,val.off);
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
        auto key=tp.topic+"_"+std::to_string(tp.partition);

        Endoffsetmap::const_accessor cac;
        auto it=map_end_offset.find(cac,key);
        if(!it){
            cerr("ERROR :Invalid topic or partition in 'commit_inter' function");
            return MYMQ_Public::ClientErrorCode::INVALID_PARTITION_OR_TOPIC;
        }

        size_t now_off =cac->second.off;
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

        MessageParser mp(msg_body);
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
                    MP mp_assignment_inf(assignment_inf);
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


            if(error==Err::NULL_ERROR){
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
                    std::unordered_map<std::string,MYMQ::MYMQ_Client::Push_queue> tmp_map_push_queue;
                    auto size_tmp=tmp_queue_endoffset.size();
                    tmp_map_push_queue.reserve(size_tmp);


                    while(!tmp_queue_endoffset.empty()){
                        auto [par,off] =tmp_queue_endoffset.front();
                        std::string key=par.first+"_"+std::to_string( par.second);
                        tmp_queue_endoffset.pop();
                        Endoffsetmap::accessor ac;
                        tmp_map_endoffset.emplace(
                            ac,
                            std::piecewise_construct,
                            std::make_tuple(key),
                            std::make_tuple(off, par.first, par.second)
                            );
                        ac.release();

                        tmp_map_push_queue.emplace(std::piecewise_construct,
                                                   std::forward_as_tuple(key),
                                                   std::forward_as_tuple(par.first,par.second,batch_size));
                    }

                    map_end_offset=std::move(tmp_map_endoffset);
                    map_push_queue=std::move(tmp_map_push_queue);

               }

                cerr("[Sync group] Sync success." );
                cerr("[Sync group] JoinGroup success");


                is_ingroup.store(1);
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
            auto pull_inf_additional = mp.read_uchar_vector();
            MessageParser mp_pull_inf_additional(pull_inf_additional);

            auto topicname_ = mp_pull_inf_additional.read_string();
            auto partition_ = mp_pull_inf_additional.read_size_t();
            auto shared_key_raw = topicname_ + std::to_string(partition_);
            auto shared_key = MurmurHash2::hash(shared_key_raw, 0x9747b28c);
            auto error = static_cast<Err>(mp_pull_inf_additional.read_uint16());
            auto offset_batch_first = mp_pull_inf_additional.read_size_t();

            out("[PULL] Messages batch from (TOPIC '" + topicname_ + "' PARTITION '" + std::to_string(partition_) + ") responce reached. ");
            out(std::string{} + "[PULL] Result : " + " State : " + MYMQ_Public::to_string(error));

            if(error == Err::NULL_ERROR) {
                // 读取整个消息集合的大包 (这是唯一一次大的内存移动，之后全部是指针操作)
                auto message_collection_ = mp.read_uchar_vector();

                // 提交任务到线程池
                ShardedThreadPool::instance().submit(shared_key, [this,
                                                                  topicname = std::move(topicname_),
                                                                  partition = partition_,
                                                                  message_collection = std::move(message_collection_)]
                                                     {
                                                         size_t current_position = 0;
                                                         const size_t collection_size = message_collection.size();
                                                         const size_t RECORD_HEADER_SIZE = sizeof(uint64_t) + sizeof(uint32_t);

                                                         // 结果容器
                                                         std::vector<MYMQ_Public::ConsumerRecord> CRecordbatch;
                                                         bool is_interrupted = false;


                                                         while (current_position < collection_size) {
                                                             // 1. 边界检查
                                                             if (current_position + RECORD_HEADER_SIZE > collection_size) {
                                                                 if (collection_size > 0 && current_position < collection_size) {
                                                                     std::cerr << "Warning: Partial message header detected." << std::endl;
                                                                 }
                                                                 is_interrupted = true;
                                                                 break;
                                                             }

                                                             // 2. 读取当前 Record Batch 的长度
                                                             uint32_t record_size;
                                                             std::memcpy(&record_size, message_collection.data() + current_position + sizeof(uint64_t), sizeof(uint32_t));
                                                             record_size = ntohl(record_size);

                                                             if (record_size == 0) {
                                                                 std::cerr << "Warning: Zero length message." << std::endl;
                                                                 is_interrupted = true; break;
                                                             }
                                                             if (current_position + RECORD_HEADER_SIZE + record_size > collection_size) {
                                                                 std::cerr << "Error: Truncated message body." << std::endl;
                                                                 is_interrupted = true; break;
                                                             }

                                                             // ==============================================================
                                                             // 【不得不 Copy】因为 MessageParser 构造函数必须要 vector
                                                             // ==============================================================
                                                             auto payload_begin = message_collection.begin() + current_position + RECORD_HEADER_SIZE;
                                                             // 将当前 Batch 的数据拷贝到一个临时 vector
                                                             std::vector<unsigned char> payload_vec(payload_begin, payload_begin + record_size);

                                                             try {
                                                                 // 构造 Payload 解析器
                                                                 MessageParser mp_payload(payload_vec);

                                                                 auto base = mp_payload.read_size_t();
                                                                 auto record_num = mp_payload.read_size_t();

                                                                 auto [comp_ptr, comp_len] = mp_payload.read_bytes_view();
                                                                 auto records_nozstd = MYMQ::ZSTD::zstd_decompress_using_view(dctx, comp_ptr, comp_len);

                                                                 // 解析解压后的数据块
                                                                 MessageParser mp_records_serial(records_nozstd);

                                                                 for(size_t i = 0; i < record_num; i++) {

                                                                     std::vector<unsigned char> record_serial_vec = mp_records_serial.read_uchar_vector();

                                                                     if (record_serial_vec.empty()) throw std::out_of_range("EMPTY RECORD");

                                                                     MessageParser mp_single(record_serial_vec);
                                                                     auto crc = mp_single.read_uint32();


                                                                     if (record_serial_vec.size() < sizeof(uint32_t)) {
                                                                         throw std::out_of_range("Record too short");
                                                                     }

                                                                     const unsigned char* msg_ptr = record_serial_vec.data() + sizeof(uint32_t);
                                                                     size_t msg_len = record_serial_vec.size() - sizeof(uint32_t);

                                                                     if(!MYMQ::Crc32::verify_crc32(msg_ptr, msg_len, crc)) {
                                                                         throw std::out_of_range("Crc verify failed");
                                                                     }

                                                                     std::vector<unsigned char> msg_body_vec(msg_ptr, msg_ptr + msg_len);
                                                                     MessageParser mp_kv(msg_body_vec);

                                                                     auto key_view = mp_kv.read_string_view();
                                                                     auto value_view = mp_kv.read_string_view();
                                                                     auto time = mp_kv.read_int64();

                                                                     CRecordbatch.emplace_back(
                                                                         topicname,
                                                                         partition,
                                                                         std::string(key_view),
                                                                         std::string(value_view),
                                                                         time,
                                                                         base + i
                                                                         );
                                                                 }
                                                             } catch (const std::exception& e) {
                                                                std::cerr << "Error parsing record batch: " << e.what() << std::endl;
                                                                 is_interrupted = true;
                                                                 break;
                                                             }

                                                             // 更新游标
                                                             current_position += (RECORD_HEADER_SIZE + record_size);
                                                             if (current_position > collection_size) { is_interrupted = true; break; }

                                                         } // End While

                                                         if(is_interrupted) {
                                                             // cerr("PULL be interrupt by some error");
                                                         }

                                                         // 提交结果
                                                         if (!poll_queue.try_emplace(std::move(CRecordbatch))) {
                                                             // cerr("Poll result : Poll queue full.");
                                                         }

                                                         poll_ready.store(1);
                                                         cv_poll_ready.notify_all();

                                                     }); // End Lambda
            }
            else {
                // 处理服务器返回非 NULL_ERROR 的情况 (如没有数据等)
                poll_ready.store(1);
                cv_poll_ready.notify_all();
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

            if(generation_id!=generation_id_local&&(groupstate_digit!=2)){
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


                    Endoffsetmap::accessor ac;
                    auto it=map_end_offset.find(ac,key);
                    if(!it){
                        cerr("Error : Failed to update committed offset : Invalid topic or partition .");
                        return MYMQ_Public::CommonErrorCode::INTERNAL_ERROR;
                    }
                    auto& point=ac->second;
                    point.off=offset ;


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
        // 1. 基础检查
        if (!is_ingroup.load()) {
            return;
        }

        // 2. 遍历所有分区队列
        // 假设 map_push_queue 是 tbb::concurrent_hash_map 或 std::unordered_map
        // 如果是 TBB map，遍历通常是线程安全的，但要注意锁粒度
        for (auto& it : map_push_queue) {
            // key 是 partition string, value 是 Push_queue
            const std::string& pq_key = it.first;
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
            // 使用 Key 做 Hash，保证依然由同一个分片线程处理，保持 CPU 亲和性
            // 注意：这里 it.first 就是 key，不需要再拼字符串了，省了 malloc
            uint32_t shard_id = MurmurHash2::hash(pq_key);

            pool_.submit(shard_id, [this, &pq]() {
                this->flush_batch_task(pq);
            });

            // 锁在循环末尾自动释放，立即处理下一个 Queue
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
    }





