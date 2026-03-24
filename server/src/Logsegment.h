#ifndef LOGSEGMENT_H
#define LOGSEGMENT_H

#include"Mmapfile.h"
#include"MYMQ_Publiccodes.h"
#include"CONFIG_MANAGER.h"
#include"Printqueue.h"
#include"MYMQ_Server_ns.h"
#include <vector>
using Err=MYMQ_Public::CommonErrorCode;
using MesLoc=MYMQ_Server::MessageLocation;


class LogSegment {

public:
    LogSegment(const std::string& log_filepath, const std::string& index_filepath,
               uint64_t base_offset, size_t max_segment_size = MYMQ::LOGSEG_MAXSIZE_MB_DEFAULT * 1024 * 1024)
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
        // 1. 获取目标路径 (旧的索引文件路径)
        std::string target_path = m_receiver.get_filename();

        // 2. 关闭接收者
        // 这一步在 Linux 上虽然不是强制的（因为 inode 机制允许重命名打开的文件），
        // 但为了逻辑清晰，我们应该先释放 m_receiver 对旧文件的句柄。
        m_receiver.close();

        // 3. 让源对象执行重命名
        // 这一步既改了磁盘上的名字，也改了 m_source 内部的 filename_
        m_source.rename_file(target_path);

        // 4. 移动资源
        // 此时 m_source 的 filename_ 已经是 target_path 了，直接 move 过去即可
        m_receiver = std::move(m_source);
    }

    static bool try_parse_record_count(const unsigned char* record_batch, size_t record_batch_len, uint32_t& out_count) {
        constexpr size_t kKafkaRecordBatchHeaderSize = 8 + 4 + 4 + 1 + 4 + 2 + 4 + 8 + 8 + 8 + 2 + 4 + 4;
        constexpr size_t kRecordCountOffset = kKafkaRecordBatchHeaderSize - 4;
        if (!record_batch || record_batch_len < kKafkaRecordBatchHeaderSize) {
            return false;
        }
        uint32_t net = 0;
        std::memcpy(&net, record_batch + kRecordCountOffset, sizeof(net));
        out_count = ntohl(net);
        return true;
    }

    static bool try_parse_msg_num(const unsigned char* payload_prefix, size_t prefix_len, uint32_t payload_len, uint64_t& out_msg_num) {
        if (!payload_prefix) return false;

        if (payload_len >= 61 && prefix_len >= 61 && prefix_len >= 17) {
            uint32_t batch_len_net = 0;
            std::memcpy(&batch_len_net, payload_prefix + 8, sizeof(batch_len_net));
            uint32_t batch_len = ntohl(batch_len_net);
            const bool magic_ok = payload_prefix[16] == 2;
            const bool len_ok = (static_cast<uint64_t>(batch_len) + 12ULL) == static_cast<uint64_t>(payload_len);
            if (magic_ok && len_ok) {
                uint32_t rc = 0;
                if (!try_parse_record_count(payload_prefix, prefix_len, rc)) return false;
                out_msg_num = rc;
                return true;
            }
        }

        if (payload_len >= 16 && prefix_len >= 16) {
            uint64_t msg_num_net = 0;
            std::memcpy(&msg_num_net, payload_prefix + sizeof(uint64_t), sizeof(uint64_t));
            out_msg_num = ntohll(msg_num_net);
            return true;
        }

        return false;
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
                unsigned char prefix[61];
                size_t prefix_len = payload_size < sizeof(prefix) ? payload_size : sizeof(prefix);
                ssize_t count_read_bytes = pread(log_file_fd, prefix, prefix_len, last_phypos + 12);
                if (count_read_bytes != static_cast<ssize_t>(prefix_len)) {
                    actual_log_data_end = last_phypos;
                    recovery_failed = true;
                    break;
                }
                if (!try_parse_msg_num(prefix, prefix_len, payload_size, current_batch_count) || current_batch_count == 0) {
                    std::cerr << "[Recover] Failed to parse msg_num at pos " << last_phypos << ". Stopping." << std::endl;
                    actual_log_data_end = last_phypos;
                    recovery_failed = true;
                    break;
                }
            } else {
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
            next_offset_ = base_offset_ + total_msg_count;

            // 替换索引文件
            try {
                replace_mmapfile_content(tmp_mmap, index_file_);
            } catch (const std::runtime_error& e) {
                std::cerr << "[Recover] Failed to swap index file: " << e.what() << std::endl;
                return false;
            }

            index_file_.set_curr_used_size(index_size_on_disk);

            committed_index_size_.store(index_file_.give_curr_used_size(), std::memory_order_release);
                    committed_file_size_.store(actual_log_data_end, std::memory_order_release);

                    // 同步内存中的文件大小记录
                    actual_physical_file_size = actual_log_data_end;
            // 刷盘
            flush_log();
            flush_index();



            return true;
        }
    }

    std::pair<uint64_t, Err> append(const std::pair<const unsigned char*, uint32_t>& msg_view) {
        // 1. 加互斥锁 (只阻塞其他 Writer，不阻塞 Reader)
        std::lock_guard<std::mutex> lock(writemutex_);

        // 2. 获取当前逻辑 Offset
        uint64_t current_offset = next_offset_;

        const unsigned char* payload = msg_view.first;
        const uint32_t payload_len = msg_view.second;
        const size_t total_log_entry_size = 12ULL + payload_len;

        // 3. 检查容量 (使用 Writer 自己的非原子变量判断即可)
        if (actual_physical_file_size + total_log_entry_size > max_segment_size_) {
            return {0, Err::FULL_SEGMENT};
        }

        uint64_t msg_num = 0;
        size_t prefix_len = payload_len < 61 ? payload_len : 61;
        if (!try_parse_msg_num(payload, prefix_len, payload_len, msg_num) || msg_num == 0 || payload_len < sizeof(uint64_t)) {
            return {0, Err::IO_ERROR};
        }

        // 记录写入前的物理位置用于索引
        uint32_t index_physical_pos = actual_physical_file_size;

        // -------------------------------------------------------
        // 5. 执行写入 (关键步骤)
        // -------------------------------------------------------
        uint64_t curr_off_net = htonll(current_offset);
        uint32_t payload_len_net = htonl(payload_len);
        std::vector<unsigned char> buf;
        buf.resize(total_log_entry_size);
        std::memcpy(buf.data(), &curr_off_net, sizeof(uint64_t));
        std::memcpy(buf.data() + sizeof(uint64_t), &payload_len_net, sizeof(uint32_t));
        std::memcpy(buf.data() + 12, payload, payload_len);
        std::memcpy(buf.data() + 12, &curr_off_net, sizeof(uint64_t));

        ssize_t written = 0;
        while (true) {
            written = ::write(log_file_fd, buf.data(), buf.size());
            if (written < 0 && errno == EINTR) continue;
            break;
        }

        if (written != static_cast<ssize_t>(buf.size())) {
            ftruncate(log_file_fd, actual_physical_file_size);
            return {0, Err::IO_ERROR};
        }

        // 更新 Writer 内部状态
        actual_physical_file_size += total_log_entry_size;
        log_bytes_since_last_flush += total_log_entry_size;

        // -------------------------------------------------------
        // 6. 索引逻辑
        // -------------------------------------------------------
        bool create_index_entry = (current_offset == base_offset_ ||
            actual_physical_file_size - bytes_last_index_entry_ >= index_build_interval_bytes);

        if (create_index_entry) {
            try {
                // 分配内存
                char* index_ptr = static_cast<char*>(index_file_.allocate(sizeof(uint32_t) * 2));

                uint32_t relative_offset = static_cast<uint32_t>(current_offset - base_offset_);
                uint32_t encoded_relative = htonl(relative_offset);
                uint32_t encoded_physical = htonl(index_physical_pos);

                // 写入索引数据 (内存操作)
                std::memcpy(index_ptr, &encoded_relative, sizeof(uint32_t));
                std::memcpy(index_ptr + sizeof(uint32_t), &encoded_physical, sizeof(uint32_t));

                bytes_last_index_entry_ = actual_physical_file_size;

                // 【关键点 A】: 发布索引更新
                // 使用 memory_order_release，保证上面的 memcpy 必须在这一步之前完成，
                // 防止 Reader 看到新的 size 但读到旧的内存。
                committed_index_size_.store(index_file_.give_curr_used_size(), std::memory_order_release);

            } catch (std::out_of_range& e) {
                cerr ("[Logsegment] Index Full");
            } catch (std::runtime_error& e) {
                cerr ("[Logsegment] Index mmap crushed");
            }
        }

        // -------------------------------------------------------
        // 7. 发布 Log 更新 (Commit Point)
        // -------------------------------------------------------
        // 这是最关键的一步。
        // 使用 release 语义，确保：
        // 1. 上面的 write() 系统调用已经返回。
        // 2. 索引的更新已经完成。
        // 此时 Reader 获取到的 committed_file_size_ 增大，才能安全读取新数据。
        committed_file_size_.store(actual_physical_file_size, std::memory_order_release);


        // 8. 刷盘逻辑 (根据策略决定)
        if (log_bytes_since_last_flush.load() >= LOG_FLUSH_BYTES_INTERVAL) {
            // flush_log();
            // 注意：如果你需要强一致性，必须 flush 后再 update committed_size，
            // 但通常为了性能，PageCache 可见性就足够了，操作系统保证 write 后 pread 可见。
        }

        // 更新逻辑 Offset (原子变量，本身就是原子的)
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


    void mark_as_clean_in_lock() {
        // 1. 强行落盘，确保数据完整
        flush_log();
        flush_index();

        // 2. 关闭 Log 文件描述符
        if (log_file_fd != -1) {
            ::close(log_file_fd);
            log_file_fd = -1;
        }

        // 3. 关闭 Index 的 mmap 映射
        // 这一步至关重要，否则 rename 后原来的 mmap 仍然指向旧 inode，
        // 且如果这时有野指针访问会造成严重后果。
        index_file_.close();

        // 4. 准备文件名
        std::string clean_log_name = log_filename + ".clean";
        std::string clean_index_name = index_path + ".clean";

        // 5. 执行重命名 (Log)
        if (std::rename(log_filename.c_str(), clean_log_name.c_str()) != 0) {
            std::cerr << "[LogSegment] Failed to rename log: " << log_filename
                      << " to " << clean_log_name << " error: " << strerror(errno) << std::endl;
            // 即使失败，最好也不要抛出异常中断流程，只是记录错误
        }

        // 6. 执行重命名 (Index)
        if (std::rename(index_path.c_str(), clean_index_name.c_str()) != 0) {
            std::cerr << "[LogSegment] Failed to rename index: " << index_path
                      << " to " << clean_index_name << " error: " << strerror(errno) << std::endl;
        }
    }


    MesLoc find(uint64_t target_offset, size_t byte_need) {
        // 【关键修改 1】不再加锁！
        MesLoc loc{}; // 默认 found=0

        // 【关键修改 2】获取原子快照 (Acquire 语义)
        // 配合 Writer 的 Release，确保读到的 size 之前的数据都已经落盘/写入内存
        size_t safe_log_limit = committed_file_size_.load(std::memory_order_acquire);
        size_t safe_index_limit = committed_index_size_.load(std::memory_order_acquire);

        // 1. 基础范围检查
        // next_offset_ 也是原子的，可以直接读
        if (target_offset >= next_offset_.load()) {
            return loc;
        }
        if (target_offset < base_offset_) {
            target_offset = base_offset_;
        }

        uint32_t relative_offset = static_cast<uint32_t>(target_offset - base_offset_);

        // index_file_ 预分配且地址固定，直接获取指针是安全的
        char* index_start = static_cast<char*>(index_file_.give_mapped_data_ptr());

        // 【关键修改 3】使用 safe_index_limit 而不是实时去问 mmap 对象
        size_t index_size = safe_index_limit;
        size_t num_entries = index_size / (sizeof(uint32_t) * 2);

        uint32_t phy_pos = 0;

        // 2. 二分查找 (Binary Search) - 纯内存操作，线程安全
        if (num_entries != 0) {
            int low = 0, high = static_cast<int>(num_entries - 1), found_idx = -1;
            while (low <= high) {
                int mid = low + (high - low) / 2;

                // 边界检查使用快照大小
                if ((size_t)mid * 8 + 4 > index_size) break;

                uint32_t current_relative;
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
                if ((size_t)found_idx * 8 + 8 <= index_size) {
                    std::memcpy(&phy_pos, index_start + found_idx * 8 + 4, 4);
                    phy_pos = ntohl(phy_pos);
                }
            }
        }

        // 3. 线性扫描 (Linear Scan)
        // 【关键修改 4】使用 safe_log_limit 作为循环上限
        size_t logsize = safe_log_limit;

        if (phy_pos >= logsize) return loc;

        size_t current_scan_pos = phy_pos;

        while (current_scan_pos < logsize) {
            // 读取 Log Header (12 Bytes)
            char header_buf[12];
            if (current_scan_pos + 12 > logsize) break;

            // pread 是系统调用，线程安全，不依赖文件指针
            ssize_t r = pread(log_file_fd, header_buf, 12, current_scan_pos);
            if (r < 12) break;

            uint64_t off_net;
            uint32_t len_net;
            std::memcpy(&off_net, header_buf, 8);
            std::memcpy(&len_net, header_buf + 8, 4);

            uint64_t batch_base_offset = ntohll(off_net);
            uint32_t batch_len = ntohl(len_net); // Payload 长度

            size_t total_msg_size = 12 + batch_len;
            if (current_scan_pos + total_msg_size > logsize) break;

            // --- 判定是否命中 ---
            bool is_target_batch = false;

            if (batch_base_offset >= target_offset) {
                is_target_batch = true;
            } else {
                // 检查 Target 是否在 Batch 内部
                if (batch_len >= 16) {
                    unsigned char prefix[61];
                    size_t prefix_len = batch_len < sizeof(prefix) ? batch_len : sizeof(prefix);
                    if (pread(log_file_fd, prefix, prefix_len, current_scan_pos + 12) == static_cast<ssize_t>(prefix_len)) {
                        uint64_t batch_count = 0;
                        if (try_parse_msg_num(prefix, prefix_len, batch_len, batch_count) && batch_count > 0) {
                            if (batch_base_offset + batch_count > target_offset) {
                                is_target_batch = true;
                            }
                        }
                    }
                }
            }

            if (is_target_batch) {
                loc.found = 1;
                loc.file_descriptor = log_file_fd;
                loc.offset_in_file = static_cast<off_t>(current_scan_pos + 12);
                loc.length = batch_len;
                uint64_t batch_count = 0;
                if (batch_len >= 16) {
                    unsigned char prefix[61];
                    size_t prefix_len = batch_len < sizeof(prefix) ? batch_len : sizeof(prefix);
                    if (pread(log_file_fd, prefix, prefix_len, current_scan_pos + 12) == static_cast<ssize_t>(prefix_len)) {
                        (void)try_parse_msg_num(prefix, prefix_len, batch_len, batch_count);
                    }
                }
                if (batch_count == 0) {
                    return MesLoc{};
                }
                loc.offset_next_to_consume = batch_base_offset + batch_count;
                return loc;
            }

            current_scan_pos += total_msg_size;
        }

        return loc;
    }

    std::vector<std::vector<unsigned char>> dump_payloads_snapshot() {
        std::vector<std::vector<unsigned char>> result;
        size_t logsize = committed_file_size_.load(std::memory_order_acquire);
        size_t pos = 0;
        while (pos + 12 <= logsize) {
            char header_buf[12];
            ssize_t r = pread(log_file_fd, header_buf, 12, pos);
            if (r < 12) break;
            uint32_t size_net;
            std::memcpy(&size_net, header_buf + 8, 4);
            uint32_t payload_len = ntohl(size_net);
            if (payload_len == 0) break;
            if (pos + 12ULL + payload_len > logsize) break;
            std::vector<unsigned char> payload(payload_len);
            ssize_t r2 = pread(log_file_fd, payload.data(), payload_len, pos + 12);
            if (r2 < static_cast<ssize_t>(payload_len)) break;
            result.emplace_back(std::move(payload));
            pos += 12ULL + payload_len;
        }
        return result;
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
        std::lock_guard<std::mutex> lockg(writemutex_);
        // 1. 先把 Atomic 设为 0，立刻阻断 lock-free Reader 读取后续内容
            // (Reader 可能还在读旧的一瞬间，但新的请求会被挡在 0 处)
            committed_file_size_.store(0, std::memory_order_release);
            committed_index_size_.store(0, std::memory_order_release);

            // 2. 重置 Writer 内部状态
            next_offset_ = base_offset_;
            log_bytes_since_last_flush.store(0);
            bytes_last_index_entry_ = 0;
            actual_physical_file_size = 0; // 【重要】重置物理大小计数

            index_file_.close();
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
    std::mutex writemutex_;
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

    std::atomic<uint32_t> committed_file_size_{0}; // 给 Reader 看的“已提交数据边界”


        std::atomic<size_t> committed_index_size_{0}; //给 Reader 看的“已提交索引边界”

};



#endif // LOGSEGMENT_H
