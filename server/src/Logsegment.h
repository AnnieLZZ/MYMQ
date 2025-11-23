#ifndef LOGSEGMENT_H
#define LOGSEGMENT_H

#include"Mmapfile.h"
#include"MYMQ_Publiccodes.h"
#include <sys/uio.h>
#include"CONFIG_MANAGER.h"
#include"MYMQ_Server_ns.h"
using Err=MYMQ_Public::CommonErrorCode;
using MesLoc=MYMQ_Server::MessageLocation;


class LogSegment {

public:
    LogSegment(const std::string& log_filepath, const std::string& index_filepath,
               uint64_t base_offset, size_t max_segment_size = 100 * 1024 * 1024)
        : base_offset_(base_offset),
          max_segment_size_(max_segment_size),
          index_file_(index_filepath),
          next_offset_(base_offset),
          index_path(index_filepath),
          log_filename(log_filepath),
          bytes_last_index_entry_(0) {
        init();


        Config_manager::ensure_path_existed(log_filename);

        log_file_fd = open(log_filename.c_str(), O_RDWR | O_CREAT, 0644);
        if (log_file_fd == -1) {
            throw std::runtime_error("Failed to open file: " + log_filename+ " - " + std::strerror(errno));
        }

        struct stat st;
        if (fstat(log_file_fd, &st) == -1) {
            close(log_file_fd);
            throw std::runtime_error("Failed to get file status for " + log_filename + ": " + std::strerror(errno));
        }

        actual_physical_file_size = st.st_size;
        if(actual_physical_file_size==UINT32_MAX){
            cerr("[Logsegment] Get size of seg failed .Seg :"+log_filename);
            return ;
        }


        if(recover_index()){
//            out(log_filepath+" : recover successfully");
        }
        else{
            cerr(log_filepath+" : recover failed");
        }

        close(log_file_fd);
        log_file_fd = open(log_filename.c_str(), O_RDWR | O_APPEND, 0644);
        if (log_file_fd == -1) {
            throw std::runtime_error("Failed to open file: " + log_filename+ " - " + std::strerror(errno));
        }

    }



    void replace_mmapfile_content(Mmapfile& m_source, Mmapfile& m_receiver) {
        m_source.flush_sync();
        m_receiver.reset();
        if (std::rename(m_source.get_filename().c_str(), m_receiver.get_filename().c_str()) != 0) {
            throw std::runtime_error("Failed to rename file from '" + m_source.get_filename() +
                                     "' to '" + m_receiver.get_filename() + "': " + std::strerror(errno));
        }

        m_receiver.take_ownership_of_internal(std::move(m_source));
    }



