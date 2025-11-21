#ifndef MESSAGEQUEUE_H
#define MESSAGEQUEUE_H

#include "Server.h"
#include "MurmurHash2.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <bitset>
#include <semaphore.h>
#include <type_traits>
#include "uuid/uuid.h"
#include <optional>
#include <tbb/tbb.h>
#include"MYMQ_Publiccodes.h"
#include"Logsegment.h"


using Record=MYMQ::MSG_serial::Record;
using ConsumerInfo=MYMQ::ConsumerInfo;
using ConsumerGroupState=MYMQ::MYMQ_Server::ConsumerGroupState;
using HeartbeatResponce=MYMQ::HeartbeatResponce;
using Err=MYMQ_Public::CommonErrorCode;
using Gstate=MYMQ::MYMQ_Server::ConsumerGroupState::GroupState;
using MB= MessageBuilder;
using MP=MessageParser;
using MesLoc=MYMQ::MYMQ_Server::MessageLocation;
using Mybyte=std::vector<unsigned char>;
using Eve=MYMQ::EventType;

class Topic;
class Mmapfile;

struct Topicmap{
    std::unordered_map<std::string,std::unique_ptr<Topic>> topics_;
    std::shared_mutex mtx_topic;
};


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

    Err save_msg(std::vector<unsigned char>& msg) {
        std::unique_lock<std::shared_mutex> lock(mtx_file);
        auto [offset,err] = curr_write_segment->append(msg);

                if (err==Err::FULL_SEGMENT) {
            create_new_segment(curr_write_segment->next_offset());
            curr_write_segment = segments_.back().get();
            auto  pair = curr_write_segment->append(msg);
            if (pair.second != Err::NULL_ERROR) {
                throw std::runtime_error("Failed to append message even after creating new segment.");
            }
            offset=pair.first;
        }

        end_offset.store(offset+1);
        return Err::NULL_ERROR;
    }

    MesLoc get_msg(size_t target_offset,size_t byte_need) {
        std::shared_lock<std::shared_mutex> lock(mtx_file);

        LogSegment* target_seg= find_segment(target_offset);
        if (target_seg==nullptr) {
            return MesLoc{};
        }

        return target_seg->find(target_offset,byte_need);

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
    Partition(const std::string& data_root_dir, const std::string& topicname, int parti_id,bool is_belong_consumer_offset=0)
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

    Err push(std::vector<unsigned char>& msg) {
        return  msg_stor->save_msg(msg);

    }

    MesLoc pull(size_t target_offset,size_t byte_need){

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


    Err push(std::vector<unsigned char>& msg, int partition_idx) {
        return  partitions_.at(partition_idx)->push(msg);
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
            a->second.emplace(MYMQ::MSG_serial::build_Record(key,std::to_string(off)));
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

    GroupCoordinator(const std::shared_ptr<ConsumerOffset>& consumer_offset_manager,Topicmap& topics_)
        : consumer_offset_manager_(consumer_offset_manager),topics_map(topics_) {


    }

    bool is_leader(const std::string& groupid,const std::string& memberid){
        std::shared_lock<std::shared_mutex> global_lock(group_states_mutex_);
        auto it=group_states_.find(groupid);
        if(it==group_states_.end()){
            return 0;
        }
        ConsumerGroupState& group_state = it->second;
        std::unique_lock<std::mutex> group_lock(group_state.state_mutex);
        global_lock.unlock();
        if(group_state.leader_id!=memberid){
            return 0;
        }

        return 1;
    }

    using Groupcoordinator_cb=std::function<void(int sock,uint32_t correlation_id,Eve event_type, const Mybyte msg)>;
    void set_callback(const Groupcoordinator_cb& cb){
        std::unique_lock<std::shared_mutex> ulock(mtx_callback);
        callback_=cb;
    }


    Err send_notice(const std::string& groupid,uint32_t correlation_id,const std::string& memberid,Eve event_type,const Mybyte& msg_){
        std::shared_lock<std::shared_mutex> global_lock(group_states_mutex_);
        auto it=group_states_.find(groupid);
        if(it==group_states_.end()){
            return Err::GROUP_NOT_FOUND;
        }
        ConsumerGroupState& group_state = it->second;
        std::unique_lock<std::mutex> group_lock(group_state.state_mutex);
        global_lock.unlock();

        int client_fd=-1;
        auto inf_pair= group_state.members.find(memberid);
        if(inf_pair!=group_state.members.end()){
            client_fd=  inf_pair->second.userinfo.sock;
        }
        else{
            return Err::MEMBER_NOT_FOUND;
        }

        group_lock.unlock();



        Groupcoordinator_cb curr_cb;
        {
            std::shared_lock<std::shared_mutex> slock(mtx_callback);
            curr_cb=callback_;
        }
        if(curr_cb){
            ThreadPool::instance().commit([curr_cb,client_fd,event_type,msg_,correlation_id]{
                curr_cb(client_fd,correlation_id,event_type,std::move(msg_) );
            });
        }

  return Err::NULL_ERROR;

    }
    void send_notice_internal(int client_fd,uint32_t correlation_id ,Eve event_type,const Mybyte& msg_){
        Groupcoordinator_cb curr_cb;
        {
            std::shared_lock<std::shared_mutex> slock(mtx_callback);
            curr_cb=callback_;
        }
        if(curr_cb){
            ThreadPool::instance().commit([curr_cb,client_fd,event_type,msg_,correlation_id]{
                curr_cb(client_fd,correlation_id,event_type,std::move(msg_) );
            });


        }
    }

    void initialize() {
        if(!init_ed){
            startLivenessCheck();
            init_ed=1;
        }

    }
    std::pair< ConsumerGroupState::GroupState,bool> get_SpecificState(const std::string& groupid){
        std::shared_lock<std::shared_mutex> global_lock(group_states_mutex_);
        auto it=group_states_.find(groupid);
        if(it==group_states_.end()){
            return {ConsumerGroupState::GroupState::EMPTY,0};
        }
        ConsumerGroupState& group_state = it->second;
        std::unique_lock<std::mutex> group_lock(group_state.state_mutex);
        global_lock.unlock();
        return {group_state.state,1};
    }


    // 1. JoinGroup: 消费者加入组
    std::pair<int,Err>  joinGroup(const std::string& group_id, ConsumerInfo& consumer_info) {
        std::pair<int,Err> res{-1,Err::NULL_ERROR};
        std::string member_id = consumer_info.memberid.empty() ? uuid_gen_str() : consumer_info.memberid;
        consumer_info.memberid=std::move(member_id) ;

        std::unique_lock<std::shared_mutex> global_lock(group_states_mutex_);
        auto [it, is_new_group] = group_states_.emplace(std::piecewise_construct,
                std::forward_as_tuple(group_id),
                std::forward_as_tuple(group_id));
                ConsumerGroupState& group_state = it->second;
                std::unique_lock<std::mutex> group_lock(group_state.state_mutex);
                global_lock.unlock();

                res.first=group_state.generation_id;
                if(group_state.state==Gstate::AWAITING_SYNC){
            group_lock.unlock();
            res.second=Err::REBALANCE_IN_PROGRESS;

            return res;
        }
        else{
            group_lock.unlock();
            ConsumerInfo captured_consumer_info = consumer_info;
            triggerRebalance(group_id, captured_consumer_info);
            return res;
        }
    }


    // 2. SyncGroup: 消费者获取其分配的分区 (或领导者提交分配)
    // 返回 member_id -> topic -> partitions 的映射
    std::pair<std::map<std::string, std::set<size_t>>,Err>  syncGroup(
            const std::string& group_id,
            const std::string& member_id,
            int generation_id,
            const std::map<std::string, std::map<std::string, std::set<size_t>>>& leader_assignments) {

        std::pair<std::map<std::string, std::set<size_t>>,Err> res{std::map<std::string, std::set<size_t>>(),Err::NULL_ERROR};

        std::shared_lock<std::shared_mutex> global_lock(group_states_mutex_);
        auto it = group_states_.find(group_id);
        if (it == group_states_.end()) {
            global_lock.unlock();
            res.second=Err::GROUP_NOT_FOUND;

            return res;
        }
        ConsumerGroupState& group_state = it->second;

        std::unique_lock<std::mutex> group_lock(group_state.state_mutex);
        global_lock.unlock();

        // 检查成员是否存在
        if (group_state.members.find(member_id) == group_state.members.end()) {
            res.second=Err::MEMBER_NOT_FOUND;
            return res;
        }

        // 检查世代ID是否匹配
        if (group_state.generation_id != generation_id) {
            res.second=Err::ILLEGAL_GENERATION;
            return res;
        }

        if (member_id == group_state.leader_id) {
            // 这是领导者提交分配
            if (group_state.state != Gstate::AWAITING_SYNC) {
                // 领导者在非 AWAITING_SYNC 状态提交分配，可能是旧的 SyncGroup 请求或状态错误
                res.second=Err::REBALANCE_IN_PROGRESS;
            }
            group_state.assignments = leader_assignments;
            group_state.state = ConsumerGroupState::STABLE;
            group_state.rebalance_ing.store(0);
            cerr("SYNC RESULT : "+member_id+ " Leader sync successfully");



        } else {
            // 非领导者成员请求分配
            if (group_state.state == ConsumerGroupState::JOIN_COLLECTING) {
                res.second=Err::REBALANCE_IN_PROGRESS;
                return res;
            }
            if (group_state.state == ConsumerGroupState::AWAITING_SYNC) {
                if (group_state.assignments.empty()) {
                    // 领导者尚未提交分配
                    res.second=Err::AWAITING_LEADER_SYNC;
                    return res;

                }
            }
            // 如果是 STABLE 状态，或者 AWAITING_SYNC 且 assignments 不为空，则继续返回分配
        }

        // 返回该成员的分配
        auto member_assignments_it = group_state.assignments.find(member_id);
        if (member_assignments_it != group_state.assignments.end()) {
            return {member_assignments_it->second,Err::NULL_ERROR};
        } else {
            // 领导者提交后，可能某个成员没有分配到分区（例如，没有订阅任何主题或所有分区都被其他成员分配）
            // 或者在 AWAITING_SYNC 状态下，领导者已提交但该成员的分配还未准备好（不应该发生如果分配逻辑正确）
            if (group_state.state == ConsumerGroupState::STABLE) {
                res.second=Err::NO_ASSIGNED_PARTITION;
                return res; // 稳定状态下，如果没找到分配，返回空，表示没有分配到分区
            } else {
                // 非稳定状态下，如果没找到分配，可能是重平衡未完成，或者其他问题
                res.second=Err::REBALANCE_IN_PROGRESS;

                return res;
            }
        }
        return res;
    }

    // 3. Heartbeat: 消费者发送心跳
    HeartbeatResponce heartbeat(const std::string& group_id, const std::string& member_id) {
        HeartbeatResponce responce={-1,0};
        std::shared_lock<std::shared_mutex> global_lock(group_states_mutex_);
        auto it = group_states_.find(group_id);
        if (it == group_states_.end()) {
            global_lock.unlock();
            throw (Err::GROUP_NOT_FOUND);

        }
        ConsumerGroupState& group_state = it->second;
        std::unique_lock<std::mutex> group_lock(group_state.state_mutex);
        global_lock.unlock();

        if (group_state.members.find(member_id) == group_state.members.end()) {
            throw (Err::MEMBER_NOT_FOUND);
        }
        auto now=std::chrono::steady_clock::now();
        if(group_state.state==Gstate::STABLE){
            group_state.last_heartbeat[member_id] = now;
        }

        if(group_state.state==Gstate::STABLE){
            responce.groupstate_digit=0;
        }
        else  if(group_state.state==Gstate::JOIN_COLLECTING){
            responce.groupstate_digit=1;
        }
        else  if(group_state.state==Gstate::AWAITING_SYNC){
            responce.groupstate_digit=2;
        }
        else  if(group_state.state==Gstate::EMPTY){
            responce.groupstate_digit=3;
        }

        responce.generation_id=group_state.generation_id;

        return responce;
    }

    // 4. LeaveGroup: 消费者主动离开组
    Err leaveGroup(const std::string& group_id, const std::string& member_id) {
        std::unique_lock<std::shared_mutex> global_lock(group_states_mutex_);
        auto it = group_states_.find(group_id);
        if (it == group_states_.end()) {
            global_lock.unlock();
            cerr("Warning: LeaveGroup: Group not found: " + group_id );
            return Err::GROUP_NOT_FOUND;
        }
        ConsumerGroupState& group_state = it->second;

        std::unique_lock<std::mutex> group_lock(group_state.state_mutex);
        global_lock.unlock();
        if (group_state.members.erase(member_id) > 0) {
            group_state.last_heartbeat.erase(member_id);
            group_state.map_subscribed_topics.erase(member_id);
            if (group_state.members.empty()) {
                group_state.state = ConsumerGroupState::EMPTY;
                group_lock.unlock();
            }
            triggerRebalance(group_id);
        } else {
            cerr("Warning: LeaveGroup: Member '" + member_id + "' not found in group '" + group_id+"' ." ) ;
            return Err::MEMBER_NOT_FOUND;
        }

        out("LeaveGroup: Member " + member_id + " leave group " + group_id);
        return Err::NULL_ERROR;
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
        std::vector<std::string> groups_to_rebalance;
        std::vector<std::string> empty_groups_to_remove;

        std::vector<std::string> current_group_ids;
        {
            std::shared_lock<std::shared_mutex> global_read_lock(group_states_mutex_);
            if (group_states_.empty()) {
                return;
            }
            for (const auto& pair : group_states_) {
                current_group_ids.push_back(pair.first);
            }
        }

        for (const std::string& group_id : current_group_ids) {
            std::shared_lock<std::shared_mutex> global_read_lock_for_group_ref(group_states_mutex_);
            auto it = group_states_.find(group_id);
            if (it == group_states_.end()) {
                global_read_lock_for_group_ref.unlock();
                continue;
            }
            ConsumerGroupState& group_state = it->second;

            std::unique_lock<std::mutex> group_lock(group_state.state_mutex);
            global_read_lock_for_group_ref.unlock();

            if(group_state.state == ConsumerGroupState::EMPTY){
                empty_groups_to_remove.push_back(group_id);
                group_lock.unlock();
                continue;
            }


            if (group_state.state == ConsumerGroupState::JOIN_COLLECTING ||
                    group_state.state == ConsumerGroupState::AWAITING_SYNC) {
                group_lock.unlock();
                continue;
            }

            auto now = std::chrono::steady_clock::now();
            bool rebalance_needed = false;
            std::vector<std::string> members_to_remove; // Collect members to remove

            // Iterate through members to find timed-out ones
            for (const auto& member_pair : group_state.members) {
                auto heartbeat_it = group_state.last_heartbeat.find(member_pair.first);
                if (heartbeat_it != group_state.last_heartbeat.end() &&
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - heartbeat_it->second).count() >= session_timeout_ms_) {
                    members_to_remove.push_back(member_pair.first);
                    rebalance_needed = true;
                }
            }

            for (const std::string& member_id : members_to_remove) {
                timer.cancel_task( group_state.rebalance_timeout_taskid);
                timer.cancel_task(group_state.join_collect_timeout_taskid);
                group_state.members.erase(member_id);
                group_state.last_heartbeat.erase(member_id);
                group_state.map_subscribed_topics.erase(member_id);
                cerr("CheckHeartbeat : Group '"+group_id+"' : Member '"+member_id+"' heartbeat timeout .");
            }

            if (rebalance_needed) {


                if (group_state.members.empty()) {
                    group_state.state = ConsumerGroupState::EMPTY;
                    empty_groups_to_remove.push_back(group_id);
                } else {
                    groups_to_rebalance.push_back(group_id);
                }
            }
            group_lock.unlock(); // 释放组锁
        }


        // Phase 2: Perform map modifications (erase empty groups)
        if (!empty_groups_to_remove.empty()) {
            std::unique_lock<std::shared_mutex> global_write_lock(group_states_mutex_);
            for (const std::string& group_id : empty_groups_to_remove) {
                group_states_.erase(group_id);
                cerr("Checkliveness : Group '"+group_id+"' is empty now . Remove .");
            }
        }

        // Phase 3: Trigger rebalances asynchronously
        for (const auto& group_id : groups_to_rebalance) {
            triggerRebalance(group_id); // 调用 triggerRebalance，它会处理调度
        }
    }



    Mybyte assign_prepare_action(ConsumerGroupState& group_state) {
        group_state.leader_id=group_state.members.begin()->first;

        MB mb;
        mb.append(group_state.leader_id);


        std::set<std::string > set_this_group_partitionnum_of_topic;

        mb.append_size_t(group_state.map_subscribed_topics.size());

        {
            for(const auto& [member,subsribe_topics]:group_state.map_subscribed_topics){
                mb.append(member,static_cast<size_t>(subsribe_topics.size()) );
                for(const auto& topicname:subsribe_topics){
                    mb.append(topicname);
                    set_this_group_partitionnum_of_topic.emplace(topicname);
                }
            }

        }

        mb.append_size_t(set_this_group_partitionnum_of_topic.size());


        {
             std::shared_lock<std::shared_mutex> slock(topics_map.mtx_topic);
            for(auto& topic:set_this_group_partitionnum_of_topic){

                mb.append_string(topic);
                auto it=topics_map.topics_.find(topic);
                if(it==topics_map.topics_.end()){
                    mb.append_size_t(0);
                }
                else{
                    mb.append_size_t( it->second->get_parti_num());
                }
            }

        }




        return mb.data;
    }


    void forcestop_rebalance(const std::string& group_id){
        std::unique_lock<std::shared_mutex> global_lock(group_states_mutex_);
        auto it = group_states_.find(group_id);
        if (it == group_states_.end()) {
            global_lock.unlock();
            std::cerr << "Rebalance: Group " << group_id << " not found, possibly removed." << std::endl;

            return;
        }
        ConsumerGroupState& group_state = it->second;

        std::unique_lock<std::mutex> group_lock(group_state.state_mutex);
        global_lock.unlock();


        forcestop_rebalance(group_state);


    }

    void forcestop_rebalance(ConsumerGroupState& group_state){
        if(group_state.state!=Gstate::STABLE){
            out("rebalance be forced to stop");
        }

        group_state.state=Gstate::STABLE;
        group_state.rebalance_ing.store(0);

        timer.commit_ms([this,&group_state]{
            timer.cancel_task(group_state.join_collect_timeout_taskid);
            timer.cancel_task(group_state.rebalance_timeout_taskid);
        },10,10,1);

    }

    void Rebalance(const std::string& group_id) {
        std::unique_lock<std::shared_mutex> global_lock(group_states_mutex_);
        auto it = group_states_.find(group_id);
        if (it == group_states_.end()) {
            global_lock.unlock();
            std::cerr << "Rebalance: Group " << group_id << " not found, possibly removed." << std::endl;
            return;
        }
        ConsumerGroupState& group_state = it->second;

        std::unique_lock<std::mutex> group_lock(group_state.state_mutex); // 持有组锁
        global_lock.unlock(); // 释放全局锁


        if(group_state.state != ConsumerGroupState::JOIN_COLLECTING){
            return ;
        }


        if(group_state.members.empty()){
            group_state.state=Gstate::EMPTY;
            forcestop_rebalance(group_state);
            return ;
        }



        // 执行分区分配准备逻辑
        auto memberinfs = assign_prepare_action(group_state);
        group_state.state = ConsumerGroupState::AWAITING_SYNC;

        cerr("Rebalance: Group " + group_id +" (gen " +std::to_string( group_state.generation_id) + ") moved to state 'AWAITING_SYNC' . Leader: " + group_state.leader_id);


        std::vector<std::pair<std::string, std::string>> members_to_notify;
        for(const auto& member_pair : group_state.members){
            MB mb;
            mb.append(static_cast<uint16_t>(Err::NULL_ERROR),group_id,group_state.generation_id,member_pair.second.memberid);
            if(member_pair.first == group_state.leader_id){
                mb.append_uchar_vector(memberinfs);
            }
            else{
                mb.append(Mybyte{});
            }
            send_notice_internal( member_pair.second.userinfo.sock,member_pair.second.correlation_id_lastjoin, Eve::SERVER_RESPONSE_JOIN_REQUEST_HANDLED,mb.data );
        }




    }
    void loadConsumerinf(ConsumerGroupState& group_state, const ConsumerInfo& inf){
        group_state.members.insert({inf.memberid,inf});
        group_state.map_subscribed_topics[inf.memberid]=inf.subscribed_topics;
        group_state.last_heartbeat[inf.memberid]=std::chrono::steady_clock::now();
    }

    // 触发重平衡
    int triggerRebalance(const std::string& group_id,const ConsumerInfo& inf=ConsumerInfo()){

        std::unique_lock<std::shared_mutex> global_lock(group_states_mutex_);
        auto it = group_states_.find(group_id);
        if (it == group_states_.end()) {
            global_lock.unlock();
            return -1;
        }

        ConsumerGroupState& group_state = it->second;

        std::unique_lock<std::mutex> group_lock(group_state.state_mutex);
        global_lock.unlock();


        // 如果已经在重平衡任务调度过程中，则不再重复调度
        std::weak_ptr<GroupCoordinator> weak_self = shared_from_this();
        if (!group_state.rebalance_ing.load()) {
            std::vector<std::string> expected_members;
            expected_members.reserve(group_state.members.size());
            for(auto pair:group_state.members){
                expected_members.emplace_back(pair.second.memberid);
            }
            group_state.expected_members.reset(expected_members);
            group_state.map_subscribed_topics.clear();
            group_state.generation_id++;
            group_state.leader_id.clear();
            group_state.state = ConsumerGroupState::JOIN_COLLECTING;
            group_state.rebalance_ing.store(1);
            group_state.assignments.clear();

            std::cerr << "TriggerRebalance: Group " << group_id << " entering PREPARING_REBALANCE (new gen: " << group_state.generation_id << ")" << std::endl;

            if(inf.userinfo.sock!=-1){
                loadConsumerinf(group_state,inf);
                auto res= group_state.expected_members.callandcheck(inf.memberid);
                group_lock.unlock();
                if(res){
                    if (auto self = weak_self.lock()) {
                        self->Rebalance(group_id);
                    }
                }
                else{
                    group_state.join_collect_timeout_taskid=timer.commit_ms(
                                [weak_self, group_id]() {
                        if (auto self = weak_self.lock()) {
                            self->Rebalance(group_id);
                        }
                    },
                    join_collect_timeout_ms_,
                    join_collect_timeout_ms_,
                    1
                    );
                }
            }
            else{
                group_lock.unlock();
            }

            group_state.rebalance_timeout_taskid=timer.commit_ms(
                        [weak_self, group_id]() {
                if (auto self = weak_self.lock()) {
                    self->forcestop_rebalance(group_id);
                }
            },
            rebalance_timeout_ms,
            rebalance_timeout_ms,
            1
            );
        }
        else{
            if(group_state.state==Gstate::JOIN_COLLECTING){
                loadConsumerinf(group_state,inf);
                auto res= group_state.expected_members.callandcheck(inf.memberid);
                group_lock.unlock();
                if(res){
                    if (auto self = weak_self.lock()) {
                        self->Rebalance(group_id);
                    }
                }
            }
            else{
                group_lock.unlock();
            }
        }
        int generation_return=group_state.generation_id;
        return generation_return;
    }

private:

    std::shared_ptr<ConsumerOffset> consumer_offset_manager_;

    std::map<std::string, ConsumerGroupState> group_states_; // group_id -> ConsumerGroupState
    std::shared_mutex group_states_mutex_; // 保护 group_states_

    Timer timer;


    int session_timeout_ms_ =MYMQ::session_timeout_ms_;
    int join_collect_timeout_ms_ = MYMQ::join_collect_timeout_ms; // 重平衡join窗口期
    int rebalance_timeout_ms=MYMQ::rebalance_timeout_ms; // 重平衡超时时长
    bool init_ed{0};
    Groupcoordinator_cb callback_;
    std::shared_mutex mtx_callback;
    Topicmap& topics_map;

};


class MessageQueue : public std::enable_shared_from_this<MessageQueue> {
public:
    explicit MessageQueue(const std::string& data_root_dir = "./data/")
        : data_root_dir_(data_root_dir),
          topics_metadata_filename_(data_root_dir_ + "/topics_metadata.conf")

    {
        std::filesystem::create_directories(data_root_dir_);
        init();
        load_topics_metadata(); // 这一步会尝试从文件加载并创建 consumer_offset_topic_ptr_

        // 确保 consumer_offset_topic_ptr_ 已经初始化
        if (!consumer_offset_topic_ptr_) {
            // 如果元数据文件不存在，或者文件中没有 MYMQ::consumeroffset_name 的条目，则在此处创建
            std::cerr << "Warning: Special topic '" << MYMQ::consumeroffset_name << "' not found in metadata. Creating with default partitions (10)." << std::endl;
            consumer_offset_topic_ptr_ = std::make_unique<Topic>(MYMQ::consumeroffset_name, data_root_dir_, 10);
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
    }


    size_t get_partition_endoffset(const std::string& topicname,size_t partition ){


        std::shared_lock<std::shared_mutex> slock(topicmap.mtx_topic);
        auto it= topicmap.topics_.find(topicname);
        if(it==topicmap.topics_.end()){
            cerr("Topic not found.") ;
            slock.unlock();
            return UINT64_MAX;

        }
        slock.unlock();
        return   it->second->get_endoffset(partition);



    }
    void load_topics_metadata() {
        std::ifstream ifs(topics_metadata_filename_);
        if (!ifs.is_open()) {
            std::cerr << "Warning: Topic metadata file not found, will be created on save: " << topics_metadata_filename_ << std::endl;
            return; // 文件不存在，consumer_offset_topic_ptr_ 将在构造函数中创建
        }

        std::string line;
        while (std::getline(ifs, line)) {
            std::istringstream iss(line);
            std::string topicname;
            size_t parti_num;
            if (!(iss >> topicname >> parti_num)) {
                std::cerr << "Error: Malformed line in topics_metadata.conf: " << line << std::endl;
                continue;
            }

            if (topicname == MYMQ::consumeroffset_name) {
                // 这是特殊的消费者偏移量 Topic
                if (!consumer_offset_topic_ptr_) { // 避免重复创建
                    consumer_offset_topic_ptr_ = std::make_unique<Topic>(topicname, data_root_dir_, parti_num);
                } else {
                    std::cerr << "Warning: Duplicate entry for '" << MYMQ::consumeroffset_name << "' in metadata file. Ignoring subsequent entries." << std::endl;
                }
            } else {
                // 普通 Topic，添加到 topicmap
                std::unique_lock<std::shared_mutex> ulock(topicmap.mtx_topic);
                topicmap.topics_.emplace(topicname, std::make_unique<Topic>(topicname, data_root_dir_, parti_num));
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

        {
            std::shared_lock<std::shared_mutex> slock(topicmap.mtx_topic);
            for (const auto& pair : topicmap.topics_) {
                ofs_tmp << pair.first << " " << pair.second->get_parti_num() << std::endl;
            }
        }

        ofs_tmp.close();

        if (std::rename(temp_filename.c_str(), topics_metadata_filename_.c_str()) != 0) {
            std::remove(temp_filename.c_str());
            throw std::runtime_error("Failed to rename temporary topic metadata file to " + topics_metadata_filename_ + ": " + std::strerror(errno));
        }
    }

    Err push(Mybyte& msg ,const std::string& topicname,size_t partition) {
        auto it = topicmap.topics_.find(topicname);
        if (it == topicmap.topics_.end()) {
            throw std::runtime_error("Topic not found: " + topicname);
        }
        Topic* topic_ptr = it->second.get();
        if (partition < 0 || partition>= topic_ptr->get_parti_num()) {
            throw std::out_of_range("Invalid partition ID for topic " + topicname + ": " + std::to_string(partition));
            return Err::NO_ASSIGNED_PARTITION;
        }
       return topic_ptr->push(msg,partition);
    }

    std::pair<MesLoc,Err>  pull(size_t target_offset,const std::string& groupid,const std::string& topicname, size_t partition,size_t byte_need) {

        auto statepair=get_SpecificState(groupid);
        if(!statepair.second){
            return {MesLoc{},Err::GROUP_NOT_FOUND};
        }

        if(statepair.first==ConsumerGroupState::GroupState::STABLE){

        }
        else if(statepair.first==ConsumerGroupState::GroupState::EMPTY){
            return {MesLoc{},Err::EMPTY_GROUP};
        }
        else{
            return {MesLoc{},Err::REBALANCE_IN_PROGRESS};
        }



        std::unique_lock<std::shared_mutex> ulock(topicmap.mtx_topic);
        auto it = topicmap.topics_.find(topicname);
        if (it == topicmap.topics_.end()) {
            ulock.unlock();
            return {MesLoc{},Err::TOPIC_NOT_FOUND};
        }
        Topic* topic_ptr = it->second.get();
        ulock.unlock();

        auto locinf= topic_ptr->pull(target_offset,partition,byte_need);
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

        {
            std::unique_lock<std::shared_mutex> ulock(topicmap.mtx_topic);
            if (topicmap.topics_.count(topicname)) {
                out("CREATE TOPIC: Topic '"+ topicname + "' already exists." );
                return false;
            }
            topicmap.topics_.emplace(topicname, std::make_unique<Topic>(topicname, data_root_dir_, parti_num));
        }

        save_topics_metadata(); // 保存新的 Topic 元数据
        return true;
    }


    Err commit_sync(const std::string& topicname, size_t partition ,uint32_t consumeroffset_parid_hash,const std::string& key,uint32_t offset_digit) {
        if(!check_topic_and_partition_outlock(topicname,partition)){
            return Err::TOPIC_NOT_FOUND;
        }
        if (!consumer_offset_manager_ptr_) {
            return Err::INTERNAL_ERROR; // 或者更具体的错误类型
        }
        return consumer_offset_manager_ptr_->commit_sync(consumeroffset_parid_hash,key,offset_digit);
    }




    void clear_partition(const std::string& topicname,size_t partition) {
        std::unique_lock<std::shared_mutex> ulock(topicmap.mtx_topic);
        auto it = topicmap.topics_.find(topicname);
        if (it == topicmap.topics_.end()) {
            throw std::runtime_error("Topic not found: " + topicname);
        }
        Topic* topic_ptr = it->second.get();

        ulock.unlock();
        if (partition >= 0 && partition < topic_ptr->get_parti_num()) {
            topic_ptr->clear_partition(partition);
        } else {
            throw std::out_of_range("Invalid partition ID for clear_partition.");
        }
    }

    std::pair<int,Err> joinGroup(const std::string& groupid, ConsumerInfo& inf){
        for(const auto&it:inf.subscribed_topics){
            create_topic(it);
        }

        return groupcoordinator_->joinGroup(groupid,inf);
    }

    Err leave_group(const std::string& groupid,const std::string& memberid){
       return groupcoordinator_->leaveGroup(groupid,memberid);
    }

    std::pair<std::map<std::string, std::set<size_t>>,Err> sync_group(
            const std::string& group_id,
            const std::string& member_id,
            int generation_id,
            const std::map<std::string, std::map<std::string, std::set<size_t>>>& leader_assignments = {}
            ) {
        return groupcoordinator_->syncGroup(group_id, member_id, generation_id, leader_assignments);

    }


    HeartbeatResponce heartbeat(const std::string& group_id, const std::string& member_id) {
        return groupcoordinator_->heartbeat(group_id, member_id);
    }

    std::pair< ConsumerGroupState::GroupState,bool> get_SpecificState(const std::string& groupid){
        return  groupcoordinator_->get_SpecificState(groupid);

    }



private:

    bool check_topic_and_partition_outlock(const std::string& topicname,size_t partition,std::string description_if_needed=std::string{}){
        if(!description_if_needed.empty()){
            cerr("Description : "+description_if_needed);
        }
        std::unique_lock<std::shared_mutex> ulock(topicmap.mtx_topic);
        auto it = topicmap.topics_.find(topicname);
        if (it == topicmap.topics_.end()) {
            cerr("Topic not found: " + topicname);
            return 0;

        }
        Topic* topic_ptr = it->second.get();

        ulock.unlock();

        if (partition < 0 || partition>= topic_ptr->get_parti_num()) {
            cerr("Invalid partition ID for topic " + topicname + ": " + std::to_string(partition));
            return 0;

        }


        return 1;
    }

    void start_server(){

        server_.set_client_message_callback(
                    [this](int client_fd,std::string clientid, short event_type_short,uint32_t correlation_id,uint16_t ack_level ,const Mybyte msg_body) {
            MYMQ::EventType type = static_cast<MYMQ::EventType>(event_type_short);

            auto decoded_msg=std::move(msg_body) ;
            cerr("["+std::to_string(correlation_id)+"]["+clientid+"]"+ MYMQ::to_string(static_cast<Eve>(event_type_short))+" called.");


            MessageParser mp(decoded_msg);
            if(type==MYMQ::EventType::CLIENT_REQUEST_PULL){

                auto groupid=mp.read_string();
                auto topicname=mp.read_string();
                auto partition=mp.read_size_t();
                auto offset=mp.read_size_t();
                auto bytes_need=mp.read_size_t();
                auto res= pull(offset,groupid,topicname,partition,bytes_need);
                bool failed=1;
                if(res.second==Err::NULL_ERROR){
                    auto pullout_res= server_.pull_out(client_fd,correlation_id,ack_level, res.first,topicname,partition) ;
                    if(pullout_res==Err::NULL_ERROR){
                        failed=0;
                    }
                }
                if(failed){

                    cerr(MYMQ_Public::to_string(static_cast<Err>( res.second)));
                    MB mb;
                    mb.append( static_cast<uint16_t>( res.second),offset,topicname,partition);
                    send(client_fd,Eve::SERVER_RESPONSE_PULL_DATA,correlation_id,ack_level,mb.data);
                    cerr(std::to_string(offset));
                }





            }
            else if(type==MYMQ::EventType::CLIENT_REQUEST_PUSH){



                auto topicname= mp.read_string();
                auto partition=mp.read_size_t();

                auto crc= mp.read_uint32();
                auto msg_batch=mp.read_uchar_vector();


                MB mb_res;
                mb_res.reserve(sizeof (uint32_t)+topicname.size()+sizeof(size_t)+sizeof (uint16_t));
                mb_res.append(topicname,partition);

                if(!MYMQ::Crc32::verify_crc32(msg_batch,crc)){
                    cerr("Push CRC verify : Not match , refused to push");
                    mb_res.append_uint16(static_cast<uint16_t>(Err::CRC_VERIFY_FAILED));
                    send(client_fd,Eve::SERVER_RESPONSE_PUSH_ACK,correlation_id,ack_level,mb_res.data);
                    return;
                }
                if(ack_level==static_cast<uint16_t>(MYMQ::ACK_Level::ACK_PROMISE_ACCEPT)){
                    mb_res.append_uint16(static_cast<uint16_t>(Err::NULL_ERROR));
                    send(client_fd,Eve::SERVER_RESPONSE_PUSH_ACK,correlation_id,ack_level,mb_res.data);
                }

                auto push_res= push(msg_batch,topicname,partition);

                if(ack_level==static_cast<uint16_t>(MYMQ::ACK_Level::ACK_PROMISE_INDISK)){
                    mb_res.append_uint16(static_cast<uint16_t>(push_res));
                    send(client_fd,Eve::SERVER_RESPONSE_PUSH_ACK,correlation_id,ack_level,mb_res.data);
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
                send(client_fd,Eve::SERVER_RESPONCE_COMMIT_OFFSET,correlation_id,ack_level,mb.data);



            }
            else if( type==MYMQ::EventType::CLIENT_REQUEST_JOIN_GROUP){

                auto groupid=mp.read_string();
                auto memberid=mp.read_string();
                auto generationid=mp.read_int();
                auto client_id=mp.read_string();
                auto topics=mp.read_uchar_vector();

                MessageParser mp2(topics);
                size_t topicsnum=mp2.read_size_t();
                std::set<std::string> topicset;
                for(size_t i=0;i<topicsnum;i++){
                    topicset.emplace(mp2.read_string());
                }
                ConsumerInfo inf(std::move(topicset),std::move(memberid) , generationid,correlation_id,client_id,client_fd);
                auto res= joinGroup(groupid,inf);

                if(res.second!=Err::NULL_ERROR){
                    MessageBuilder mb;
                    mb.append(static_cast<uint16_t>(res.second),groupid,res.first);
                    send(client_fd,Eve::SERVER_RESPONSE_JOIN_REQUEST_HANDLED,correlation_id,ack_level,mb.data);
                }


            }
            else if(type==MYMQ::EventType::CLIENT_REQUEST_LEAVE_GROUP){
                auto groupid=mp.read_string();
                auto memberid=mp.read_string();
                auto res=  leave_group(groupid,memberid);
                MB mb;
                mb.append(static_cast<uint16_t>(res),groupid);
                send(client_fd,Eve::SERVER_RESPONCE_LEAVE_GROUP,correlation_id,ack_level,mb.data);
            }
            else if(type==MYMQ::EventType::CLIENT_REQUEST_SYNC_GROUP){

                auto groupid=mp.read_string();
                auto memberid=mp.read_string();
                auto generationid=mp.read_int();
                auto pull_option= static_cast<MYMQ::PullSet>( mp.read_uint16());
                auto isleader=is_leader(groupid,memberid);
                std::pair<std::map<std::string, std::set<size_t>>,Err> sync_res;
                if(isleader){
                    auto assignment=parse_assignments_message(mp.read_uchar_vector());
                    sync_res= sync_group(groupid,memberid,generationid,assignment);
                }
                else{
                    sync_res= sync_group(groupid,memberid,generationid);
                }




                MB mb;
                mb.append(static_cast<short>(sync_res.second),groupid );

                if(sync_res.second==Err::NULL_ERROR){
                    mb.append(sync_res.first.size());
                    for(const auto& [topic,parti_set]:sync_res.first){
                        mb.append(topic);
                        mb.append_size_t( parti_set.size());
                        for(const auto& parti_id:parti_set){
                            //需要放入每个分区的起始消费的那个偏移
                            if(pull_option==MYMQ::PullSet::END_OFFSET){
                                auto endoff= get_partition_endoffset(topic,parti_id);
                                mb.append(parti_id,endoff);
                            }
                            else if(pull_option==MYMQ::PullSet::EARLIEST_OFFSET){
                                mb.append(parti_id,size_t(0));
                            }


                        }
                    }
                }



                send(client_fd,Eve::SERVER_RESPONSE_SYNC_GROUP_ACK,correlation_id,ack_level,mb.data);
            }
            else if(type==MYMQ::EventType::CLIENT_REQUEST_HEARTBEAT){
                auto groupid=mp.read_string();
                auto memberid=mp.read_string();
                auto res= heartbeat(groupid,memberid);
                MB mb;
                mb.append(res.generation_id,res.groupstate_digit);
                send(client_fd,Eve::SERVER_RESPONCE_HEARTBEAT,correlation_id,ack_level,mb.data);


            }
            else if(type==Eve::CLIENT_REQUEST_CREATE_TOPIC){
                auto topicname=mp.read_string();
                auto num=mp.read_size_t();
                auto res= create_topic(topicname,num);
                MB mb;
                mb.append(res);
                send(client_fd,Eve::SERVER_RESPONSE_CREATE_TOPIC,correlation_id,ack_level,mb.data);

            }

        }
        );



        server_thread_ = server_.start_in_thread();
        out("Server has started and is listening for connections." );
    }


    void send(int target_fd,Eve event_type,uint32_t correlation_id,uint16_t ack_level,const Mybyte& msg_body){
        server_.send_msg(target_fd,event_type,correlation_id,ack_level,msg_body);
    }

    void start_groupcoordinator(){
        // 在使用之前检查 consumer_offset_manager_ptr_ 是否有效
        if (!consumer_offset_manager_ptr_) {
            throw std::runtime_error("ConsumerOffset manager not initialized before starting GroupCoordinator.");
        }
        groupcoordinator_ = std::make_shared<GroupCoordinator>(consumer_offset_manager_ptr_, topicmap);
        groupcoordinator_->set_callback([this](int client_fd,uint32_t correlation_id_lastjoin,Eve event_type, Mybyte msg){
            uint16_t placehold2=UINT16_MAX;
            server_.send_msg(client_fd,event_type,correlation_id_lastjoin,placehold2, msg);
        });
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

    bool is_leader(const std::string& groupid,const std::string& memberid){
        return groupcoordinator_->is_leader(groupid,memberid);
    }

    std::map<std::string, std::map<std::string, std::set<size_t>>> parse_assignments_message(const Mybyte& serialized_data) {
        MessageParser parser(serialized_data);
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
    Topicmap topicmap;


    std::shared_ptr<GroupCoordinator> groupcoordinator_;


    Server server_;
    Timer timer_;
    std::thread server_thread_;


};

#endif // MESSAGEQUEUE_H

