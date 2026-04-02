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
#include"SharedThreadPool.h"
#include"Controller.h"
#include"unordered_set"
#include<unordered_map>


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
            actual_max_offset_from_segments = segments_.back()->next_offset();
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
                if (err == Err::Success) {
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
            if (err == Err::Success) {

            }
            return err; // 无论是否再次 Full，这里简单返回，或者你可以做循环重试
        }

        // 确实满了，创建新 Segment
        create_new_segment(curr_write_segment->next_offset());
        curr_write_segment = segments_.back().get();

        // 写入新 Segment
        auto pair = curr_write_segment->append(msg_view);
        if (pair.second == Err::Success) {
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


    void archive_segment(uint64_t segment_base_offset) {
        // 1. 获取独占写锁 (Block all Readers and Writers)
        std::unique_lock<std::shared_mutex> lock(mtx_file);

        // 2. 查找目标 Segment 的迭代器
        auto it = std::find_if(segments_.begin(), segments_.end(),
            [segment_base_offset](const std::unique_ptr<LogSegment>& seg) {
                return seg->base_offset() == segment_base_offset;
            });

        // 如果没找到，或者试图清理当前正在写的 Active Segment，直接返回
        if (it == segments_.end()) {
            std::cerr << "[PartitionStorage] Archive failed: Segment not found. Offset: " << segment_base_offset << std::endl;
            return;
        }

        if (it->get() == curr_write_segment) {
            std::cerr << "[PartitionStorage] Archive failed: Cannot clean active segment. Offset: " << segment_base_offset << std::endl;
            return;
        }

        // 3. 转移所有权 (Move Ownership)
        // 将 unique_ptr 从 vector 移动到局部变量 victim_segment。
        // 此时 vector 中该位置不再拥有对象，随后我们立即 erase。
        std::unique_ptr<LogSegment> victim_segment = std::move(*it);

        // 4. 从列表中移除 (Erase)
        segments_.erase(it);

        // 此时 segments_ 已经不包含该段，新的 Reader 无法通过 find_segment 找到它。
        // 旧的 Reader 因为被 unique_lock 阻塞，所以也不会正在访问它。

        // 5. 执行清理操作 (Flush, Close, Unmap, Rename)
        // 这一步必须在 victim_segment 析构之前完成
        victim_segment->mark_as_clean_in_lock();

        // 6. 函数结束，lock 析构自动解锁，victim_segment 析构自动释放内存
        std::cout << "[PartitionStorage] Segment archived: " << segment_base_offset << std::endl;
    }


    size_t get_endoffset(){
        return end_offset.load();
    }

    uint64_t get_earilestoffset()  {
        std::shared_lock<std::shared_mutex> lock(mtx_file);

        return segments_.front()->base_offset();
    }

    void start_logcleaner(){
        timer_.commit_s([this]{
            log_compact();
        },LOG_CLEAN_S,LOG_CLEAN_S);
    }
    void log_compact(){
        std::vector<LogSegment*> old_segments;
        uint64_t new_base_offset = 0;
        {
            std::shared_lock<std::shared_mutex> lock(mtx_file);
            if (segments_.empty()) return;
            LogSegment* active = curr_write_segment;
            for (size_t i = 0; i < segments_.size(); ++i) {
                if (segments_[i].get() == active) break;
                if (i == 0) new_base_offset = segments_[i]->base_offset();
                old_segments.push_back(segments_[i].get());
            }
        }
        if (old_segments.empty()) return;
        std::unordered_map<std::string, size_t> latest;
        for (auto* seg : old_segments) {
            auto payloads = seg->dump_payloads_snapshot();
            for (auto& p : payloads) {
                if (p.size() < 16) continue;
                MessageParser mp(p.data(), p.size());
                mp.skip(16);
                auto key = mp.read_string();
                auto off = mp.read_size_t();
                latest[key] = off;
            }
        }
        std::vector<std::unique_ptr<LogSegment>> new_compacted;
        std::unique_ptr<LogSegment> curr_seg;
        {
            std::string log_file_path = partition_data_dir_ + "/" + LogSegment::compute_filename(new_base_offset) + ".log";
            std::string index_file_path = partition_data_dir_ + "/" + LogSegment::compute_filename(new_base_offset) + ".index";
            curr_seg = std::make_unique<LogSegment>(log_file_path, index_file_path, new_base_offset);
        }
        for (const auto& kv : latest) {
            MB mb_payload;
            mb_payload.append(kv.first);
            mb_payload.append_size_t(kv.second);
            std::vector<unsigned char> payload;
            payload.resize(sizeof(uint64_t) + sizeof(uint64_t) + mb_payload.data.size());
            uint64_t off_net = htonll(0);
            std::memcpy(payload.data(), &off_net, sizeof(uint64_t));
            uint64_t msg_num_net = htonll(1);
            std::memcpy(payload.data() + sizeof(uint64_t), &msg_num_net, sizeof(uint64_t));
            if(!mb_payload.data.empty()){
                std::memcpy(payload.data() + sizeof(uint64_t) + sizeof(uint64_t),
                            mb_payload.data.data(),
                            mb_payload.data.size());
            }
            auto res = curr_seg->append(Byte_view_pair{payload.data(), static_cast<uint32_t>(payload.size())});
            if (res.second == Err::FULL_SEGMENT) {
                new_compacted.emplace_back(std::move(curr_seg));
                uint64_t next_base = new_compacted.back()->next_offset();
                std::string new_log_file_path = partition_data_dir_ + "/" + LogSegment::compute_filename(next_base) + ".log";
                std::string new_index_file_path = partition_data_dir_ + "/" + LogSegment::compute_filename(next_base) + ".index";
                curr_seg = std::make_unique<LogSegment>(new_log_file_path, new_index_file_path, next_base);
                auto res2 = curr_seg->append(Byte_view_pair{payload.data(), static_cast<uint32_t>(payload.size())});
                (void)res2;
            }
        }
        if (curr_seg) {
            new_compacted.emplace_back(std::move(curr_seg));
        }
        {
            std::unique_lock<std::shared_mutex> lock(mtx_file);
            if (segments_.empty()) return;
            size_t active_idx = 0;
            for (; active_idx < segments_.size(); ++active_idx) {
                if (segments_[active_idx].get() == curr_write_segment) break;
            }
            for (size_t i = 0; i < active_idx; ++i) {
                segments_[i]->mark_as_clean_in_lock();
            }
            std::vector<std::unique_ptr<LogSegment>> rebuilt;
            for (auto& ns : new_compacted) {
                rebuilt.emplace_back(std::move(ns));
            }
            for (size_t i = active_idx; i < segments_.size(); ++i) {
                rebuilt.emplace_back(std::move(segments_[i]));
            }
            segments_.swap(rebuilt);
            curr_write_segment = segments_.back().get();
        }
    }



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
        auto LOG_CLEAN_S_tmp=cm_s.get_size_t("LOG_CLEAN_S");
        if(!inrange(LOG_CLEAN_S_tmp,60,864000)){
            LOG_CLEAN_S_tmp=MYMQ::LOG_CLEAN_S_DEFAULT;
        }
        LOG_CLEAN_S=static_cast<int>(LOG_CLEAN_S_tmp);

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


    size_t get_earliestoffset()  {
        return msg_stor->get_earilestoffset();
    }


    size_t get_endoffset() {
        return msg_stor->get_endoffset();
    }


private:
    std::string file_name_;
    std::string owner_topic_;
    std::string partition_data_dir_;
    std::shared_ptr<PartitionStorage> msg_stor;
};



