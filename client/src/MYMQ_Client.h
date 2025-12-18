#ifndef MYMQ_CLIENT_H
#define MYMQ_CLIENT_H
#include "Communication.h"
#include "MurmurHash2.h"
#include"MYMQ_innercodes.h"
#include"MYMQ_PublicCodes.h"
#include"SharedThreadPool.h"
#include"tbb/parallel_for_each.h"
#include"tbb/enumerable_thread_specific.h"
#include"Timer.h"
#include <unordered_set>


using Consumerbasicinfo=MYMQ::MYMQ_Client::Consumerbasicinfo;
using Eve=MYMQ::EventType;
using Err=MYMQ_Public::CommonErrorCode;
using MB=MessageBuilder;
using Err_Client=MYMQ_Public::ClientErrorCode;
using Mybyte=std::vector<unsigned char>;
using TopicPartition=MYMQ_Public::TopicPartition;
using TP_Point=MYMQ::MYMQ_Client::TP_Point;
using TP_PointMap =tbb::concurrent_hash_map<TopicPartition,TP_Point >;
// struct TP_Point
// {
//     std::shared_ptr<endoffset_point> endoffset_ptr=nullptr;
//     std::shared_ptr<PollBuffer> pollqueue_ptr=nullptr;
// };

class MYMQ_Produceruse{


public:
    MYMQ_Produceruse(const std::string& clientid=std::string(),uint8_t ack_level=UINT8_MAX);
    MYMQ_Produceruse(const MYMQ_Produceruse&)=delete;
    MYMQ_Produceruse& operator= (const MYMQ_Produceruse&)=delete;
    ~MYMQ_Produceruse();




    Err_Client push(const MYMQ_Public::TopicPartition& tp,const std::string& key,const std::string& value
                    ,MYMQ_Public::PushResponceCallback cb) ;

  \


    void create_topic(const std::string& topicname,size_t parti_num=1);

private:


    void flush_batch_task(MYMQ::MYMQ_Client::Push_queue& pq);
    void finish_flush(MYMQ::MYMQ_Client::Push_queue& pq);


    void push_perioric_start();
    void push_perioric_stop();


    void init(const std::string& clientid,uint8_t ack_level);


    bool send(MYMQ::EventType event_type, const Mybyte& msg_body, std::vector<MYMQ::MYMQ_Client::SparseCallback> cbs_=std::vector<MYMQ::MYMQ_Client::SparseCallback>());

    MYMQ_Public::ResultVariant handle_response(Eve event_type,const Mybyte& msg_body);

    void push_timer_send();
    bool is_register();
    void cerr(const std::string& str){
        Printqueue::instance().out(str,1,0);
    }

    void out(const std::string& str){
        Printqueue::instance().out(str,0,0);
    }


    bool inrange(size_t obj,size_t min,size_t max){
        return (obj<=max&&obj>=min);
    }

private:
    //Config配置项
    size_t zstd_level;
    size_t batch_size;
    size_t autopush_perior_ms;
    size_t max_in_flight_requests_num;
    size_t local_push_buffer_size;

    //Config配置项
    std::string path_;
    Communication_client cmc_;

    Timer timer;
    size_t push_perioric_taskid{0};

    ClientState state;
    std::mutex mtx_state;


    Consumerbasicinfo info_basic;

    MYMQ::MYMQ_Client::RecordAccumulator recordaccumulator;


    ZSTD_DCtx* dctx;
    MYMQ::ACK_Level ack_level_;

    tbb::enumerable_thread_specific<ZSTD_DCtx*> tbb_dctx_pool;
    ShardedThreadPool& pool_=ShardedThreadPool::instance(8);


};









class MYMQ_Consumeruse{

    struct Workitem {
        size_t index;
        MYMQ::MYMQ_Client::TopicPartition tp;
        std::vector<unsigned char> raw_big_chunk;
        std::vector<MYMQ_Public::ConsumerRecord> parsed_records;
        MYMQ_Public::ClientErrorCode err = Err_Client::NULL_ERROR;
        Workitem(size_t i, MYMQ::MYMQ_Client::TopicPartition t, std::vector<unsigned char> r)
            : index(i), tp(std::move(t)), raw_big_chunk(std::move(r)) {}
    };

public:
    MYMQ_Consumeruse(const std::string& clientid=std::string(),uint8_t ack_level=UINT8_MAX);
    MYMQ_Consumeruse(const MYMQ_Consumeruse&)=delete;
    MYMQ_Consumeruse& operator= (const MYMQ_Consumeruse&)=delete;
    ~MYMQ_Consumeruse();



    Err_Client get_local_consumed_position(const MYMQ_Public::TopicPartition& tp,size_t& pos);



    Err_Client push(const MYMQ_Public::TopicPartition& tp,const std::string& key,const std::string& value
                    ,MYMQ_Public::PushResponceCallback cb) ;

