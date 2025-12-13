#ifndef MESSAGEQUEUE_H
#define MESSAGEQUEUE_H

#include "Server.h"
#include "MurmurHash2.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <bitset>
#include <semaphore.h>
#include <type_traits>
#include <optional>
#include <tbb/tbb.h>
#include"MYMQ_Publiccodes.h"
#include"Logsegment.h"
#include"MYMQ_innercodes.h"
#include"MYMQ_Server_ns.h"
#include"Controller.h"
#include"unordered_set"


using Record=MYMQ::MSG_serial::Record;
using HeartbeatResponce=MYMQ::HeartbeatResponce;
using Err=MYMQ_Public::CommonErrorCode;
using MB= MessageBuilder;
using MP=MessageParser;
using MesLoc=MYMQ_Server::MessageLocation;
using Mybyte=std::vector<unsigned char>;
using Eve=MYMQ::EventType;
using ConsumerGroupState=MYMQ_Server::ConsumerGroupState;
using ServerConsumerInfo=MYMQ_Server::ServerConsumerInfo;
using Byte_view_pair=std::pair<const unsigned char*, uint32_t>;
class Partition;
using TopicPartition=MYMQ_Public::TopicPartition;
using TopicPartition_to_Log_Map=tbb::concurrent_hash_map<TopicPartition,std::shared_ptr< Partition>,TbbHashCompare>;
using MemberMap=tbb::concurrent_hash_map<std::string,ServerConsumerInfo> ;

/////函数声明区


class PartitionStorage: public std::enable_shared_from_this<PartitionStorage> {
public:
    explicit PartitionStorage(const std::string& partition_data_dir)
        : partition_data_dir_(partition_data_dir) // 初始化分区数据目录

    {

        // 确保分区目录存在 (虽然 Partition 类也会创建，但这里再检查一次无害)
        std::filesystem::create_directories(partition_data_dir_);
        init();
        recover_segments();
        if (segments_.empty()) {
            create_new_segment(0);
        }
        curr_write_segment = segments_.back().get();
        uint64_t actual_max_offset_from_segments = 0;
        if (!segments_.empty()) {
            actual_max_offset_from_segments = segments_.back()->next_offset()+segments_.back()->base_offset() ;
        }
        end_offset.store(actual_max_offset_from_segments);
        //因为这个只是便于查看endoffset的一个变量
    }

    ~PartitionStorage() {
        timer_.stop();
    }

    Err save_msg(const Byte_view_pair& msg_view) {
        LogSegment* segment_to_write = nullptr;

        // 1. 尝试使用共享锁获取当前 Segment 并写入
        // 共享锁允许 Reader 和其他 Writer 同时进入这里
        {
            std::shared_lock<std::shared_mutex> read_lock(mtx_file);
            segment_to_write = curr_write_segment;

            // 注意：这里我们持有的是 Partition 的读锁，
            // 但 LogSegment::append 内部有它自己的互斥锁，所以是安全的。
            auto [offset, err] = segment_to_write->append(msg_view);

            if (err != Err::FULL_SEGMENT) {
                // 写入成功（或非 Full 错误），更新 EndOffset 并返回
                if (err == Err::NULL_ERROR) {
                    uint64_t next_val = curr_write_segment->next_offset();
                    end_offset.store(next_val, std::memory_order_release);
                }
                return err;
            }
        }
        // <--- 读锁释放

        // 2. 如果走到这里，说明 Segment 满了，需要轮转文件
        // 获取独占锁（写锁），这会阻塞 Reader 和其他 Writer
        std::unique_lock<std::shared_mutex> write_lock(mtx_file);

        // Double Check Pattern (双重检查)
        // 可能在释放读锁到获取写锁期间，别的线程已经创建了新 Segment
        if (curr_write_segment != segment_to_write) {
            // 已经被别的线程轮转过了，尝试直接写入新的 Segment
            auto [offset, err] = curr_write_segment->append(msg_view);
            if (err == Err::NULL_ERROR) {

            }
            return err; // 无论是否再次 Full，这里简单返回，或者你可以做循环重试
        }

        // 确实满了，创建新 Segment
        create_new_segment(curr_write_segment->next_offset());
        curr_write_segment = segments_.back().get();

        // 写入新 Segment
        auto pair = curr_write_segment->append(msg_view);
        if (pair.second == Err::NULL_ERROR) {
             uint64_t next_val = curr_write_segment->next_offset();
             end_offset.store(next_val, std::memory_order_release);
        }

        return pair.second;
    }


    MesLoc get_msg(size_t target_offset, size_t byte_need) {
        LogSegment* target_seg = nullptr;

        // 1. 临界区仅限于查找 Segment 指针
        {
            std::shared_lock<std::shared_mutex> lock(mtx_file);
            target_seg = find_segment(target_offset);
        }

        if (target_seg == nullptr) {
            return MesLoc{};
        }

        return target_seg->find(target_offset, byte_need);
    }



