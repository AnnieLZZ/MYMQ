#ifndef MMAPFILE_H
#define MMAPFILE_H

#include <iostream>
#include <string>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <utility> // for std::swap
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <errno.h>

// 假设这是你项目中定义的头文件，包含了 MAX_ALLOWED_FILE_SIZE
#include <MYMQ_innercodes.h>

class Mmapfile {
public:
    // =============================================================
    // 1. 构造与析构
    // =============================================================

    // 【关键】删除默认构造函数，强制要求创建时必须绑定有效文件
    Mmapfile() = delete;

    // 标准构造函数
    Mmapfile(const std::string& filename, size_t init_size = 20 * 1024 * 1024)
        : filename_(filename), mapped_data_ptr(nullptr)
    {
        fd_ = ::open(filename.c_str(), O_RDWR | O_CREAT, 0644);
        if (fd_ == -1) {
            throw std::runtime_error("Failed to open file: " + filename + " - " + std::strerror(errno));
        }

        struct stat st;
        if (::fstat(fd_, &st) == -1) {
            ::close(fd_);
            throw std::runtime_error("Failed to get file status for " + filename + ": " + std::strerror(errno));
        }

        size_t recovered_curr_used_size = st.st_size;
        curr_used_size_.store(recovered_curr_used_size);

        // 确定文件需要映射和预留的容量大小
        size_t desired_capacity = std::max(recovered_curr_used_size, init_size);
        file_size_ = desired_capacity;

        // 如果容量大于当前实际文件大小，则扩容物理文件
        if (file_size_ > recovered_curr_used_size) {
            if (::ftruncate(fd_, file_size_) != 0) {
                ::close(fd_);
                throw std::runtime_error("Failed to ftruncate file to desired capacity " + std::to_string(file_size_) + ": " + std::strerror(errno));
            }
        }

        // 映射整个容量区域
        if (file_size_ > 0) {
            mapped_data_ptr = ::mmap(nullptr, file_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
            if (mapped_data_ptr == MAP_FAILED) {
                ::close(fd_);
                throw std::runtime_error("Failed to mmap file " + filename_ + " with initial size " + std::to_string(file_size_) + ": " + std::strerror(errno));
            }
        }
    }

    // 析构函数：确保资源释放
    ~Mmapfile() {
        close();
    }

    // =============================================================
    // 2. 禁止拷贝 (Copy Semantics)
    // =============================================================
    Mmapfile(const Mmapfile&) = delete;
    Mmapfile& operator=(const Mmapfile&) = delete;

    // =============================================================
    // 3. 移动语义 (Move Semantics) - 使用 Swap Idiom
    // =============================================================

    // 移动构造函数
    // 因为没有默认构造函数，我们在初始化列表中手动构造一个"空壳"状态，
    // 然后与 other 进行交换。
    Mmapfile(Mmapfile&& other) noexcept
        : filename_(""),
          fd_(-1),
          file_size_(0),
          mapped_data_ptr(nullptr)
    {
        curr_used_size_.store(0); // atomic 需要单独处理
        swap(other);
    }

    // 移动赋值运算符
    Mmapfile& operator=(Mmapfile&& other) noexcept {
        if (this != &other) {
            // 交换资源：
            // 1. 我原本持有的资源（如果有）被换到 other 里。
            // 2. 我拿到了 other 的资源。
            // 3. 函数结束时，other 析构，自动调用 close() 释放我原本的资源。
            swap(other);
        }
        return *this;
    }

    // 核心交换函数
    void swap(Mmapfile& other) noexcept {
        using std::swap;
        swap(filename_, other.filename_);
        swap(fd_, other.fd_);
        swap(file_size_, other.file_size_);
        swap(mapped_data_ptr, other.mapped_data_ptr);

        // std::atomic 不支持移动或交换，需要手动交换数值
        size_t temp = curr_used_size_.load();
        curr_used_size_.store(other.curr_used_size_.load());
        other.curr_used_size_.store(temp);
    }

    // =============================================================
    // 4. 核心功能接口
    // =============================================================

    // 显式关闭并释放资源
    void close() noexcept {
        // 1. 处理内存映射逻辑
        if (mapped_data_ptr != nullptr && mapped_data_ptr != MAP_FAILED) {
            if (fd_ != -1 && curr_used_size_.load() > 0) {
                // 确保数据同步到磁盘
                if (::msync(mapped_data_ptr, curr_used_size_.load(), MS_SYNC) != 0) {
                    std::cerr << "Mmapfile::close: ERROR: msync failed for '" << filename_ << "': " << std::strerror(errno) << std::endl;
                }
            }
            ::munmap(mapped_data_ptr, file_size_);
            mapped_data_ptr = nullptr;
        }

        // 2. 处理文件描述符与物理大小逻辑
        if (fd_ != -1) {
            size_t final_size = curr_used_size_.load();
            if (::ftruncate(fd_, final_size) != 0) {
                std::cerr << "Mmapfile::close: Warning: Failed to ftruncate '" << filename_ << "' to " << final_size << ": " << std::strerror(errno) << std::endl;
            }
            ::close(fd_);
            fd_ = -1; // 标记为已关闭
        }

        // 3. 重置状态
        file_size_ = 0;
        curr_used_size_.store(0);
        // filename_ 保留，因为它是对象标识的一部分，即使文件关闭了
    }

    char* allocate(size_t length) {
        // 防御性检查：防止对已移动或已关闭的对象操作
        if (mapped_data_ptr == nullptr || fd_ == -1) {
            throw std::runtime_error("Mmapfile is not mapped, closed, or moved-from.");
        }

        size_t start_offset = curr_used_size_.load();
        size_t new_used_size = start_offset + length;

        if (new_used_size > file_size_) {
            throw std::out_of_range("Insufficient capacity: " + std::to_string(file_size_) + " < " + std::to_string(new_used_size));
        }

        char* ptr = static_cast<char*>(mapped_data_ptr) + start_offset;
        curr_used_size_.store(new_used_size);
        return ptr;
    }

    // 原子重命名文件，并更新内部状态
    void rename_file(const std::string& new_path) {
        if (fd_ == -1) {
             throw std::runtime_error("Cannot rename a closed or moved-from file.");
        }

        flush_sync(); // 重命名其必须刷盘保证数据一致性

        if (::rename(filename_.c_str(), new_path.c_str()) != 0) {
            throw std::runtime_error("Failed to rename file from '" + filename_ +
                                     "' to '" + new_path + "': " + std::strerror(errno));
        }

        filename_ = new_path;
    }

    // =============================================================
    // 5. 辅助功能接口
    // =============================================================

    void* give_mapped_data_ptr() const { return mapped_data_ptr; }
    size_t give_curr_used_size() const { return curr_used_size_.load(); }
    int get_fd() const { return fd_; }
    std::string get_filename() const { return filename_; }

    void flush_async() {
        if (mapped_data_ptr && fd_ != -1 && curr_used_size_.load() > 0) {
            ::msync(mapped_data_ptr, curr_used_size_.load(), MS_ASYNC);
        }
    }

    void flush_sync() {
        if (mapped_data_ptr && fd_ != -1 && curr_used_size_.load() > 0) {
            ::msync(mapped_data_ptr, curr_used_size_.load(), MS_SYNC);
        }
    }

    void set_curr_used_size(size_t offset) {
        if (offset > file_size_) throw std::out_of_range("Offset exceeds capacity.");
        curr_used_size_.store(offset);
    }

    void truncate_physical_file_to_curr_used_size() {
        if (fd_ != -1) {
            ::ftruncate(fd_, curr_used_size_.load());
        }
    }

    bool is_open() const noexcept {
        return fd_ != -1;
    }

private:
    void remap(size_t new_size) {
        const size_t MAX_ALLOWED_FILE_SIZE = MYMQ::MAX_ALLOWED_FILE_SIZE;

        if (new_size > MAX_ALLOWED_FILE_SIZE) {
            throw std::runtime_error("Remap exceeds max allowed size.");
        }

        if (mapped_data_ptr && fd_ != -1) {
            ::msync(mapped_data_ptr, curr_used_size_.load(), MS_SYNC);
            ::munmap(mapped_data_ptr, file_size_);
        }

        if (::ftruncate(fd_, new_size) != 0) {
            throw std::runtime_error("Failed to ftruncate for remap.");
        }

        if (new_size > 0) {
            mapped_data_ptr = ::mmap(nullptr, new_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
            if (mapped_data_ptr == MAP_FAILED) {
                throw std::runtime_error("Failed to mmap after remap.");
            }
        } else {
            mapped_data_ptr = nullptr;
        }
        file_size_ = new_size;
    }

    std::string filename_;
    int fd_{-1};
    size_t file_size_{0};
    std::atomic<size_t> curr_used_size_{0};
    void* mapped_data_ptr{nullptr};
};

#endif // MMAPFILE_H