    bool recover_index() {
        // --- 1. 准备阶段 ---
        size_t index_size_on_disk = 0;
        uint64_t total_msg_count = 0; // 用于累计消息条数，计算 next_offset
        size_t actual_log_data_end = 0; // 记录最后一个有效字节的位置

        std::string index_path_recover = index_path + ".recover";

        // 创建一个新的临时索引 mmap
        Mmapfile tmp_mmap(index_path_recover);

        size_t last_phypos = 0;
        size_t log_size_on_disk = actual_physical_file_size;
        size_t bytes_since_last_index_entry = 0;

        // --- 2. 处理空文件情况 ---
        if (actual_physical_file_size == 0) {
            next_offset_ = base_offset_;
            tmp_mmap.set_curr_used_size(0);

            try {
                replace_mmapfile_content(tmp_mmap, index_file_);
            } catch (const std::runtime_error& e) {
                std::cerr << "LogSegment::recover: Failed to replace index for empty log: " << e.what() << std::endl;
                return false;
            }
            index_file_.flush_sync();
            return true;
        }

        bool recovery_failed = false;

        // --- 3. 主循环：遍历 Log 文件 ---
        while (last_phypos < log_size_on_disk) {
            // -------------------------------------------------------
            // A. 读取 Log Entry Header (12 字节: 8B Offset + 4B Size)
            // -------------------------------------------------------
            char header_buf[12];
            ssize_t read_bytes = pread(log_file_fd, header_buf, sizeof(header_buf), last_phypos);

            if (read_bytes < static_cast<ssize_t>(sizeof(header_buf))) {
                std::cerr << "[Recover] Truncated log header at pos " << last_phypos
                          << ". Expected 12 bytes, got " << read_bytes << ". Stopping." << std::endl;
                actual_log_data_end = last_phypos;
                recovery_failed = true;
                break;
            }

            uint64_t offset_net;
            uint32_t size_net;
            std::memcpy(&offset_net, header_buf, sizeof(uint64_t));
            std::memcpy(&size_net, header_buf + sizeof(uint64_t), sizeof(uint32_t));

            uint64_t current_entry_offset = ntohll(offset_net); // 当前消息的逻辑 Offset
            uint32_t payload_size = ntohl(size_net);            // 消息体长度

            // -------------------------------------------------------
            // B. 验证消息体完整性
            // -------------------------------------------------------
            size_t total_entry_size = sizeof(uint64_t) + sizeof(uint32_t) + payload_size;

            if (payload_size == 0 || last_phypos + total_entry_size > log_size_on_disk) {
                std::cerr << "[Recover] Incomplete/Invalid message body at pos " << last_phypos
                          << " (Payload Len: " << payload_size << "). Stopping." << std::endl;
                actual_log_data_end = last_phypos; // 截断到这个坏消息之前
                recovery_failed = true;
                break;
            }


            uint64_t current_batch_count = 0;

            if (payload_size >= 16) {
                char count_buf[8];
                // 读取位置 = 当前物理位置 + 12(LogHeader) + 8(BatchBaseOffset)
                ssize_t count_read_bytes = pread(log_file_fd, count_buf, 8, last_phypos + 12 + 8);

                if (count_read_bytes == 8) {
                    uint64_t batch_cnt_net;
                    std::memcpy(&batch_cnt_net, count_buf, 8);
                    current_batch_count = ntohll(batch_cnt_net);
                } else {
                    // 理论上不应该发生，因为上面已经检查了文件大小
                    actual_log_data_end = last_phypos;
                    recovery_failed = true;
                    break;
                }
            } else {
                 // Payload 太小，连 Header 都不全，视为损坏
                 std::cerr << "[Recover] Payload too small at pos " << last_phypos << ". Stopping." << std::endl;
                 actual_log_data_end = last_phypos;
                 recovery_failed = true;
                 break;
            }

            // -------------------------------------------------------
            // D. 构建稀疏索引 (Sparse Index)
            // -------------------------------------------------------
            bool should_index = (current_entry_offset == base_offset_) ||
                                (bytes_since_last_index_entry >= index_build_interval_bytes);

            if (should_index) {
                try {
                    char* index_ptr = static_cast<char*>(tmp_mmap.allocate(sizeof(uint32_t) * 2));

                    uint32_t relative_offset = static_cast<uint32_t>(current_entry_offset - base_offset_);
                    uint32_t physical_pos = static_cast<uint32_t>(last_phypos);

                    uint32_t rel_net = htonl(relative_offset);
                    uint32_t phy_net = htonl(physical_pos);

                    std::memcpy(index_ptr, &rel_net, sizeof(uint32_t));
                    std::memcpy(index_ptr + sizeof(uint32_t), &phy_net, sizeof(uint32_t));

                    index_size_on_disk += sizeof(uint32_t) * 2;
                    bytes_since_last_index_entry = 0; // 重置计数器
                } catch (const std::exception& e) {
                    std::cerr << "[Recover] Index allocation failed: " << e.what() << std::endl;
                    // 索引写失败不一定要终止 Log 恢复，但为了一致性这里选择停止
                    actual_log_data_end = last_phypos;
                    recovery_failed = true;
                    break;
                }
            }

            last_phypos += total_entry_size;           // 物理指针移动
            bytes_since_last_index_entry += total_entry_size; // 索引间隔累计
            total_msg_count += current_batch_count;    // 逻辑 Offset 累计
        }

        // --- 4. 收尾工作 ---
        if (recovery_failed) {
            // 失败处理：截断日志文件到最后一条有效数据的末尾
            std::cerr << "[Recover] Recovery failed. Truncating log to " << actual_log_data_end << std::endl;

            // 截断文件
            if (ftruncate(log_file_fd, actual_log_data_end) == -1) {
                 std::cerr << "[Recover] Critical: Failed to truncate log file: " << strerror(errno) << std::endl;
            }
            actual_physical_file_size = actual_log_data_end;

            // 删除临时索引文件
            std::remove(index_path_recover.c_str());
            return false;
        } else {
            // 成功处理
            actual_log_data_end = last_phypos;

            // 更新 next_offset_
            // 假设 offset 是连续的，next = base + count
            next_offset_ = base_offset_ + total_msg_count;

            // 替换索引文件
            try {
                replace_mmapfile_content(tmp_mmap, index_file_);
            } catch (const std::runtime_error& e) {
                std::cerr << "[Recover] Failed to swap index file: " << e.what() << std::endl;
                return false;
            }

            index_file_.set_curr_used_size(index_size_on_disk);

            // 刷盘
            flush_log();
            flush_index();
            index_file_.flush_sync();

            // 同步内存中的文件大小记录
            actual_physical_file_size = actual_log_data_end;

            return true;
        }
    }