    void setup_periodic_flush() {
        std::weak_ptr<PartitionStorage> weak_self = shared_from_this();
        timer_.commit_ms(
                    [weak_self]() {
            if (auto self = weak_self.lock()) {
                std::unique_lock<std::shared_mutex> ulock(self->mtx_file);
                for(auto& seg:self->segments_){
                    seg->flush_log();
                    seg->flush_index();
                }

            }
        },
        LOG_FLUSH_INTERVAL_MS,
        LOG_FLUSH_INTERVAL_MS
        );

    }




    void clear() {
        std::unique_lock<std::shared_mutex> lock(mtx_file);
        // 删除所有段文件，路径需要根据 partition_data_dir_ 构建
        for (const auto& seg : segments_) {
            std::string base = LogSegment::compute_filename(seg->base_offset());
            std::string log_file = partition_data_dir_ + "/" + base + ".log";
            std::string index_file = partition_data_dir_ + "/" + base + ".index";
            // 确保文件存在再删除，避免错误
            if (std::filesystem::exists(log_file)) {
                std::remove(log_file.c_str());
            }
            if (std::filesystem::exists(index_file)) {
                std::remove(index_file.c_str());
            }
        }
        segments_.clear();
        // 创建新的初始段，传递完整路径
        create_new_segment(0);
        curr_write_segment = segments_.back().get();


        end_offset.store(0);

    }

    size_t get_endoffset(){
        return end_offset.load();
    }

    size_t get_first_baseoffset(){
        std::shared_lock<std::shared_mutex> lock(mtx_file);
        if(segments_.empty()){
            return static_cast<uint64_t>(-1);
        }
        return segments_.front()->base_offset();
    }

    void start_logcleaner(){
        timer_.commit_s([this]{
            //            log_compact();
        },LOG_CLEAN_S,LOG_CLEAN_S);
    }
    //暂时不实现



private:




    void recover_segments() {
        std::vector<uint64_t> base_offsets;
        // 遍历 partition_data_dir_ 查找 .log 文件
        for (const auto& entry : std::filesystem::directory_iterator(partition_data_dir_)) {
            if (entry.path().extension() == ".log") {
                std::string stem = entry.path().stem().string();
                if (stem.length() == 20 && std::all_of(stem.begin(), stem.end(), ::isdigit)) {
                    base_offsets.push_back(std::stoull(stem));
                }
            }
        }
        std::sort(base_offsets.begin(), base_offsets.end());

        segments_.reserve(base_offsets.size());
        for (auto base : base_offsets) {
            std::string log_file_path = partition_data_dir_ + "/" + LogSegment::compute_filename(base) + ".log";
            std::string index_file_path = partition_data_dir_ + "/" + LogSegment::compute_filename(base) + ".index";
            segments_.emplace_back(std::make_unique<LogSegment>(log_file_path, index_file_path, base));
        }
    }

    void create_new_segment(uint64_t base_offset) {
        std::string log_file_path = partition_data_dir_ + "/" + LogSegment::compute_filename(base_offset) + ".log";
        std::string index_file_path = partition_data_dir_ + "/" + LogSegment::compute_filename(base_offset) + ".index";
        segments_.emplace_back(std::make_unique<LogSegment>(log_file_path, index_file_path, base_offset));
    }



    LogSegment* find_segment(uint64_t offset) {

        if (segments_.empty()) return nullptr;
        auto it = std::lower_bound(
                    segments_.begin(),
                    segments_.end(),
                    offset,
                    [](const std::unique_ptr<LogSegment>& seg_ptr, uint64_t val_offset) {
            return seg_ptr->base_offset() < val_offset;
        });


        if (it == segments_.begin()) {
            if (offset < segments_.front()->base_offset()) {
                return nullptr;
            }

            return segments_.front().get();
        }


        if (it == segments_.end()) {
            auto& last_seg = segments_.back();
            if (offset < last_seg->next_offset()) {
                return last_seg.get();
            }
            return nullptr;
        }

        auto& candidate_seg = *(it - 1);
        if (offset < candidate_seg->next_offset()) {
            return candidate_seg.get();
        }

        if (offset == (*it)->base_offset()) {
            return (*it).get();
        }


        return nullptr;

    }

private:
    void init(){

        Config_manager cm_s("config/storage.properity");
        auto LOG_FLUSH_INTERVAL_MS_tmp=cm_s.get_size_t("LOG_FLUSH_INTERVAL_MS");
        if(!inrange(LOG_FLUSH_INTERVAL_MS_tmp,60000,144000000)){
            LOG_FLUSH_INTERVAL_MS_tmp=MYMQ::LOG_FLUSH_INTERVAL_MS;
        }
        LOG_FLUSH_INTERVAL_MS=LOG_FLUSH_INTERVAL_MS_tmp;
        LOG_CLEAN_S=MYMQ::LOG_CLEAN_S_DEFAULT;

    }

    bool inrange(size_t obj,size_t min,size_t max){
        return (obj<=max&&obj>=min);
    }

private:
    std::string log_filename_;
    std::vector<std::unique_ptr<LogSegment>> segments_;
    LogSegment* curr_write_segment{nullptr};
    std::shared_mutex mtx_file;
    std::atomic<size_t> end_offset;
    Timer timer_;
    size_t LOG_FLUSH_INTERVAL_MS;
    int LOG_CLEAN_S;
    std::string partition_data_dir_;
};



