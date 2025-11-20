#ifndef MMAPFILE_H
#define MMAPFILE_H
#include <iostream>      // std::cerr, std::endl
#include <string>        // std::string, std::to_string, std::move
#include <cstring>       // std::strerror
#include <stdexcept>     // std::runtime_error, std::out_of_range
#include <algorithm>     // std::min, std::max
#include <atomic>        // std::atomic
#include <cstdio>       // 已经包含，但现代C++中通常用 <cstdio>
#include <MYMQ_innercodes.h> // 用户自定义
#include <fcntl.h>       // open, O_RDWR, O_CREAT
#include <unistd.h>      // close, ftruncate
#include <sys/stat.h>    // fstat, struct stat
#include <sys/mman.h>    // mmap, munmap, msync, MAP_FAILED, PROT_*, MAP_*
#include <errno.h>       // errno
#include <string>
#include <stdexcept>
#include <cstring> // For std::strerror
#include <iostream>
#include <algorithm>
#include <atomic>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

class Mmapfile {
public:
    // 将 MMAP_HEADER_SIZE 移到构造函数外，并在私有成员中初始化，
    // 以保持类结构一致性。
    // 注意：在实际项目中，最好使用 static constexpr。
    Mmapfile(const std::string& filename, size_t init_size = 20 * 1024 * 1024)
        : filename_(filename), MMAP_HEADER_SIZE(MYMQ::MMAP_HEADER_SIZE), mapped_data_ptr(nullptr)
    {
        fd_ = open(filename.c_str(), O_RDWR | O_CREAT, 0644);
        if (fd_ == -1) {
            throw std::runtime_error("Failed to open file: " + filename + " - " + std::strerror(errno));
        }

        struct stat st;
        if (fstat(fd_, &st) == -1) {
            close(fd_);
            throw std::runtime_error("Failed to get file status for " + filename + ": " + std::strerror(errno));
        }

        size_t actual_physical_file_size = st.st_size;
        size_t recovered_curr_used_size = 0;

        // 1. 尝试读取文件头部以恢复 curr_used_size_
        void* temp_mapped_ptr = nullptr;
        if (actual_physical_file_size >= MMAP_HEADER_SIZE) {
            temp_mapped_ptr = mmap(nullptr, MMAP_HEADER_SIZE, PROT_READ, MAP_SHARED, fd_, 0);
            if (temp_mapped_ptr == MAP_FAILED) {
                close(fd_);
                throw std::runtime_error("Failed to mmap header for " + filename_ + ": " + std::strerror(errno));
            }
            // 从头部读取持久化的 curr_used_size_
            recovered_curr_used_size = *static_cast<size_t*>(temp_mapped_ptr);
            munmap(temp_mapped_ptr, MMAP_HEADER_SIZE); // 解除临时映射
        }

        // 确保恢复的 curr_used_size_ 不会超过文件实际物理大小
        recovered_curr_used_size = std::min(recovered_curr_used_size, actual_physical_file_size);
        // 确保数据区域从 HEADER_SIZE 之后开始（至少保留头部空间）
        if (recovered_curr_used_size < MMAP_HEADER_SIZE) {
            recovered_curr_used_size = MMAP_HEADER_SIZE;
        }

        // 2. 将文件物理截断到恢复的 curr_used_size_，满足崩溃恢复时文件大小的保证
        if (ftruncate(fd_, recovered_curr_used_size) != 0) {
            close(fd_);
            throw std::runtime_error("Failed to ftruncate file to recovered size " + std::to_string(recovered_curr_used_size) + ": " + std::strerror(errno));
        }
        curr_used_size_.store(recovered_curr_used_size); // 设置实际已使用的大小

        // 3. 确定文件需要映射和预留的容量大小 (capacity)
        size_t desired_capacity = std::max(curr_used_size_.load(), init_size);
        // 确保容量至少能容纳头部
        if (desired_capacity < MMAP_HEADER_SIZE) {
            desired_capacity = MMAP_HEADER_SIZE;
        }
        file_size_ = desired_capacity; // 设置容量

        // 4. 如果容量大于当前实际数据大小，则将物理文件扩容到 desired_capacity
        if (file_size_ > curr_used_size_.load()) {
            if (ftruncate(fd_, file_size_) != 0) {
                close(fd_);
                throw std::runtime_error("Failed to ftruncate file to desired capacity " + std::to_string(file_size_) + ": " + std::strerror(errno));
            }
        }

        // 5. 映射整个容量区域
        if (file_size_ > 0) {
            mapped_data_ptr = mmap(nullptr, file_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
            if (mapped_data_ptr == MAP_FAILED) {
                close(fd_);
                throw std::runtime_error("Failed to mmap file " + filename_ + " with initial size " + std::to_string(file_size_) + ": " + std::strerror(errno));
            }
        } else {
            mapped_data_ptr = nullptr;
        }

        // 6. 确保头部本身已初始化（如果是新文件或恢复大小为0）
        if (mapped_data_ptr != nullptr) {
            update_header_curr_used_size(); // 写入当前的 curr_used_size_ 到头部
        }
    }

    Mmapfile(const Mmapfile&) = delete;
    Mmapfile& operator=(const Mmapfile&) = delete;

    // 移动构造函数 (与原版逻辑一致)
    Mmapfile(Mmapfile&& other) noexcept
        : filename_(std::move(other.filename_)),
          MMAP_HEADER_SIZE(other.MMAP_HEADER_SIZE),
          fd_(other.fd_),
          file_size_(other.file_size_),
          mapped_data_ptr(other.mapped_data_ptr)
    {
        curr_used_size_.store(other.curr_used_size_.load());
        other.fd_ = -1;
        other.file_size_ = 0;
        other.curr_used_size_.store(0);
        other.mapped_data_ptr = nullptr;
        other.filename_ = "";
    }

    // 移动赋值运算符 (与原版逻辑一致)
    Mmapfile& operator=(Mmapfile&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        // 清理当前对象资源，并截断文件到实际使用大小
        if (mapped_data_ptr != MAP_FAILED && mapped_data_ptr != nullptr) {
            // 在 munmap 前同步数据和头部
            // 注意：这里 curr_used_size_.load() 是当前对象的
            if (curr_used_size_.load() > 0 && fd_ != -1) {
                if (msync(mapped_data_ptr, curr_used_size_.load(), MS_SYNC) != 0) {
                    std::cerr << "Mmapfile::operator=: ERROR: msync data failed for '" << filename_ << "': " << std::strerror(errno) << std::endl;
                }
            }
            if (msync(mapped_data_ptr, MMAP_HEADER_SIZE, MS_SYNC) != 0) { // 同步头部
                std::cerr << "Mmapfile::operator=: ERROR: msync header failed for '" << filename_ << "': " << std::strerror(errno) << std::endl;
            }
            munmap(mapped_data_ptr, file_size_);
        }
        if (fd_ != -1) {
            // 在移动赋值前，将文件截断到实际使用的大小，释放预留但未使用的空间
            if (ftruncate(fd_, curr_used_size_.load()) != 0) {
                std::cerr << "Warning: Failed to ftruncate file " << filename_ << " to "
                          << curr_used_size_.load() << " bytes on move assignment: " << std::strerror(errno) << std::endl;
            }
            close(fd_);
        }

        // 从 other 移动资源
        filename_ = std::move(other.filename_);
        fd_ = other.fd_;
        file_size_ = other.file_size_;
        curr_used_size_.store(other.curr_used_size_.load());
        mapped_data_ptr = other.mapped_data_ptr;
        // MMAP_HEADER_SIZE 是 const，不能移动赋值，但可以确保 other 的被忽略

        // 使 other 对象无效
        other.fd_ = -1;
        other.file_size_ = 0;
        other.curr_used_size_.store(0);
        other.mapped_data_ptr = nullptr;
        other.filename_ = "";

        return *this;
    }

    // 析构函数 (与原版逻辑一致)
    ~Mmapfile() {
        if (mapped_data_ptr != MAP_FAILED && mapped_data_ptr != nullptr) {
            // 在 munmap 前同步数据和头部
            if (fd_ != -1) { // 只有 fd 有效时才尝试 msync
                if (curr_used_size_.load() > 0) {
                    if (msync(mapped_data_ptr, curr_used_size_.load(), MS_SYNC) != 0) {
                        std::cerr << "Mmapfile::Destructor: ERROR: msync data failed for '" << filename_ << "': " << std::strerror(errno) << std::endl;
                    }
                }
                if (msync(mapped_data_ptr, MMAP_HEADER_SIZE, MS_SYNC) != 0) { // 同步头部
                    std::cerr << "Mmapfile::Destructor: ERROR: msync header failed for '" << filename_ << "': " << std::strerror(errno) << std::endl;
                }
            }
            munmap(mapped_data_ptr, file_size_); // munmap 整个映射区域
            mapped_data_ptr = nullptr;
        }
        if (fd_ != -1) {
            // 析构时，将文件截断到实际使用的大小，释放预留但未使用的空间
            size_t final_size_to_truncate = curr_used_size_.load();
            if (ftruncate(fd_, final_size_to_truncate) != 0) {
                std::cerr << "Warning: Failed to ftruncate file " << filename_ << " to "
                          << final_size_to_truncate << " bytes on close: " << std::strerror(errno) << std::endl;
            }
            close(fd_);
            fd_ = -1;
        }
    }

    // *** 关键修改点：移除对齐逻辑 ***
    char* allocate(size_t length) {
        // 紧密排列，起始偏移直接使用当前的 curr_used_size_
        size_t start_offset = curr_used_size_.load();

        // 确保分配从 HEADER_SIZE 之后开始
        if (start_offset < MMAP_HEADER_SIZE) {
            // 这一步是为了确保分配的第一个数据块在头部之后。
            // 理论上，在构造函数和 set_curr_used_size 中已经保证了 curr_used_size_ >= MMAP_HEADER_SIZE，
            // 除非用户通过某种方式将其重置。此处作为二次检查。
            start_offset = MMAP_HEADER_SIZE;
        }

        size_t new_used_size = start_offset + length;

        if (new_used_size > file_size_) {
            // 这里可以添加自动扩容（remap）的逻辑，如果需要的话。
            // 当前版本只是抛出异常
            throw std::out_of_range("Insufficient capacity to allocate " + std::to_string(length) +
                                    " bytes. Current file capacity: " + std::to_string(file_size_) +
                                    ", requested total size: " + std::to_string(new_used_size) + ".");
        }

        if (mapped_data_ptr == nullptr) {
            throw std::runtime_error("Mmapfile is not mapped or in an invalid state for allocation.");
        }

        char* ptr = static_cast<char*>(mapped_data_ptr) + start_offset;
        curr_used_size_.store(new_used_size); // 更新实际使用大小
        update_header_curr_used_size(); // 持久化新的 curr_used_size_
        return ptr;
    }

    // 返回指向数据区域起始的指针 (跳过头部)
    void* give_mapped_data_ptr() const {
        if (mapped_data_ptr == nullptr) return nullptr;
        return static_cast<char*>(mapped_data_ptr) + MMAP_HEADER_SIZE;
    }

    // 以下方法（flush_async, flush_sync, set_curr_used_size, get_fd, reset,
    // truncate_physical_file_to_curr_used_size, get_filename, take_ownership_of_internal）
    // 与原版逻辑一致，不再赘述。

    size_t give_curr_used_size() const { return curr_used_size_.load(); }

    void flush_async() {
        if (mapped_data_ptr != MAP_FAILED && mapped_data_ptr != nullptr && fd_ != -1) {
            // 仅同步实际使用的部分
            if (curr_used_size_.load() > 0) {
                if (msync(mapped_data_ptr, curr_used_size_.load(), MS_ASYNC) != 0) {
                    std::cerr << "Mmapfile::flush_async: ERROR: msync data failed for '" << filename_ << "': " << std::strerror(errno) << std::endl;
                }
            }
            // 同步头部
            if (msync(mapped_data_ptr, MMAP_HEADER_SIZE, MS_ASYNC) != 0) {
                std::cerr << "Mmapfile::flush_async: ERROR: msync header failed for '" << filename_ << "': " << std::strerror(errno) << std::endl;
            }
        }
    }

    void flush_sync() {
        if (mapped_data_ptr != MAP_FAILED && mapped_data_ptr != nullptr && fd_ != -1) {
            // 仅同步实际使用的部分
            if (curr_used_size_.load() > 0) {
                if (msync(mapped_data_ptr, curr_used_size_.load(), MS_SYNC) != 0) {
                    std::cerr << "Mmapfile::flush_sync: ERROR: msync data failed for '" << filename_ << "': " << std::strerror(errno) << std::endl;
                }
            }
            // 同步头部
            if (msync(mapped_data_ptr, MMAP_HEADER_SIZE, MS_SYNC) != 0) {
                std::cerr << "Mmapfile::flush_sync: ERROR: msync header failed for '" << filename_ << "': " << std::strerror(errno) << std::endl;
            }
        }
    }

    void set_curr_used_size(size_t offset) {
        // 允许设置 curr_used_size_，但不能超过当前容量 file_size_
        // 且不能小于 HEADER_SIZE
        if (offset > file_size_) {
            throw std::out_of_range("Attempted to set curr_used_size_ beyond current file_size_ capacity.");
        }
        if (offset < MMAP_HEADER_SIZE && offset != 0) { // 如果设置为0，表示清空，可以小于HEADER_SIZE
            throw std::out_of_range("Attempted to set curr_used_size_ to a value less than HEADER_SIZE (unless it's 0).");
        }
        curr_used_size_.store(offset);
        update_header_curr_used_size(); // 持久化新的 curr_used_size_
    }

    int get_fd() const { return fd_; }

    void reset() {
        if (mapped_data_ptr != MAP_FAILED && mapped_data_ptr != nullptr) {
            // 在 munmap 前同步数据和头部
            if (curr_used_size_.load() > 0 && fd_ != -1) {
                if (msync(mapped_data_ptr, curr_used_size_.load(), MS_SYNC) != 0) {
                    std::cerr << "Mmapfile::reset: ERROR: msync data failed for '" << filename_ << "': " << std::strerror(errno) << std::endl;
                }
            }
            if (fd_ != -1) { // 只有 fd 有效时才尝试 msync 头部
                if (msync(mapped_data_ptr, MMAP_HEADER_SIZE, MS_SYNC) != 0) {
                    std::cerr << "Mmapfile::reset: ERROR: msync header failed for '" << filename_ << "': " << std::strerror(errno) << std::endl;
                }
            }
            if (munmap(mapped_data_ptr, file_size_) != 0) {
                throw std::runtime_error("Failed to munmap during reset for " + filename_ + ": " + std::strerror(errno));
            }
            mapped_data_ptr = nullptr;
        }

        if (fd_ != -1) {
            // 在关闭文件前截断到实际使用大小
            size_t final_size_to_truncate = curr_used_size_.load();
            if (final_size_to_truncate < MMAP_HEADER_SIZE) { // 确保至少保留头部空间
                final_size_to_truncate = MMAP_HEADER_SIZE;
            }
            if (ftruncate(fd_, final_size_to_truncate) != 0) {
                std::cerr << "Warning: Failed to ftruncate file " << filename_ << " to "
                          << final_size_to_truncate << " bytes during reset: " << std::strerror(errno) << std::endl;
            }
            if (close(fd_) != 0) {
                throw std::runtime_error("Failed to close file descriptor during reset for " + filename_ + ": " + std::strerror(errno));
            }
            fd_ = -1;
        }

        file_size_ = 0;
        curr_used_size_.store(0);
    }

    void truncate_physical_file_to_curr_used_size() {
        if (fd_ != -1) {
            size_t target_size = curr_used_size_.load();
            if (target_size < MMAP_HEADER_SIZE) { // 确保至少保留头部空间
                target_size = MMAP_HEADER_SIZE;
            }
            if (ftruncate(fd_, target_size) != 0) {
                std::cerr << "Warning: Failed to ftruncate physical file '" << filename_ << "' to "
                          << target_size << " bytes: " << std::strerror(errno) << std::endl;
            }
        }
    }

    std::string get_filename() const { return filename_; }

    void take_ownership_of_internal(Mmapfile&& other) noexcept {
        if (this == &other) { return; }

        // 首先，重置当前对象的所有资源，包括截断其文件
        reset();

        fd_ = other.fd_;
        file_size_ = other.file_size_;
        curr_used_size_.store(other.curr_used_size_.load());
        mapped_data_ptr = other.mapped_data_ptr;
        filename_ = std::move(other.filename_);
        // MMAP_HEADER_SIZE 是 const，此处无需处理

        // 使 other 对象无效，防止双重释放
        other.fd_ = -1;
        other.file_size_ = 0;
        other.curr_used_size_.store(0);
        other.mapped_data_ptr = nullptr;
        other.filename_ = "";
    }


private:
    // 持久化 curr_used_size_ 到文件头部
    void update_header_curr_used_size() {
        if (mapped_data_ptr != nullptr && fd_ != -1) {
            // 写入当前的 curr_used_size_ 到映射内存的头部
            *static_cast<size_t*>(mapped_data_ptr) = curr_used_size_.load();
            // 立即同步头部区域到磁盘，确保崩溃恢复的持久性
            if (msync(mapped_data_ptr, MMAP_HEADER_SIZE, MS_SYNC) != 0) {
                std::cerr << "Mmapfile::update_header_curr_used_size: ERROR: msync header failed for '" << filename_ << "': " << std::strerror(errno) << std::endl;
            }
        }
    }

    void remap(size_t new_size) {
        const size_t MAX_ALLOWED_FILE_SIZE = MYMQ::MAX_ALLOWED_FILE_SIZE;

        if (new_size > MAX_ALLOWED_FILE_SIZE) {
            throw std::runtime_error("Attempted to remap file beyond maximum allowed size (" +
                                     std::to_string(MAX_ALLOWED_FILE_SIZE / (1024.0 * 1024.0 * 1024.0)) + " GB). Current requested size: " +
                                     std::to_string(new_size / (1024.0 * 1024.0 * 1024.0)) + " GB.");
        }
        // 确保新容量至少能容纳头部
        if (new_size < MMAP_HEADER_SIZE) {
            new_size = MMAP_HEADER_SIZE;
        }

        // 在 munmap 前同步当前数据和头部
        if (mapped_data_ptr != MAP_FAILED && mapped_data_ptr != nullptr && fd_ != -1) {
            if (curr_used_size_.load() > 0) {
                if (msync(mapped_data_ptr, curr_used_size_.load(), MS_SYNC) != 0) { // 同步实际数据
                    std::cerr << "Mmapfile::remap: ERROR: msync data failed for '" << filename_ << "': " << std::strerror(errno) << std::endl;
                }
            }
            if (msync(mapped_data_ptr, MMAP_HEADER_SIZE, MS_SYNC) != 0) { // 同步头部
                std::cerr << "Mmapfile::remap: ERROR: msync header failed for '" << filename_ << "': " << std::strerror(errno) << std::endl;
            }
        }

        if (mapped_data_ptr != MAP_FAILED && mapped_data_ptr != nullptr) {
            munmap(mapped_data_ptr, file_size_); // 解除旧的映射
        }

        // 扩容物理文件
        if (ftruncate(fd_, new_size) != 0) {
            throw std::runtime_error("Failed to ftruncate file to new size: " + std::to_string(new_size) + " - " + std::string(std::strerror(errno)));
        }

        if (new_size > 0) {
            // 重新映射新大小的区域
            mapped_data_ptr = mmap(nullptr, new_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
            if (mapped_data_ptr == MAP_FAILED) {
                throw std::runtime_error("Failed to mmap file after remap: " + std::string(std::strerror(errno)));
            }
        } else {
            mapped_data_ptr = nullptr;
        }
        file_size_ = new_size; // 更新内部记录的容量

        // 重新映射后，确保头部在新的映射中也正确设置
        update_header_curr_used_size();
    }


    std::string filename_;
    // 将 MMAP_HEADER_SIZE 变为 const 成员，并保持它与 MYMQ::MMAP_HEADER_SIZE 一致
    const size_t MMAP_HEADER_SIZE;
    int fd_{-1};
    size_t file_size_; // 文件的当前容量 (capacity)，对应 mmap 的大小
    std::atomic<size_t> curr_used_size_{0}; // 文件中实际已使用的数据大小 (size)，对应 std::vector::size()
    void* mapped_data_ptr{nullptr};
};
#endif // MMAPFILE_H
