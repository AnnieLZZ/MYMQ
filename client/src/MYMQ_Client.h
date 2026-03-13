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


using Consumerbasicinfo=MYMQ::Client::Consumerbasicinfo;
using Eve=MYMQ::EventType;
using Err=MYMQ_Public::CommonErrorCode;
using MB=MessageBuilder;
using Err_Client=MYMQ_Public::ClientErrorCode;
using Mybyte=std::vector<unsigned char>;
using TopicPartition=MYMQ_Public::TopicPartition;
using TP_Point=MYMQ::Client::TP_Point;
using TP_PointMap =tbb::concurrent_hash_map<TopicPartition,TP_Point >;

namespace MYMQ {
namespace Client {

class ClientBase {
public:
    ClientBase(const std::string& path) : cmc_(path, MYMQ::REQUEST_TIMEOUT_MS_DEFAULT) {}
    virtual ~ClientBase() = default;

protected:
    MYMQ::Network::Communication_client cmc_;
    size_t max_in_flight_requests_num = MYMQ::MAX_IN_FLIGHT_REQUEST_NUM_DEFAULT;

    // Common send method
    bool send(MYMQ::EventType event_type, const Mybyte& msg_body, std::vector<MYMQ::Client::SparseCallback> cbs_ = std::vector<MYMQ::Client::SparseCallback>());
    bool send_via(MYMQ::Network::Communication_client& channel, MYMQ::EventType event_type, const Mybyte& msg_body, std::vector<MYMQ::Client::SparseCallback> cbs_ = std::vector<MYMQ::Client::SparseCallback>());

    // Virtual hook for response handling
    virtual MYMQ_Public::ResultVariant handle_response(Eve event_type, const Mybyte& msg_body) = 0;
};

class MYMQ_Produceruse : public ClientBase {


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


    void flush_batch_task(MYMQ::Client::Push_queue& pq);


    void push_perioric_start();
    void push_perioric_stop();


    void init(const std::string& clientid,uint8_t ack_level);


    MYMQ_Public::ResultVariant handle_response(Eve event_type,const Mybyte& msg_body) override;

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
    //size_t max_in_flight_requests_num; // Moved to base
    size_t local_push_buffer_size;
    size_t push_max_queued_batches;

    //Config配置项
    std::string path_;
    MYMQ::Network::Communication_client cmc_produce_{MYMQ::run_directory_DEFAULT, MYMQ::REQUEST_TIMEOUT_MS_DEFAULT};

    Timer timer;
    size_t push_perioric_taskid{0};

    ClientState state;
    std::mutex mtx_state;


    Consumerbasicinfo info_basic;

    MYMQ::Client::RecordAccumulator recordaccumulator;


    ZSTD_DCtx* dctx;
    MYMQ::ACK_Level ack_level_;

    tbb::enumerable_thread_specific<ZSTD_DCtx*> tbb_dctx_pool;
    ShardedThreadPool& pool_=ShardedThreadPool::instance(8);


};









class MYMQ_Consumeruse : public ClientBase {

    struct Workitem {
        size_t index;
        MYMQ_Public::TopicPartition tp;
        std::vector<unsigned char> raw_big_chunk;
        std::vector<MYMQ_Public::ConsumerRecord> parsed_records;
        MYMQ_Public::ClientErrorCode err = Err_Client::Success;
        Workitem(size_t i, MYMQ_Public::TopicPartition t, std::vector<unsigned char> r)
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
    void set_pull_fetch_min_bytes(size_t bytes);

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
    void set_pull_max_record_num_local(size_t bytes);

       void  trigger_poll_for_low_cap_pollbuffer();

private:
    MYMQ::Network::Communication_client cmc_fetch_{MYMQ::run_directory_DEFAULT, MYMQ::REQUEST_TIMEOUT_MS_DEFAULT};
    void call_parse_impl(
        const std::vector<unsigned char>& raw_big_chunk,            // IO 线程收到的原始大包
        std::vector<MYMQ_Public::ConsumerRecord>& out_records,      // 输出结果
        const MYMQ_Public::TopicPartition& tp,                // 所属分区
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
    // bool send(...) moved to base

    Err_Client commit_inter(const MYMQ_Public::TopicPartition& tp,size_t next_offset_to_consume,MYMQ_Public::CommitAsyncResponceCallback cb);
    MYMQ_Public::ResultVariant handle_response(Eve event_type,const Mybyte& msg_body) override;
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
    size_t local_pollqueue_low_bytes;
    size_t local_pollqueue_high_bytes;
    size_t batch_size;
    MYMQ::PullSet pull_start_location;
    size_t autopush_perior_ms;
    size_t autocommit_perior_ms;
    bool is_auto_commit;
    //size_t max_in_flight_requests_num; // Moved to base
    std::atomic<size_t> pull_max_record_num_local{100000};

    std::atomic<size_t>  pull_fetch_min_bytes;

    //Config配置项


    std::string path_;
    //MYMQ::Network::Communication_client cmc_; // Moved to base


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

} // namespace Client
} // namespace MYMQ

#endif