class Partition {
public:
    Partition(const std::string& data_root_dir, const std::string& topicname, size_t parti_id,bool is_belong_consumer_offset=0)
        :   owner_topic_(topicname) {
        // 构建分区的数据目录路径：data_root_dir/topicname/ParX
        partition_data_dir_ = data_root_dir + "/" + topicname + "/Par" + std::to_string(parti_id);
        // 确保目录存在
        std::filesystem::create_directories(partition_data_dir_);
        // 将分区数据目录传递给 MessageStorage
        msg_stor = std::make_shared<PartitionStorage>(partition_data_dir_);
        msg_stor->setup_periodic_flush();
        if(is_belong_consumer_offset){
            msg_stor-> start_logcleaner();
        }
    }

    Err push(const Byte_view_pair& msg_view) {
        return  msg_stor->save_msg(msg_view);

    }

    MesLoc pull(size_t target_offset,size_t byte_need) const{

        return msg_stor->get_msg(target_offset,byte_need);
    }


    void clear() {
        msg_stor->clear();
    }


    size_t getEarliestOffset()  {
        return msg_stor->get_first_baseoffset();
    }


    size_t get_endoffset() {
        return msg_stor->get_endoffset();
    }

    void get_latest_committed_offset(std::unordered_map<std::string, Record>& map){
        //            map= msg_stor->log_compact();
    }
private:
    std::string file_name_;
    std::string owner_topic_;
    std::string partition_data_dir_;
    std::shared_ptr<PartitionStorage> msg_stor;
};


class Topic{
public:
    explicit  Topic(const std::string& topicname, const std::string& data_root_dir, int parti_num=1,bool is_belong_consumer_offset=0)
        : topicname_(topicname), num_partitions_(parti_num), data_root_dir_(data_root_dir) {
        // 确保 topic 目录存在
        std::filesystem::create_directories(data_root_dir_ + "/" + topicname_);
        partitions_.reserve(num_partitions_);
        for(int i=0; i < num_partitions_; ++i){
            // 将 data_root_dir 和 topicname 传递给 Partition
            partitions_.emplace_back(std::make_unique<Partition>(data_root_dir_, topicname_, i,is_belong_consumer_offset));
        }
    }

    std::string get_topicname(){
        return topicname_;
    }


    size_t get_parti_num() const {
        return partitions_.size();
    }


    Err push(const Byte_view_pair& msg_view, int partition_idx) {
        return  partitions_.at(partition_idx)->push(msg_view);
    }

    MesLoc pull(size_t target_offset, int partition_idx,size_t byte_need) {


        try {
            auto res= partitions_.at(partition_idx)->pull(target_offset,byte_need);
            return res;

        } catch (const std::out_of_range& e) {
            cerr("Invalid partition ID.");
        }

        return MesLoc{};

    }



    void clear_partition(int partition_id) {
        partitions_.at(partition_id)->clear();
        //throw std::out_of_range("Invalid partition ID for clear_partition.");

    }

    // 获取指定分区中最早可用的消息偏移量
    size_t getEarliestOffset(int partition_id) const {
        return partitions_.at(partition_id)->getEarliestOffset();
    }

    // 获取指定分区中最新可用的消息偏移量 (即下一个消息的写入位置)
    size_t get_endoffset(int partition_id) const {
        return  partitions_.at(partition_id)->get_endoffset();

    }

    void get_latest_committed_offset(int partition_id,std::unordered_map<std::string, Record>& map) const {
        partitions_.at(partition_id)->get_latest_committed_offset(map);
    }

    std::vector<std::unique_ptr<Partition>>& get_partition_ref(){
        return partitions_;
    }


private:
    std::string topicname_;
    std::vector<std::unique_ptr<Partition>> partitions_;
    int num_partitions_;
    std::string data_root_dir_;
};


class ConsumerOffset{
public:
    ConsumerOffset(Topic& consumer_offset_topic_ref)
        : consumer_offset(consumer_offset_topic_ref){
    }



    Err commit_sync(uint32_t consumeroffset_parid_hash,const std::string& key,size_t offset_digit){
        auto parti_num= consumer_offset.get_parti_num();
        if(parti_num==0){
            return Err::TOPIC_EMPTY;
        }
        size_t par_id=consumeroffset_parid_hash%parti_num;
        return commit_offset(key,offset_digit);
    }



    //    void load_latest_offset_to_coordinator(){
    //        auto num=consumer_offset.get_parti_num();
    //        std::vector<std::unordered_map<Mybyte, MessageConstruct>> map_msgs;
    //        map_msgs.resize(num);
    //        int i=0;
    //        for(auto &par:consumer_offset.get_partition_ref()){
    //            par->get_latest_committed_offset(map_msgs.at(i));
    //        }
    //        std::queue<std::pair<Mybyte, MessageConstruct>> tmp_queue{};
    //        for(int i=0;i<num;i++){
    //            for(const auto& pair:map_msgs[i]){
    //                tmp_queue.emplace(pair);
    //            }
    //        }
    //        std::unordered_map<Mybyte,size_t> map_latest_offset_tmp;
    //        map_latest_offset_tmp.reserve(tmp_queue.size());
    //        while(!tmp_queue.empty()){
    //            auto [key,val]=tmp_queue.front();
    //                    map_latest_offset_tmp[key]=std::stoull(val.value);
    //        }
    //                    map_latest_offset=std::move(map_latest_offset_tmp);