    std::pair<uint64_t, Err> append(std::vector<unsigned char>& msg) {
        std::unique_lock<std::shared_mutex> lock(rw_mutex_);


        uint64_t current_offset = next_offset_;
        uint32_t msg_size = static_cast<uint32_t>(msg.size());
        size_t total_log_entry_size = sizeof(uint64_t) + sizeof(uint32_t) + msg_size;

        // 2. 检查容量
        if (actual_physical_file_size+ total_log_entry_size > max_segment_size_) {
            return {0, Err::FULL_SEGMENT};
        }

        // 3. 准备数据
        uint32_t encoded_size = htonl(msg_size);
        uint64_t curr_off_net = htonll(current_offset);

        // 修改 msg 内部 BaseOffset (注意：这会修改入参，确保这是预期的副作用)
        std::memcpy(msg.data(), &curr_off_net, sizeof(uint64_t));

        // 解析 msg_num
        size_t msg_num;
        uint64_t msg_num_raw;
        std::memcpy(&msg_num_raw, msg.data() + sizeof(uint64_t), sizeof(uint64_t));
        msg_num = ntohll(msg_num_raw);

        // 4. 使用 writev 避免大内存拷贝
        struct iovec iov[3];
        iov[0].iov_base = &curr_off_net;
        iov[0].iov_len = sizeof(uint64_t);
        iov[1].iov_base = &encoded_size;
        iov[1].iov_len = sizeof(uint32_t);
        iov[2].iov_base = msg.data();
        iov[2].iov_len = msg.size();

        // 5. 关键：先保存当前的物理位置用于索引！
        uint32_t index_physical_pos = actual_physical_file_size;

        // 6. 执行写入并检查错误
        ssize_t written = writev(log_file_fd, iov, 3);
        if (written != total_log_entry_size) {
            // truncate 文件到 actual_physical_file_size 以丢弃可能写入的一半数据
            ftruncate(log_file_fd, actual_physical_file_size);
            return {0, Err::IO_ERROR};
        }

        // 更新状态
        actual_physical_file_size += total_log_entry_size;
        log_bytes_since_last_flush += total_log_entry_size;


        // 7. 索引逻辑
        bool create_index_entry = (current_offset == base_offset_ ||
            actual_physical_file_size - bytes_last_index_entry_ >= index_build_interval_bytes);

        if (create_index_entry) {
            try {
                char* index_ptr = static_cast<char*>(index_file_.allocate(sizeof(uint32_t) * 2));
                uint32_t relative_offset = static_cast<uint32_t>(current_offset - base_offset_);

                // 使用之前保存的 index_physical_pos
                uint32_t encoded_relative = htonl(relative_offset);
                uint32_t encoded_physical = htonl(index_physical_pos);

                std::memcpy(index_ptr, &encoded_relative, sizeof(uint32_t));
                std::memcpy(index_ptr + sizeof(uint32_t), &encoded_physical, sizeof(uint32_t));

                bytes_last_index_entry_ = actual_physical_file_size;
            } catch (std::out_of_range& e) {
                cerr("[Logsegment] Index Full");

            }
            catch (std::runtime_error& e) {
                            cerr("[Logsegment] Index mmap crushed");

                        }
        }

        if (log_bytes_since_last_flush.load() >= LOG_FLUSH_BYTES_INTERVAL) {
            flush_log();
        }
        if(bytes_last_index_entry_>index_build_interval_bytes){
            flush_index();
        }

        next_offset_ += msg_num;
        return {current_offset, Err::NULL_ERROR};
    }