class ConsumerOffset {
    // 直接用 vector 存储 Partition 的指针（或智能指针）
    std::vector<std::shared_ptr<Partition>> partitions_;

    public:
    ConsumerOffset(const std::string& root_dir,size_t num_partitions=10){
        if(num_partitions==0){
            throw std::out_of_range("Invalid partition num for '__consumer_offset' .");
        }
        partitions_.resize(num_partitions);
        for(size_t i = 0; i < num_partitions; ++i) {
            partitions_[i] = std::make_shared<Partition>(root_dir,MYMQ::consumeroffset_name,i,1);
        }
    }


    size_t get_partition_num(){
        return partitions_.size();
    }

    std::shared_ptr<Partition> getPartition(size_t partitionId) {
        if (partitionId < 0 || partitionId >= partitions_.size()) {
            return nullptr;
        }
        return partitions_[partitionId]; // 纯数组索引访问
    }

    std::shared_ptr<Partition> getPartition(const std::string& groupid) {

       uint32_t idx= MurmurHash2::hash(groupid)%partitions_.size();
        return partitions_[idx];
    }
    Err commit_sync(uint32_t groupid_hash_key,const Byte_view_pair& packed_offset_byte_view){
        uint32_t idx= groupid_hash_key%partitions_.size();
         return partitions_[idx]->push(packed_offset_byte_view);
    }
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