    //        }

private:


    Err commit_offset(const std::string& key,size_t offset_digit){
        tbb::concurrent_hash_map<std::string, size_t>::accessor acc;
        map_latest_offset.insert(acc,key);
        acc->second=offset_digit;


        return Err::NULL_ERROR;
    }

    Err get_latest_offset(const std::string& key,size_t& off,size_t par_id){
        tbb::concurrent_hash_map<std::string, size_t>::const_accessor ca;
        if(map_latest_offset.find(ca,key)){
            off=ca->second;
            return Err::NULL_ERROR;
        }

        tbb::concurrent_hash_map<size_t,std::queue<Mybyte>> ::accessor a;

        if(map_offset_queue.find(a,par_id)){
//            a->second.emplace(MYMQ::MSG_serial::build_Record(key,std::to_string(off)));
        }
        else{
            return Err::INTERNAL_ERROR;
        }

        return Err::UNKNOWN_OFFSET_KEY;
    }


private:
    Topic& consumer_offset;
    Timer timer;
    tbb::concurrent_hash_map<std::string,size_t> map_latest_offset;
    tbb::concurrent_hash_map<size_t,std::queue<Mybyte>> map_offset_queue;

};




class GroupCoordinator :public std::enable_shared_from_this<GroupCoordinator>{

public:

    GroupCoordinator(const std::shared_ptr<ConsumerOffset>& consumer_offset_manager,std::shared_ptr<MetadataCache> metadata_ptr)
        : consumer_offset_manager_(consumer_offset_manager),cache_metadata_ptr(metadata_ptr) {


    }


    void initialize() {
        if(!init_ed){
            startLivenessCheck();
            init_ed=1;
        }

    }


     Err leave_group(const std::string& group_id,const std::string& memberid){

         std::shared_lock<std::shared_mutex> global_lock(group_states_mutex_);
         auto it = group_states_.find(group_id);
         if (it == group_states_.end()) {
             global_lock.unlock();
             throw (Err::GROUP_NOT_FOUND);

         }
         auto  group_state_ptr = it->second;

         global_lock.unlock();

       auto res=  group_state_ptr->handle_leave(memberid,*cache_metadata_ptr);

       if(!res){
           return Err::MEMBER_NOT_FOUND;
       }
       return Err::NULL_ERROR;
    }

    // 3. Heartbeat: 消费者发送心跳
    HeartbeatResponce heartbeat(const std::string& group_id, const std::string& member_id,size_t gen_id) {
        HeartbeatResponce resp;
        std::shared_lock<std::shared_mutex> global_lock(group_states_mutex_);
        auto it = group_states_.find(group_id);
        if (it == group_states_.end()) {
            global_lock.unlock();
             resp.errorcode=Err::GROUP_NOT_FOUND;
            return resp;

        }
        auto  group_state_ptr = it->second;

        global_lock.unlock();

       auto res= group_state_ptr->handle_heartbeat(member_id,gen_id);
       if(res.first==0){
           resp.errorcode=Err::MEMBER_NOT_FOUND;
       }
       else if(res.first!=gen_id){
           resp.assign=std::move(res.second);
           resp.errorcode=Err::UPDATE_GENERATION;
       }
       resp.generation_id=res.first;
       return resp;



    }

    HeartbeatResponce update_subscription(const std::string& group_id, std::string& member_id, size_t gen_id, const std::set<std::string>& client_full_list) {
        HeartbeatResponce resp;


        std::unique_lock<std::shared_mutex> global_lock(group_states_mutex_);

        auto it = group_states_.find(group_id);
        if (it == group_states_.end()) {
            if (gen_id == 0) {
                // 自动创建
                auto new_group = std::make_shared<ConsumerGroupState>(group_id);
                group_states_.emplace(group_id, new_group);
                it = group_states_.find(group_id);
            } else {
                // 找不到 Group 且不是新建请求
                resp.errorcode=Err::GROUP_NOT_FOUND;
                return resp;
            }
        }
        auto group_state_ptr = it->second;

        // 拿到 shared_ptr 后可以尽早释放全局锁，减小锁粒度
        global_lock.unlock();

        if (member_id.empty()) {
            member_id = uuid_gen_str();
        }

        // 确保 cache_metadata_ptr 已经初始化！
        if (!cache_metadata_ptr) {
             // Log Error: Cache not initialized
             return resp; // 或者抛异常
        }

        auto res = group_state_ptr->update_subscription(member_id, client_full_list, *cache_metadata_ptr);

        if (res.first != gen_id) {
            resp.assign = std::move(res.second);
            resp.errorcode = Err::UPDATE_GENERATION;
        }
        resp.generation_id = res.first;
        return resp;
    }

private:


    std::string uuid_gen_str(){
        uuid_t my_uuid_bytes;
        uuid_generate(my_uuid_bytes);
        char uuid_str[UUID_STR_LEN];
        uuid_unparse(my_uuid_bytes, uuid_str);
        std::string unique_id_string = uuid_str;
        return unique_id_string;
    }

    void startLivenessCheck() {
        std::weak_ptr<GroupCoordinator> weak_self = shared_from_this();
        timer.commit_ms(
                    [weak_self]() {
            if (auto self = weak_self.lock()) {
                self->checkLiveness();
            }
        },
        session_timeout_ms_,
        session_timeout_ms_
        );
        cerr("Groupcoordinator : Heartbeat check start .");
    }


    void checkLiveness() {
    }



private:

    std::shared_ptr<ConsumerOffset> consumer_offset_manager_;

    std::map<std::string,std::shared_ptr<ConsumerGroupState> > group_states_; // group_id -> ConsumerGroupState
    std::shared_mutex group_states_mutex_; // 保护 group_states_

    Timer timer;


    int session_timeout_ms_ =MYMQ::session_timeout_ms_;
    int join_collect_timeout_ms_ = MYMQ::join_collect_timeout_ms; // 重平衡join窗口期
    int rebalance_timeout_ms=MYMQ::rebalance_timeout_ms; // 重平衡超时时长
    bool init_ed{0};

   std::shared_ptr<MetadataCache>  cache_metadata_ptr;

};


class MessageQueue : public std::enable_shared_from_this<MessageQueue> {

public:
    explicit MessageQueue(const std::string& data_root_dir = "./data/")
        : data_root_dir_(data_root_dir),cache_metadata(nullptr)

    {
        topics_metadata_filename_ = data_root_dir_ + "/topics_metadata.conf";
        std::filesystem::create_directories(data_root_dir_);
        init();
        load_topics_metadata(); // 这一步会尝试从文件加载并创建 consumer_offset_topic_ptr_

        // 确保 consumer_offset_topic_ptr_ 已经初始化
        if (!consumer_offset_topic_ptr_) {
            // 如果元数据文件不存在，或者文件中没有 MYMQ::consumeroffset_name 的条目，则在此处创建
            std::cerr << "Warning: Special topic '" << MYMQ::consumeroffset_name << "' not found in metadata. Creating with default partitions (10)." << std::endl;
            consumer_offset_topic_ptr_ = std::make_unique<Topic>(MYMQ::consumeroffset_name, data_root_dir_, MYMQ::PARTITION_NUM_OF_CONSUMER_OFFSET_TOPIC);
            // 由于是新创建的，需要立即保存到元数据文件
            save_topics_metadata();
        }

        // 现在 consumer_offset_topic_ptr_ 保证是有效的，可以用来构造 ConsumerOffset 管理器
        consumer_offset_manager_ptr_ = std::make_shared<ConsumerOffset>(*consumer_offset_topic_ptr_);

        start_groupcoordinator(); // 此时 consumer_offset_manager_ptr_ 已经就绪
        start_server();
    }
    ~MessageQueue(){
        server_.stop();
        if (server_thread_.joinable()) {
            server_thread_.join();
            out("Server thread joined successfully." ) ;
        }


        save_topics_metadata();
        ThreadPool::instance().stop();

    }
    void init(){
        Config_manager cm_sys("config/system.properity");
        auto core_num=cm_sys.getint("max_threadnum");
        ThreadPool::instance(core_num).start();
        Config_manager cm_communication("config/communication.propertity");

        std::string server_IP;
        size_t PORT;
        try {
           server_IP= cm_communication.getstring("IP");
        } catch (std::exception& e) {
            throw ("IP not found");
        }
        try {
            PORT= cm_communication.get_size_t("port");
        } catch (std::exception& e) {
            throw ("port not found");
        }

        cache_metadata=std::make_shared<MetadataCache>(server_IP,PORT);
    }


    size_t get_partition_endoffset(const std::string& topicname,size_t partition ){

        TopicPartition_to_Log_Map::const_accessor cac;
        if(! map_tp_to_log .find(cac,TopicPartition(topicname,partition))){
           return SIZE_MAX;
        }
        auto partition_ptr=cac->second;
        cac.release();

        return  partition_ptr->get_endoffset();
    }
    void load_topics_metadata() {
        std::ifstream ifs(topics_metadata_filename_);
        if (!ifs.is_open()) {
            std::cerr << "Warning: Topic metadata file not found..." << std::endl;
            return;
        }

        std::string line;
        while (std::getline(ifs, line)) {
            std::istringstream iss(line);
            std::string topicname;
            size_t parti_num;
            if (!(iss >> topicname >> parti_num)) continue;

            // 1. 同步到 MetadataCache (这是新增的逻辑)
            // 告诉元数据缓存：我有这个 Topic，我有这么多分区，Leader 都是我自己
            cache_metadata->createTopic(topicname, parti_num);

            // 2. 同步到 TopicMap (保持原逻辑)
            if (topicname == MYMQ::consumeroffset_name) {
                if (!consumer_offset_topic_ptr_) {
                    consumer_offset_topic_ptr_ = std::make_unique<Topic>(topicname, data_root_dir_, parti_num,1);
                }
            } else {
                for(size_t i=0;i<parti_num;i++){
                    TopicPartition_to_Log_Map::accessor ac;
                    map_tp_to_log.insert(ac,TopicPartition(topicname,i));
                    ac->second=std::make_shared<Partition>( data_root_dir_, topicname,parti_num);

                }

            }
        }
    }