    void flush_log() {

        fsync(log_file_fd);
log_bytes_since_last_flush.store(0);
    }

    void flush_index(){
        index_file_.flush_sync();
        bytes_last_index_entry_.store(0);
    }

    MesLoc find(uint64_t target_offset, size_t byte_need) {
        std::shared_lock<std::shared_mutex> lock(rw_mutex_);
        MesLoc loc{}; // 默认 found=0

        // 1. 基础范围检查
        // 注意：如果 target_offset < base_offset，通常应该返回最早的数据(base_offset)，而不是空
        // 这里保留你的逻辑：如果超出范围则返回空
        if (target_offset >= next_offset_.load()) {
            return loc;
        }
        // 如果请求的比当前 base 还小，从 base 开始读
        if (target_offset < base_offset_) {
            target_offset = base_offset_;
        }

        uint32_t relative_offset = static_cast<uint32_t>(target_offset - base_offset_);
        char* index_start = static_cast<char*>(index_file_.give_mapped_data_ptr());
        size_t index_size = index_file_.give_curr_used_size();
        size_t num_entries = index_size / (sizeof(uint32_t) * 2);

        uint32_t phy_pos = 0;

        // 2. 二分查找 (Binary Search) - 保持不变，逻辑正确
        // 寻找最后一个 relative_offset <= target 的索引项
        if (num_entries != 0) {
            int low = 0, high = static_cast<int>(num_entries - 1), found_idx = -1;
            while (low <= high) {
                int mid = low + (high - low) / 2;
                uint32_t current_relative;
                // 边界检查
                if ((size_t)mid * 8 + 4 > index_size) break;

                std::memcpy(&current_relative, index_start + mid * 8, 4);
                current_relative = ntohl(current_relative);

                if (current_relative <= relative_offset) {
                    found_idx = mid;
                    low = mid + 1;
                } else {
                    high = mid - 1;
                }
            }

            if (found_idx != -1) {
                // 读取对应的物理位置
                if ((size_t)found_idx * 8 + 8 <= index_size) {
                    std::memcpy(&phy_pos, index_start + found_idx * 8 + 4, 4);
                    phy_pos = ntohl(phy_pos);
                }
            }
        }

        // 3. 线性扫描 (Linear Scan) 找到精确的 Batch
        size_t logsize = actual_physical_file_size;
        if (phy_pos >= logsize) return loc;

        size_t current_scan_pos = phy_pos;

        // 这里的目标是找到第一个 "结束Offset > Target" 的 Batch
        // 或者更简单：找到第一个 "Offset >= Target" 的 Batch，
        // 但如果 Target 落在某个 Batch 中间，我们需要返回那个 Batch。

        while (current_scan_pos < logsize) {
            // 读取 Log Header (12 Bytes)
            char header_buf[12];
            if (current_scan_pos + 12 > logsize) break;

            ssize_t r = pread(log_file_fd, header_buf, 12, current_scan_pos);
            if (r < 12) break;

            uint64_t off_net;
            uint32_t len_net;
            std::memcpy(&off_net, header_buf, 8);
            std::memcpy(&len_net, header_buf + 8, 4);

            uint64_t batch_base_offset = ntohll(off_net);
            uint32_t batch_len = ntohl(len_net); // Payload 长度

            // 完整性检查
            size_t total_msg_size = 12 + batch_len;
            if (current_scan_pos + total_msg_size > logsize) break;

            // --- 关键修正：判定是否命中 ---

            bool is_target_batch = false;

            if (batch_base_offset >= target_offset) {
                // Case A: 当前 Batch 的起始位置已经 >= 目标，肯定是它（或者目标不存在）
                is_target_batch = true;
            } else {
                // Case B: 当前 Batch 起始位置 < 目标。
                // 我们需要检查 Target 是否在这个 Batch 内部 (Offset ~ Offset + Count)
                // 这需要读取 Payload 里的 Batch Count (8~15字节)

                if (batch_len >= 16) {
                    char count_buf[8];
                    // Payload start = current_scan_pos + 12
                    // Batch Count offset inside payload = 8
                    // So read at: current_scan_pos + 12 + 8
                    if (pread(log_file_fd, count_buf, 8, current_scan_pos + 20) == 8) {
                        uint64_t cnt_net;
                        std::memcpy(&cnt_net, count_buf, 8);
                        uint64_t batch_count = ntohll(cnt_net);

                        // 如果 target 落在 [base, base + count) 区间内
                        if (batch_base_offset + batch_count > target_offset) {
                            is_target_batch = true;
                        }
                    }
                }
            }

            if (is_target_batch) {
                // --- 4. 命中！开始累积 byte_need ---
                loc.found = 1;
                loc.file_descriptor = log_file_fd; // 只要 fd，不重新 open
                loc.offset_in_file = static_cast<off_t>(current_scan_pos);
                loc.offset_batch_first = batch_base_offset; // 实际返回的第一个 offset

                size_t accumulated_len = 0;
                size_t accumulate_pos = current_scan_pos;

                // 循环读取后续消息，直到满足 byte_need
                while (accumulated_len < byte_need && accumulate_pos < logsize) {
                     // 读取 Header 里的 Size
                     char len_buf[4];
                     // Offset(8) + Size(4). 我们只读 Size，所以在 pos + 8
                     if (accumulate_pos + 12 > logsize) break;

                     if (pread(log_file_fd, len_buf, 4, accumulate_pos + 8) != 4) break;

                     uint32_t this_msg_len_net;
                     std::memcpy(&this_msg_len_net, len_buf, 4);
                     uint32_t this_msg_len = ntohl(this_msg_len_net);

                     size_t this_total_size = 12 + this_msg_len;

                     // 检查越界
                     if (accumulate_pos + this_total_size > logsize) break;

                     accumulated_len += this_total_size;
                     accumulate_pos += this_total_size;
                }

                loc.length = accumulated_len;
                return loc;
            }

            // 如果不是目标 Batch，跳过，看下一个
            current_scan_pos += total_msg_size;
        }

        return loc;
    }
    uint64_t base_offset() const { return base_offset_; }
    uint64_t next_offset() const { return next_offset_.load(); }