    Err commit_offset(const std::string& group_id,const std::string& member_id, size_t client_gen_id,
                      const std::string& topic, size_t partition, size_t offset){

        std::shared_lock<std::shared_mutex> global_lock(group_states_mutex_);
        auto it = group_states_.find(group_id);
        if (it == group_states_.end()) {
            global_lock.unlock();
            throw (Err::GROUP_NOT_FOUND);

        }
        auto  group_state_ptr = it->second;

        global_lock.unlock();

      auto res=  group_state_ptr->commit_offset(member_id,client_gen_id,topic,partition,offset);

        return res;
    }

    Err get_endoffset(const std::string& group_id,const std::string& topic, size_t partition, size_t& offset){

        std::shared_lock<std::shared_mutex> global_lock(group_states_mutex_);
        auto it = group_states_.find(group_id);
        if (it == group_states_.end()) {
            global_lock.unlock();
            throw (Err::GROUP_NOT_FOUND);

        }
        auto  group_state_ptr = it->second;

        global_lock.unlock();

      auto res=  group_state_ptr->get_committed_offset(topic,partition,offset);

      if(!res){
          return Err::UNKNOWN_OFFSET_KEY;
      }
        return Err::Success;
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
       return Err::Success;
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

    void set_revocation_handler(std::function<void(const std::string&, size_t)> handler) {
        revocation_handler_ = handler;
    }

    HeartbeatResponce update_subscription(const std::string& group_id, std::string& member_id, size_t gen_id, const std::set<std::string>& client_full_list) {
        HeartbeatResponce resp;


        std::unique_lock<std::shared_mutex> global_lock(group_states_mutex_);

        auto it = group_states_.find(group_id);
        if (it == group_states_.end()) {
            if (gen_id == 0) {
                // 自动创建
                auto new_group = std::make_shared<ConsumerGroupState>(group_id);
                // 【新增】注入撤销回调
                if (revocation_handler_) {
                    new_group->set_revocation_callback(revocation_handler_);
                }
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

        auto res = group_state_ptr->update_subscription(member_id, gen_id,client_full_list, *cache_metadata_ptr);

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
    std::function<void(const std::string&, size_t)> revocation_handler_;

};


class MessageQueue : public std::enable_shared_from_this<MessageQueue> {


public:
    explicit MessageQueue(const std::string& data_root_dir = "./data/")
        : data_root_dir_(data_root_dir),cache_metadata(nullptr)

    {
        topics_metadata_filename_ = data_root_dir_ + "/topics_metadata.conf";
        std::filesystem::create_directories(data_root_dir_);
        init();
        load_topics_metadata();

        // 现在 consumer_offset_topic_ptr_ 保证是有效的，可以用来构造 ConsumerOffset 管理器
         std::cerr << "Warning: Special topic '" << MYMQ::consumeroffset_name << "' not found in metadata. Creating with default partitions (10)." << std::endl;
        consumer_offset_manager_ptr_ = std::make_shared<ConsumerOffset>(data_root_dir,MYMQ::PARTITION_NUM_OF_CONSUMER_OFFSET_TOPIC);

         save_topics_metadata();
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
                if (!consumer_offset_manager_ptr_) {
                    consumer_offset_manager_ptr_ = std::make_shared<ConsumerOffset>(data_root_dir_, parti_num);
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
        if (consumer_offset_manager_ptr_) {
            ofs_tmp << MYMQ::consumeroffset_name<< " " << consumer_offset_manager_ptr_->get_partition_num() << std::endl;
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
       auto res = partition_ptr->push(msg_view);
       if(res == Err::Success) {
           // 【新增】如果有挂起的 Pull 请求，尝试唤醒
           try_complete_purgatory(topicname, partition);
       }
       return res;
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

        return {locinf,Err::Success};

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


    Err  get_endoffset_of_group_metadatacache(const std::string& group_id,const std::string& topic, size_t partition, size_t& offset) {
        return groupcoordinator_->get_endoffset(group_id,topic,partition,offset);
    }

    Err commit_sync(const std::string& group_id,const std::string& member_id, size_t client_gen_id,
                      const std::string& topic, size_t partition, size_t offset) {

        return groupcoordinator_->commit_offset(group_id,member_id,client_gen_id,topic,partition,offset);
    }

    HeartbeatResponce heartbeat(const std::string& group_id, const std::string& member_id,size_t gen_id) {
        return groupcoordinator_->heartbeat(group_id, member_id,gen_id);
    }


    HeartbeatResponce update_subscription(const std::string& group_id, std::string& member_id, size_t gen_id,const std::set<std::string>& client_full_list){
       return groupcoordinator_->update_subscription(group_id,member_id,gen_id,client_full_list);
    }


    size_t get_partition_earliestoffset(const std::string& topicname,size_t partition){

        TopicPartition_to_Log_Map::const_accessor cac;
        if(! map_tp_to_log .find(cac,TopicPartition(topicname,partition))){
           return SIZE_MAX;
        }
        auto partition_ptr=cac->second;
        cac.release();

        return  partition_ptr->get_earliestoffset();
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

private:




    // Long Polling 相关结构
    struct PendingPullRequest {
        TcpSession session;
        uint32_t correlation_id;
        uint16_t ack_level;
        std::string topic;
        size_t partition;
        size_t offset;
        size_t bytes_need;
        std::chrono::steady_clock::time_point expiration;
    };

    std::mutex pending_fetches_mutex_;
    std::map<TopicPartition, std::list<PendingPullRequest>> pending_fetches_;

    // 尝试完成挂起的请求 (当有新消息写入时调用)
    void try_complete_purgatory(const std::string& topic, size_t partition) {
        std::lock_guard<std::mutex> lock(pending_fetches_mutex_);
        TopicPartition tp(topic, partition);
        auto it = pending_fetches_.find(tp);
        if (it == pending_fetches_.end()) return;

        auto& list = it->second;
        for (auto list_it = list.begin(); list_it != list.end(); ) {
            // 尝试再次拉取
            auto res = pull(list_it->offset, list_it->topic, list_it->partition, list_it->bytes_need);
            
            if (res.second == Err::Success) {
                // 成功拉取到数据 -> 发送响应并移除请求
                send_file_packet(list_it->session, Eve::SERVER_RESPONSE_PULL_DATA, list_it->correlation_id, list_it->ack_level, res.first, list_it->topic, list_it->partition, list_it->offset);
                list_it = list.erase(list_it);
            } else {
                // 仍然没有数据 -> 检查超时
                if (std::chrono::steady_clock::now() > list_it->expiration) {
                    // 超时 -> 发送空响应 (或错误码)
                    // 这里我们发送 NO_RECORD 错误，让客户端知道超时了
                     send_error_response(list_it->session, list_it->correlation_id, list_it->ack_level, 
                                        list_it->topic, list_it->partition, Err::NO_RECORD, list_it->offset);
                    list_it = list.erase(list_it);
                } else {
                    ++list_it;
                }
            }
        }
        if (list.empty()) pending_fetches_.erase(it);
    }

    // 分区所有权撤销时触发 (Rebalance)
    void on_partition_revocation(const std::string& topic, size_t partition) {
        std::lock_guard<std::mutex> lock(pending_fetches_mutex_);
        TopicPartition tp(topic, partition);
        auto it = pending_fetches_.find(tp);
        if (it != pending_fetches_.end()) {
            for (auto& req : it->second) {
                // 强制返回错误，通知客户端 Rebalance 正在进行
                send_error_response(req.session, req.correlation_id, req.ack_level, 
                                    req.topic, req.partition, Err::REBALANCE_IN_PROGRESS, req.offset);
            }
            pending_fetches_.erase(it);
        }
    }

    // 定期检查超时 (Timer 驱动)
    void check_purgatory_expiration() {
        std::lock_guard<std::mutex> lock(pending_fetches_mutex_);
        auto now = std::chrono::steady_clock::now();

        for (auto it = pending_fetches_.begin(); it != pending_fetches_.end(); ) {
            auto& list = it->second;
            for (auto list_it = list.begin(); list_it != list.end(); ) {
                if (now > list_it->expiration) {
                    send_error_response(list_it->session, list_it->correlation_id, list_it->ack_level, 
                                        list_it->topic, list_it->partition, Err::NO_RECORD, list_it->offset);
                    list_it = list.erase(list_it);
                } else {
                    ++list_it;
                }
            }
            if (list.empty()) {
                it = pending_fetches_.erase(it);
            } else {
                ++it;
            }
        }
    }




    void start_server(){

        server_.set_client_message_callback(
                    [this](TcpSession session, const std::vector<unsigned char>& header, std::shared_ptr<std::vector<unsigned char>> msg_body) {
            
            if (header.size() < 12) return;
            if (!msg_body) return;
            MessageParser mp_header(header.data(), header.size());
            mp_header.skip(4); // Skip TotalLen
            uint16_t event_type_short = mp_header.read_uint16();
            uint32_t correlation_id = mp_header.read_uint32();
            uint16_t ack_level = mp_header.read_uint16();

            MYMQ::EventType type = static_cast<MYMQ::EventType>(event_type_short);

            cerr("["+std::to_string(correlation_id)+"]["+session.get_clientid()+"]"+ MYMQ::to_string(type)+" called.");
            MessageParser mp(msg_body->data(),msg_body->size());
            mp.skip(4);
            if(type==MYMQ::EventType::CLIENT_REQUEST_PULL){

                auto groupid=mp.read_string();
                auto topicname=mp.read_string();
                auto partition=mp.read_size_t();
                auto offset=mp.read_size_t();
                auto bytes_need=mp.read_size_t();
                const auto role = static_cast<MYMQ_Public::ChannelRole>(session.get_channel_role());
                if (role != MYMQ_Public::ChannelRole::UNKNOWN && role != MYMQ_Public::ChannelRole::FETCH) {
                    send_error_response(session, correlation_id, ack_level, topicname, partition, Err::INTERNAL_ERROR, offset);
                    return;
                }
                auto res= pull(offset,topicname,partition,bytes_need);
                bool failed=1;
                if(res.second==Err::Success){
                    send_file_packet(session, Eve::SERVER_RESPONSE_PULL_DATA, correlation_id, ack_level, res.first, topicname, partition, offset);
                    failed=0;

                }
                // --- Long Polling Logic ---
                else if (res.second == Err::NO_RECORD) {
                    std::lock_guard<std::mutex> lock(pending_fetches_mutex_);
                    TopicPartition tp(topicname, partition);
                    
                    PendingPullRequest req{
                        session,
                        correlation_id,
                        ack_level,
                        topicname,
                        partition,
                        offset,
                        bytes_need,
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(500)
                    };
                    
                    pending_fetches_[tp].push_back(req);
                    
                    failed = 0; // Handled async
                }
                // ---------------------------

                if (failed) {
                    cerr(MYMQ_Public::to_string(static_cast<Err>(res.second)));
                    send_error_response(session, correlation_id, ack_level, topicname, partition, static_cast<Err>(res.second), offset);
                    cerr(std::to_string(offset));
                }





            }
            else if(type==MYMQ::EventType::CLIENT_REQUEST_PUSH){



                auto topicname= mp.read_string();
                auto partition=mp.read_size_t();

                const auto role = static_cast<MYMQ_Public::ChannelRole>(session.get_channel_role());
                if (role != MYMQ_Public::ChannelRole::UNKNOWN && role != MYMQ_Public::ChannelRole::PRODUCE) {
                    MB mb_res;
                    mb_res.append(topicname,partition);
                    mb_res.append_uint16(static_cast<uint16_t>(Err::INTERNAL_ERROR));
                    mb_res.append_uint64(0);
                    send_packet(session, Eve::SERVER_RESPONSE_PUSH_ACK, correlation_id, ack_level, mb_res.data);
                    return;
                }
                auto crc= mp.read_uint32();
                auto msg_view=mp.read_bytes_view();



                MB mb_res;
                mb_res.reserve(sizeof (uint32_t)+topicname.size()+sizeof(size_t)+sizeof (uint16_t));
                mb_res.append(topicname,partition);


                if(!MYMQ::Crc32::verify_crc32(msg_view.first,msg_view.second,crc)){
                    cerr("Push CRC verify : Not match , refused to push");
                    if(ack_level!=static_cast<uint16_t>(MYMQ::ACK_Level::ACK_NORESPONCE)){
                            mb_res.append_uint16(static_cast<uint16_t>(Err::CRC_VERIFY_FAILED));
                            send_packet(session, Eve::SERVER_RESPONSE_PUSH_ACK, correlation_id, ack_level, mb_res.data);
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
                    send_packet(session, Eve::SERVER_RESPONSE_PUSH_ACK, correlation_id, ack_level, mb_res.data);
                }
                 cerr("Push result : "+MYMQ_Public::to_string(push_res));
            }
            else if(type==MYMQ::EventType::CLIENT_REQUEST_COMMIT_OFFSET){

                auto groupid=mp.read_string();
                auto memberid=mp.read_string();
                auto generationid=mp.read_size_t();
                auto topicname=mp.read_string();
                auto partition=mp.read_size_t();
                auto consumeroffset_parid_hash=mp.read_uint32();
                auto key_gtp=mp.read_string();
                auto offset_digit=mp.read_size_t();
                const auto role = static_cast<MYMQ_Public::ChannelRole>(session.get_channel_role());
                if (role != MYMQ_Public::ChannelRole::UNKNOWN && role != MYMQ_Public::ChannelRole::CONTROL) {
                    MB mb;
                    mb.reserve(sizeof (uint32_t)*2+groupid.size()+topicname.size()+sizeof (size_t)*2+sizeof (uint16_t));
                    mb.append(groupid,topicname,partition,static_cast<uint16_t>(Err::INTERNAL_ERROR),offset_digit);
                    send_packet(session, Eve::SERVER_RESPONCE_COMMIT_OFFSET, correlation_id, ack_level, mb.data);
                    return;
                }

                auto error= commit_sync(groupid,memberid,generationid,topicname,partition,offset_digit);
                if(error==Err::Success&&consumer_offset_manager_ptr_){
                    MB mb_payload;
                    mb_payload.append(key_gtp);
                    mb_payload.append_size_t(offset_digit);
                    std::vector<unsigned char> payload;
                    payload.resize(sizeof(uint64_t) + sizeof(uint64_t) + mb_payload.data.size());
                    uint64_t off_net = htonll(0);
                    std::memcpy(payload.data(), &off_net, sizeof(uint64_t));
                    uint64_t msg_num_net = htonll(1);
                    std::memcpy(payload.data() + sizeof(uint64_t), &msg_num_net, sizeof(uint64_t));

                    if(!mb_payload.data.empty()){
                        std::memcpy(payload.data() + sizeof(uint64_t) + sizeof(uint64_t),
                                    mb_payload.data.data(),
                                    mb_payload.data.size());
                    }

                    auto persist_err= consumer_offset_manager_ptr_->commit_sync(consumeroffset_parid_hash, Byte_view_pair{payload.data(), static_cast<uint32_t>(payload.size())});
                    if(persist_err!=Err::Success){
                        error=persist_err;
                    }
                }
                MB mb;
                mb.reserve(sizeof (uint32_t)*2+groupid.size()+topicname.size()+sizeof (size_t)*2+sizeof (uint16_t));
                mb.append(groupid,topicname,partition,static_cast<uint16_t>(error),offset_digit);
                send_packet(session, Eve::SERVER_RESPONCE_COMMIT_OFFSET, correlation_id, ack_level, mb.data);



            }
            else if(type==MYMQ::EventType::CLIENT_REQUEST_REGISTER){
                bool ok = false;
                uint16_t version = 0;
                uint16_t role = static_cast<uint16_t>(MYMQ_Public::ChannelRole::UNKNOWN);
                std::string clientid;
                try {
                    version = mp.read_uint16();
                    role = mp.read_uint16();
                    clientid = mp.read_string();
                    ok = (version == 1);
                } catch (...) {
                    ok = false;
                }

                if (ok) {
                    session.set_clientid(clientid);
                    session.set_channel_role(role);
                }

                MB mb;
                mb.append_bool(ok);
                send_packet(session, Eve::SERVER_RESPONSE_REGISTER, correlation_id, ack_level, mb.data);
            }
           else if(type==MYMQ::EventType::CLIENT_REQUEST_LEAVE_GROUP){
                auto groupid=mp.read_string();
                auto memberid=mp.read_string();
                const auto role = static_cast<MYMQ_Public::ChannelRole>(session.get_channel_role());
                if (role != MYMQ_Public::ChannelRole::UNKNOWN && role != MYMQ_Public::ChannelRole::CONTROL) {
                    MB mb;
                    mb.append(static_cast<uint16_t>(Err::INTERNAL_ERROR),groupid);
                    send_packet(session, Eve::SERVER_RESPONCE_LEAVE_GROUP, correlation_id, ack_level, mb.data);
                    return;
                }
                auto res=  leave_group(groupid,memberid);
                MB mb;
                mb.append(static_cast<uint16_t>(res),groupid);
               send_packet(session, Eve::SERVER_RESPONCE_LEAVE_GROUP, correlation_id, ack_level, mb.data);
            }
             else if(type==MYMQ::EventType::CLIENT_REQUEST_HEARTBEAT){

                auto groupid=mp.read_string();               
                auto memberid=mp.read_string();
                const auto role = static_cast<MYMQ_Public::ChannelRole>(session.get_channel_role());
                if (role != MYMQ_Public::ChannelRole::UNKNOWN && role != MYMQ_Public::ChannelRole::CONTROL) {
                    MB mb;
                    mb.append(static_cast<uint16_t>(Err::INTERNAL_ERROR),groupid,memberid,static_cast<size_t>(0));
                    send_packet(session, Eve::SERVER_RESPONCE_HEARTBEAT, correlation_id, ack_level, mb.data);
                    return;
                }
                auto generationid=mp.read_size_t();
                auto pull_start_location=static_cast<MYMQ::PullSet>( mp.read_uint16());
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
                            size_t off=SIZE_MAX;
                            if(pull_start_location==MYMQ::PullSet::END_OFFSET){

                               auto err= get_endoffset_of_group_metadatacache(groupid,topic,par,off);//off=SIZE_MAX代表没有记录
                               if(err==Err::UNKNOWN_OFFSET_KEY){
                                   off=get_partition_endoffset(topic,par);//off=SIZE_MAX代表没有这个分区
                                 }
                            }
                            else if(pull_start_location==MYMQ::PullSet::EARLIEST_OFFSET){
                                off= get_partition_earliestoffset(topic,par);//off=SIZE_MAX代表没有这个分区
                            }
                             mb.append_size_t(off);
                        }

                    }

                }
                send_packet(session, Eve::SERVER_RESPONCE_HEARTBEAT, correlation_id, ack_level, mb.data);




            }
            else if(type==Eve::CLIENT_REQUEST_CREATE_TOPIC){
                auto topicname=mp.read_string();
                auto num=mp.read_size_t();
                const auto role = static_cast<MYMQ_Public::ChannelRole>(session.get_channel_role());
                if (role != MYMQ_Public::ChannelRole::UNKNOWN && role != MYMQ_Public::ChannelRole::CONTROL) {
                    MB mb;
                    mb.append_bool(false);
                    send_packet(session, Eve::SERVER_RESPONSE_CREATE_TOPIC, correlation_id, ack_level, mb.data);
                    return;
                }
                auto res= create_topic(topicname,num);
                MB mb;
                mb.append(res);
                send_packet(session, Eve::SERVER_RESPONSE_CREATE_TOPIC, correlation_id, ack_level, mb.data);

            }

        }
        );



        timer_.commit_ms([this]{
            check_purgatory_expiration();
        }, 200, 200);

        server_thread_ = server_.start_in_thread();
        out("Server has started and is listening for connections." );
    }



    void start_groupcoordinator(){
        // 在使用之前检查 consumer_offset_manager_ptr_ 是否有效
        if (!consumer_offset_manager_ptr_) {
            throw std::runtime_error("ConsumerOffset manager not initialized before starting GroupCoordinator.");
        }
        groupcoordinator_ = std::make_shared<GroupCoordinator>(consumer_offset_manager_ptr_, cache_metadata);

        // 【新增】注入撤销回调
        groupcoordinator_->set_revocation_handler([this](const std::string& topic, size_t partition){
            this->on_partition_revocation(topic, partition);
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

    Err leave_group(const std::string& groupid,const std::string& memberid){
        return groupcoordinator_->leave_group(groupid,memberid);
    }

    void send_packet(Net::TcpSession& session, Eve type, uint32_t correlation_id, uint16_t ack_level, std::vector<unsigned char> body) {
        if (!session.is_connected()) return;

        MB mb_body;
        mb_body.append_uchar_vector(body);

        MB mb_header;
        const uint32_t total_len = static_cast<uint32_t>(12 + mb_body.data.size());
        mb_header.append_uint32(total_len);
        mb_header.append_uint16(static_cast<uint16_t>(type));
        mb_header.append_uint32(correlation_id);
        mb_header.append_uint16(ack_level);

        std::vector<unsigned char> packet = std::move(mb_header.data);
        packet.insert(packet.end(), std::make_move_iterator(mb_body.data.begin()), std::make_move_iterator(mb_body.data.end()));

        session.send(std::move(packet));
    }

    void send_file_packet(Net::TcpSession& session, Eve type, uint32_t correlation_id, uint16_t ack_level, MesLoc loc, const std::string& topic, size_t partition, size_t offset) {
         if (!session.is_connected()) return;

         // Construct Body for File Send (Metadata only)
         // The actual file content is sent via send_file
         // Body structure: [Topic:Str][Partition:8][ErrorCode:2][Offset:8][DataLen:8]
         
         // Let's construct the Metadata part of the body first
         MB mb_metadata;
         mb_metadata.append(topic);
         mb_metadata.append_size_t(partition);
         mb_metadata.append_uint16(static_cast<uint16_t>(Err::Success));
         mb_metadata.append_size_t(loc.offset_next_to_consume);
         mb_metadata.append_size_t(loc.length); // Data Length

         MB mb_prefix;
         mb_prefix.append_uint32(static_cast<uint32_t>(mb_metadata.data.size() + loc.length));

         const uint32_t total_len = static_cast<uint32_t>(12 + mb_prefix.data.size() + mb_metadata.data.size() + loc.length);

         MB mb_header;
         mb_header.append_uint32(total_len);
         mb_header.append_uint16(static_cast<uint16_t>(type));
         mb_header.append_uint32(correlation_id);
         mb_header.append_uint16(ack_level);

         std::vector<unsigned char> header_and_meta = std::move(mb_header.data);
         header_and_meta.insert(header_and_meta.end(), mb_prefix.data.begin(), mb_prefix.data.end());
         header_and_meta.insert(header_and_meta.end(), mb_metadata.data.begin(), mb_metadata.data.end());

         // Use Net::FileSendTask
         Net::FileSendTask task(loc.file_descriptor, loc.offset_in_file, loc.length, std::move(header_and_meta));
         session.send_file(std::move(task));
    }

    void send_error_response(Net::TcpSession& session, uint32_t correlation_id, uint16_t ack_level, const std::string& topic, size_t partition, Err error_code, size_t offset) {
        MB mb_res;
        // Construct error body
        // Body: [Topic][Partition][ErrorCode][Offset][DataLen=0]
        mb_res.append(topic);
        mb_res.append_size_t(partition);
        mb_res.append_uint16(static_cast<uint16_t>(error_code));
        mb_res.append_size_t(offset);
        mb_res.append_size_t(0); // Data Length = 0
        
        send_packet(session, Eve::SERVER_RESPONSE_PULL_DATA, correlation_id, ack_level, std::move(mb_res.data));
    }






private:

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