    void save_topics_metadata() {
        std::string temp_filename = topics_metadata_filename_ + ".tmp";
        std::ofstream ofs_tmp(temp_filename);
        if (!ofs_tmp.is_open()) {
            throw std::runtime_error("Failed to create temporary topic metadata file: " + temp_filename + " - " + std::strerror(errno));
        }

        // 首先保存特殊的消费者偏移量 Topic 的元数据，如果它存在的话
        if (consumer_offset_topic_ptr_) {
            ofs_tmp << consumer_offset_topic_ptr_->get_topicname() << " " << consumer_offset_topic_ptr_->get_parti_num() << std::endl;
        }

        std::unordered_map<std::string,size_t> tmp_metadata_map;
        {
            for(const auto& [tp,parti]:map_tp_to_log){
               tmp_metadata_map[tp.topic]++;
            }

        }
        for(const auto& [t,p_num]:tmp_metadata_map){
            ofs_tmp << t << " " << p_num << std::endl;
        }


        ofs_tmp.close();

        if (std::rename(temp_filename.c_str(), topics_metadata_filename_.c_str()) != 0) {
            std::remove(temp_filename.c_str());
            throw std::runtime_error("Failed to rename temporary topic metadata file to " + topics_metadata_filename_ + ": " + std::strerror(errno));
        }
    }

    Err push(const Byte_view_pair& msg_view ,const std::string& topicname,size_t partition) {
        TopicPartition_to_Log_Map::const_accessor cac;
        if (!map_tp_to_log.find(cac,TopicPartition(topicname,partition))) {
            return Err::UNKNOWN_TOPICPARTITION;
        }
        auto partition_ptr=cac->second;
        cac.release();
       return partition_ptr->push(msg_view);
    }

    std::pair<MesLoc,Err>  pull(size_t target_offset,const std::string& topicname, size_t partition_id,size_t byte_need) {

        TopicPartition_to_Log_Map::const_accessor cac;
        if(!map_tp_to_log.find(cac,TopicPartition(topicname,partition_id))){
           return {MesLoc{},Err::UNKNOWN_TOPICPARTITION};;
        }
        auto partition_ptr = cac->second;
        cac.release();
        auto locinf= partition_ptr->pull(target_offset,byte_need);
        if(!locinf.found){
            return {MesLoc{},Err::NO_RECORD};
        }

        return {locinf,Err::NULL_ERROR};

    }

    bool create_topic(const std::string& topicname,size_t parti_num =1){
        if(topicname.find_first_of(" \n")!=std::string::npos&&topicname!=MYMQ::consumeroffset_name){
            std::cerr << "Error: Topic name contains invalid characters (space or newline)." << std::endl;
            return false;
        }
        // 阻止通过 create_topic 创建特殊 Topic
        if (topicname == MYMQ::consumeroffset_name) {
            out("CREATE TOPIC: Attempted to create special topic '" + topicname + "' via create_topic. This topic is managed internally.");
            return false;
        }

        auto res= cache_metadata->createTopic(topicname,parti_num);
        if(!res){
             out("CREATE TOPIC: Topic '"+ topicname + "' already exists." );
             return 0;
        }

        for(size_t i=0;i<parti_num;i++){
            TopicPartition_to_Log_Map::accessor ac;
            map_tp_to_log.insert(ac,TopicPartition(topicname,i));
            ac->second= std::make_shared<Partition>(data_root_dir_,topicname,i);

        }


        save_topics_metadata(); // 保存新的 Topic 元数据
        return true;
    }


    Err commit_sync(const std::string& topicname, size_t partition ,uint32_t consumeroffset_parid_hash,const std::string& key,uint32_t offset_digit) {
        if(cache_metadata->getPartitionLeader(topicname,partition)=std::nullopt){
            return Err::TOPIC_NOT_FOUND;
        }
        if (!consumer_offset_manager_ptr_) {
            return Err::INTERNAL_ERROR; // 或者更具体的错误类型
        }
        return consumer_offset_manager_ptr_->commit_sync(consumeroffset_parid_hash,key,offset_digit);
    }


    HeartbeatResponce heartbeat(const std::string& group_id, const std::string& member_id,size_t gen_id) {
        return groupcoordinator_->heartbeat(group_id, member_id,gen_id);
    }


    HeartbeatResponce update_subscription(const std::string& group_id, std::string& member_id, size_t gen_id,const std::set<std::string>& client_full_list){
       return groupcoordinator_->update_subscription(group_id,member_id,gen_id,client_full_list);
    }


private:




    void start_server(){

        server_.set_client_message_callback(
                    [this](TcpSession session, uint16_t event_type_short,uint32_t correlation_id,uint16_t ack_level ,Mybyte msg_body) {
            MYMQ::EventType type = static_cast<MYMQ::EventType>(event_type_short);

            cerr("["+std::to_string(correlation_id)+"]["+session.get_clientid()+"]"+ MYMQ::to_string(static_cast<Eve>(event_type_short))+" called.");
            MessageParser mp(msg_body.data(),msg_body.size());
            mp.skip(4);
            if(type==MYMQ::EventType::CLIENT_REQUEST_PULL){

                auto groupid=mp.read_string();
                auto topicname=mp.read_string();
                auto partition=mp.read_size_t();
                auto offset=mp.read_size_t();
                auto bytes_need=mp.read_size_t();
                auto res= pull(offset,topicname,partition,bytes_need);
                bool failed=1;
                if(res.second==Err::NULL_ERROR){
                    SendFileTask file_resp(res.first, topicname, partition,correlation_id,ack_level);
                    session.send(Eve::SERVER_RESPONSE_PULL_DATA , correlation_id, ack_level, std::move(file_resp));

                    failed=0;

                }
                if (failed) {
                    cerr(MYMQ_Public::to_string(static_cast<Err>(res.second)));

                    // 1. 构建 Metadata (必须与 SendFileTask 里的结构完全一致！)
                    // SendFileTask: Topic -> Partition -> Error -> Offset
                    MessageBuilder mb_meta;
                    mb_meta.append_string(topicname);
                    mb_meta.append_size_t(partition);
                    mb_meta.append_uint16(static_cast<uint16_t>(res.second));
                    mb_meta.append_size_t(offset);

                    // 2. 将 Metadata 包装进 Body，并追加一个空的 Payload

                    MessageBuilder mb_body;

                    // 第一层：Metadata Vector
                    mb_body.append_uchar_vector(mb_meta.data);

                    // 第二层：空的 Payload Vector (长度0)
                    std::vector<unsigned char> empty_payload;
                    mb_body.append_uchar_vector(empty_payload);

                    // 3. 发送 (session.send 会给整个 mb_body 再加一层长度头，作为最外层的 Body)
                    session.send(Eve::SERVER_RESPONSE_PULL_DATA, correlation_id, ack_level, std::move(mb_body.data));

                    cerr(std::to_string(offset));
                }





            }
            else if(type==MYMQ::EventType::CLIENT_REQUEST_PUSH){



                auto topicname= mp.read_string();
                auto partition=mp.read_size_t();

                auto crc= mp.read_uint32();
                auto msg_view=mp.read_bytes_view();


                MB mb_res;
                mb_res.reserve(sizeof (uint32_t)+topicname.size()+sizeof(size_t)+sizeof (uint16_t));
                mb_res.append(topicname,partition);


                if(!MYMQ::Crc32::verify_crc32(msg_view.first,msg_view.second,crc)){
                    cerr("Push CRC verify : Not match , refused to push");
                    if(ack_level!=static_cast<uint16_t>(MYMQ::ACK_Level::ACK_NORESPONCE)){
                            mb_res.append_uint16(static_cast<uint16_t>(Err::CRC_VERIFY_FAILED));
                            session.send(Eve::SERVER_RESPONSE_PUSH_ACK,correlation_id,ack_level,mb_res.data);
                    }
                      return ;
                }


                auto push_res= push(msg_view,topicname,partition);
                uint64_t baseoffset;
                std::memcpy(&baseoffset,msg_view.first,sizeof(uint64_t));
                baseoffset=ntohll(baseoffset);

                if(ack_level==static_cast<uint16_t>(MYMQ::ACK_Level::ACK_PROMISE_INDISK)){
                    mb_res.append_uint16(static_cast<uint16_t>(push_res));
                    mb_res.append_uint64(baseoffset);
                    session.send(Eve::SERVER_RESPONSE_PUSH_ACK,correlation_id,ack_level,mb_res.data);
                }
                 cerr("Push result : "+MYMQ_Public::to_string(push_res));
            }
            else if(type==MYMQ::EventType::CLIENT_REQUEST_COMMIT_OFFSET){

                auto groupid=mp.read_string();
                auto topicname=mp.read_string();
                auto partition=mp.read_size_t();
                auto consumeroffset_parid_hash=mp.read_uint32();
                auto key_gtp=mp.read_string();
                auto offset_digit=mp.read_size_t();

                auto error= commit_sync(topicname,partition,consumeroffset_parid_hash,key_gtp,offset_digit);
                MB mb;
                mb.reserve(sizeof (uint32_t)*2+groupid.size()+topicname.size()+sizeof (size_t)*2+sizeof (uint16_t));
                mb.append(groupid,topicname,partition,static_cast<uint16_t>(error),offset_digit);
                session.send(Eve::SERVER_RESPONCE_COMMIT_OFFSET,correlation_id,ack_level,mb.data);



            }
            else if(type==MYMQ::EventType::CLIENT_REQUEST_REGISTER){
                MB mb;
                mb.append_bool(mp.read_bool());
                session.send(Eve::SERVER_RESPONSE_REGISTER,correlation_id,ack_level,mb.data);
            }
           else if(type==MYMQ::EventType::CLIENT_REQUEST_LEAVE_GROUP){
                auto groupid=mp.read_string();
                auto memberid=mp.read_string();
                auto res=  leave_group(groupid,memberid);
                MB mb;
                mb.append(static_cast<uint16_t>(res),groupid);
               session.send(Eve::SERVER_RESPONCE_LEAVE_GROUP,correlation_id,ack_level,mb.data);
            }
             else if(type==MYMQ::EventType::CLIENT_REQUEST_HEARTBEAT){

                auto groupid=mp.read_string();               
                auto memberid=mp.read_string();
                auto generationid=mp.read_int();
                bool is_join_group=memberid.empty();
                std::string clientid;
                if(is_join_group){
                    clientid=mp.read_string();
                }
                auto need_update_topics=mp.read_bool();
                std::set<std::string> topics;


                if(need_update_topics){
                    auto topic_add_num=mp.read_size_t();
                    for(size_t i=0;i<topic_add_num;i++){
                        topics.emplace(mp.read_string());
                    }
                }


                HeartbeatResponce res;
                if(need_update_topics){
                   res= update_subscription(groupid,memberid,generationid,topics);
                }
                else{
                   res= heartbeat(groupid,memberid,generationid);
                }

                MB mb;
                mb.append(static_cast<uint16_t>(res.errorcode) ,groupid,memberid,res.generation_id);
                if(res.errorcode==Err::UPDATE_GENERATION){
                    mb.append_size_t(res.assign.size());
                    for(const auto& [topic,partitions]:res.assign ){
                        mb.append(topic);
                        mb.append_size_t(partitions.size());
                        for(const auto& par:partitions){
                            mb.append_size_t(par);
                        }

                    }

                }
                session.send(Eve::SERVER_RESPONCE_HEARTBEAT,correlation_id,ack_level,mb.data);




            }
            else if(type==Eve::CLIENT_REQUEST_CREATE_TOPIC){
                auto topicname=mp.read_string();
                auto num=mp.read_size_t();
                auto res= create_topic(topicname,num);
                MB mb;
                mb.append(res);
                session.send(Eve::SERVER_RESPONSE_CREATE_TOPIC,correlation_id,ack_level,mb.data);

            }

        }
        );



        server_thread_ = server_.start_in_thread();
        out("Server has started and is listening for connections." );
    }