    static std::string compute_filename(size_t base_offset) {
        std::string base_string = std::to_string(base_offset);

        std::string prev_zero;
        if (base_string.length() < 20) {
            prev_zero.append(20 - base_string.length(), '0');
        }
        return prev_zero + base_string;
    }

    size_t get_this_seg_maxsize() {
        return max_segment_size_;
    }

    void clear() {
        std::unique_lock<std::shared_mutex> lock(rw_mutex_);
        index_file_.reset();
        next_offset_ = base_offset_;
        log_bytes_since_last_flush.store(0);
        bytes_last_index_entry_ = 0;
    }

private:
    void init(){
        Config_manager cm_s("config/storage.properity");
        auto log_flush_period=cm_s.get_size_t("LOG_FLUSH_BYTES_INTERVAL");
        if(!inrange(log_flush_period,256,1048576)){
            log_flush_period=MYMQ::LOG_FLUSH_BYTES_INTERVAL_DEFAULT;
        }
        LOG_FLUSH_BYTES_INTERVAL=log_flush_period;
        auto index_build_interval=cm_s.get_size_t("index_build_interval_bytes");
        if(!inrange(index_build_interval,256,1048576)){
            index_build_interval=MYMQ::index_build_interval_bytes_DEFAULT;

        }
        index_build_interval_bytes=index_build_interval;


    }

    bool inrange(size_t obj,size_t min,size_t max){
        return (obj<=max&&obj>=min);
    }
private:
    std::shared_mutex rw_mutex_;
    const uint64_t base_offset_;

    std::atomic<uint64_t> next_offset_;
    Mmapfile index_file_;
    std::atomic<size_t> log_bytes_since_last_flush{0};
    size_t LOG_FLUSH_BYTES_INTERVAL;
    size_t index_build_interval_bytes;
    std::string index_path;
    std::atomic<size_t>  bytes_last_index_entry_;



    int log_file_fd=-1;
    std::string log_filename{""};
    uint32_t actual_physical_file_size=UINT32_MAX;
    const size_t max_segment_size_;

};



#endif // LOGSEGMENT_H