    Err_Client pull(std::vector< MYMQ_Public::ConsumerRecord>& record_batc,size_t poll_wait_timeout_ms) ;
    Err_Client pull(std::vector<MYMQ_Public::ConsumerRecord>& record_batch,
                                    size_t poll_wait_timeout_ms,
                                    int64_t& out_latency_us);



    void create_topic(const std::string& topicname,size_t parti_num=1);
    void set_pull_bytes(size_t bytes);

    void subscribe_topic(const std::string& topicname);
    void unsubscribe_topic(const std::string& topicname);

    Err_Client commit_sync(const MYMQ_Public::TopicPartition& tp,size_t next_offset_to_consume) ;



    Err_Client commit_async(const MYMQ_Public::TopicPartition& tp,size_t next_offset_to_consume,MYMQ_Public::CommitAsyncResponceCallback cb=MYMQ_Public::CommitAsyncResponceCallback()) ;

    Err_Client join_group(const std::string& groupid);
    Err_Client leave_group();



    std::unordered_set<MYMQ_Public::TopicPartition> get_assigned_partition();
    Err_Client seek(const MYMQ_Public::TopicPartition& tp,size_t offset_next_to_consume);


     bool get_is_ingroup(){
        return is_ingroup.load();
    }
    void set_local_pull_bytes_once(size_t bytes);

       void  trigger_poll_for_low_cap_pollbuffer();

private:
    void call_parse_impl(
        const std::vector<unsigned char>& raw_big_chunk,            // IO 线程收到的原始大包
        std::vector<MYMQ_Public::ConsumerRecord>& out_records,      // 输出结果
        const MYMQ::MYMQ_Client::TopicPartition& tp,                // 所属分区
        Err_Client& out_error                                       // 错误码传出
        ) ;



    void sync_group() ;
    void heartbeat(bool topics_updated=0,std::string groupid ="") ;


    void heartbeat_start();

    void heartbeat_stop();

    void poll_perioric_start();
    void poll_perioric_stop();

    void autocommit_start();
    void autocommit_stop();

    void init(const std::string& clientid,uint8_t ack_level);

    void timer_commit_async();
    bool send(MYMQ::EventType event_type, const Mybyte& msg_body, std::vector<MYMQ::MYMQ_Client::SparseCallback> cbs_=std::vector<MYMQ::MYMQ_Client::SparseCallback>());

    Err_Client commit_inter(const MYMQ_Public::TopicPartition& tp,size_t next_offset_to_consume,MYMQ_Public::CommitAsyncResponceCallback cb);
    MYMQ_Public::ResultVariant handle_response(Eve event_type,const Mybyte& msg_body);
    void out_group_reset();
    void cerr(const std::string& str){
        Printqueue::instance().out(str,1,0);
    }

    void out(const std::string& str){
        Printqueue::instance().out(str,0,0);
    }


    bool inrange(size_t obj,size_t min,size_t max){
        return (obj<=max&&obj>=min);
    }

private:
    //Config配置项
    size_t join_collect_timeout_ms; // 重平衡join窗口期
    size_t rebalance_timeout_ms; // 重平衡超时时长
    size_t heartbeat_interval_ms;
    size_t memberid_wait_timeout_s;
    size_t commit_wait_timeout_s;

    size_t zstd_level;
    size_t local_pollqueue_size;
    size_t batch_size;
    MYMQ::PullSet pull_start_location;
    size_t autopush_perior_ms;
    size_t autocommit_perior_ms;
    bool is_auto_commit;
    size_t max_in_flight_requests_num;
    std::atomic<size_t> local_pull_bytes_once{10000000};


    //Config配置项


    std::string path_;
    Communication_client cmc_;


    Timer timer;
    size_t heartbeat_taskid{0};
    size_t push_perioric_taskid{0};
    size_t autocommit_taskid{0};
    size_t autopoll_taskid{0};


    ClientState state;
    std::mutex mtx_state;

    Consumerbasicinfo info_basic;

    std::atomic<bool> is_ingroup{0};
    bool is_leader{0};

    TP_PointMap map_final_assign;


    std::atomic<size_t>  pull_bytes_once_of_request;

    std::condition_variable cv_commit_ready;
    std::atomic<bool> commit_ready{0};
    std::mutex mtx_commit_ready;

    std::condition_variable cv_poll_ready;
    std::atomic<bool> poll_ready{0};
    std::mutex mtx_poll_ready;




    ZSTD_DCtx* dctx;
    MYMQ::ACK_Level ack_level_;

    tbb::enumerable_thread_specific<ZSTD_DCtx*> tbb_dctx_pool;
    ShardedThreadPool& pool_=ShardedThreadPool::instance(8);



    std::vector<Workitem> m_todo_cache;



};


#endif