    void start_groupcoordinator(){
        // 在使用之前检查 consumer_offset_manager_ptr_ 是否有效
        if (!consumer_offset_manager_ptr_) {
            throw std::runtime_error("ConsumerOffset manager not initialized before starting GroupCoordinator.");
        }
        groupcoordinator_ = std::make_shared<GroupCoordinator>(consumer_offset_manager_ptr_, cache_metadata);

        timer_.commit_ms([this]{
            start_group_checkliveness();
        },
        0,3000,5
        );
    }


    void start_group_checkliveness(){
        if(groupcoordinator_){
            groupcoordinator_->initialize();
        }
    }

    Err leave_group(const std::string& groupid,const std::string& memberid){
        return groupcoordinator_->leave_group(groupid,memberid);
    }

    std::map<std::string, std::map<std::string, std::set<size_t>>> parse_assignments_message(const Mybyte& serialized_data) {
        MessageParser parser(serialized_data.data(),serialized_data.size());
        std::map<std::string, std::map<std::string, std::set<size_t>>> assignments;

        // 1. 读取成员总数
        size_t num_members = parser.read_size_t();

        for (int i = 0; i < num_members; ++i) {
            // 2. 读取 member_id
            std::string member_id = parser.read_string();

            std::map<std::string, std::set<size_t>> topic_assignments_for_member;
            // 3. 读取当前 member 的 topic 数量
            size_t num_topics = parser.read_size_t();

            for (int j = 0; j < num_topics; ++j) {
                // 4. 读取 topic_name
                std::string topic_name = parser.read_string();

                std::set<size_t> partitions_for_topic;
                // 5. 读取当前 topic 该member分管的的 partition 数量
                size_t num_partitions = parser.read_size_t();

                for (int k = 0; k < num_partitions; ++k) {
                    // 6. 读取 partition_id
                    size_t partition_id = parser.read_size_t();
                    partitions_for_topic.insert(partition_id);
                }
                topic_assignments_for_member[topic_name] = partitions_for_topic;
            }
            assignments[member_id] = topic_assignments_for_member;
        }
        return assignments;
    }



private:
    std::unique_ptr<Topic> consumer_offset_topic_ptr_;
    std::shared_ptr<ConsumerOffset> consumer_offset_manager_ptr_;
    std::string data_root_dir_;
    std::string topics_metadata_filename_;
    TopicPartition_to_Log_Map map_tp_to_log;
    std::shared_ptr< MetadataCache > cache_metadata=nullptr;


    std::shared_ptr<GroupCoordinator> groupcoordinator_=nullptr;


    Server server_;
    Timer timer_;
    std::thread server_thread_;


};

#endif // MESSAGEQUEUE_H

