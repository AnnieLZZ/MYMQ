#ifndef MMAPFILE_H
#define MMAPFILE_H

#include <iostream>
#include <string>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <MYMQ_innercodes.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <errno.h>

class Mmapfile {
public:
    Mmapfile(const std::string& filename, size_t init_size = 20 * 1024 * 1024)
        : filename_(filename), mapped_data_ptr(nullptr)
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

        size_t recovered_curr_used_size = st.st_size;

        // 设置实际已使用的大小
        curr_used_size_.store(recovered_curr_used_size);

        // 2. 确定文件需要映射和预留的容量大小 (capacity)
        size_t desired_capacity = std::max(curr_used_size_.load(), init_size);
        file_size_ = desired_capacity; // 设置容量

        // 3. 如果容量大于当前实际文件大小，则将物理文件扩容到 desired_capacity
        if (file_size_ > recovered_curr_used_size) {
            if (ftruncate(fd_, file_size_) != 0) {
                close(fd_);
                throw std::runtime_error("Failed to ftruncate file to desired capacity " + std::to_string(file_size_) + ": " + std::strerror(errno));
            }
        }

        // 4. 映射整个容量区域
        if (file_size_ > 0) {
            mapped_data_ptr = mmap(nullptr, file_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
            if (mapped_data_ptr == MAP_FAILED) {
                close(fd_);
                throw std::runtime_error("Failed to mmap file " + filename_ + " with initial size " + std::to_string(file_size_) + ": " + std::strerror(errno));
            }
        } else {
            mapped_data_ptr = nullptr;
        }
    }

    Mmapfile(const Mmapfile&) = delete;
    Mmapfile& operator=(const Mmapfile&) = delete;

    // 移动构造函数
    Mmapfile(Mmapfile&& other) noexcept
        : filename_(std::move(other.filename_)),
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

    // 移动赋值运算符
    Mmapfile& operator=(Mmapfile&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        // 清理当前对象资源
        if (mapped_data_ptr != MAP_FAILED && mapped_data_ptr != nullptr) {
            // 在 munmap 前同步数据
            if (curr_used_size_.load() > 0 && fd_ != -1) {
                if (msync(mapped_data_ptr, curr_used_size_.load(), MS_SYNC) != 0) {
                    std::cerr << "Mmapfile::operator=: ERROR: msync data failed for '" << filename_ << "': " << std::strerror(errno) << std::endl;
                }
            }
            munmap(mapped_data_ptr, file_size_);
        }
        if (fd_ != -1) {
            // 截断到实际使用大小
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

        // 使 other 对象无效
        other.fd_ = -1;
        other.file_size_ = 0;
        other.curr_used_size_.store(0);
        other.mapped_data_ptr = nullptr;
        other.filename_ = "";

        return *this;
    }

    // 析构函数
    ~Mmapfile() {
        if (mapped_data_ptr != MAP_FAILED && mapped_data_ptr != nullptr) {
            if (fd_ != -1 && curr_used_size_.load() > 0) {
                if (msync(mapped_data_ptr, curr_used_size_.load(), MS_SYNC) != 0) {
                    std::cerr << "Mmapfile::Destructor: ERROR: msync data failed for '" << filename_ << "': " << std::strerror(errno) << std::endl;
                }
            }
            munmap(mapped_data_ptr, file_size_);
            mapped_data_ptr = nullptr;
        }
        if (fd_ != -1) {
            size_t final_size_to_truncate = curr_used_size_.load();
            if (ftruncate(fd_, final_size_to_truncate) != 0) {
                std::cerr << "Warning: Failed to ftruncate file " << filename_ << " to "
                          << final_size_to_truncate << " bytes on close: " << std::strerror(errno) << std::endl;
            }
            close(fd_);
            fd_ = -1;
        }
    }

    char* allocate(size_t length) {

        size_t start_offset = curr_used_size_.load();
        size_t new_used_size = start_offset + length;

        if (new_used_size > file_size_) {
            throw std::out_of_range("Insufficient capacity to allocate " + std::to_string(length) +
                                    " bytes. Current file capacity: " + std::to_string(file_size_) +
                                    ", requested total size: " + std::to_string(new_used_size) + ".");
        }

        if (mapped_data_ptr == nullptr) {
            throw std::runtime_error("Mmapfile is not mapped or in an invalid state for allocation.");
        }

        char* ptr = static_cast<char*>(mapped_data_ptr) + start_offset;
        curr_used_size_.store(new_used_size); // 更新实际使用大小
        // 移除了 update_header_curr_used_size()
        return ptr;
    }

    void* give_mapped_data_ptr() const {
        if (mapped_data_ptr == nullptr) return nullptr;
        return mapped_data_ptr;
    }

    size_t give_curr_used_size() const { return curr_used_size_.load(); }

    void flush_async() {
        if (mapped_data_ptr != MAP_FAILED && mapped_data_ptr != nullptr && fd_ != -1) {
            if (curr_used_size_.load() > 0) {
                if (msync(mapped_data_ptr, curr_used_size_.load(), MS_ASYNC) != 0) {
                    std::cerr << "Mmapfile::flush_async: ERROR: msync data failed for '" << filename_ << "': " << std::strerror(errno) << std::endl;
                }
            }
        }
    }

    void flush_sync() {
        if (mapped_data_ptr != MAP_FAILED && mapped_data_ptr != nullptr && fd_ != -1) {
            if (curr_used_size_.load() > 0) {
                if (msync(mapped_data_ptr, curr_used_size_.load(), MS_SYNC) != 0) {
                    std::cerr << "Mmapfile::flush_sync: ERROR: msync data failed for '" << filename_ << "': " << std::strerror(errno) << std::endl;
                }
            }
        }
    }

    void set_curr_used_size(size_t offset) {
        if (offset > file_size_) {
            throw std::out_of_range("Attempted to set curr_used_size_ beyond current file_size_ capacity.");
        }
        curr_used_size_.store(offset);
    }

    int get_fd() const { return fd_; }

    void reset() {
        if (mapped_data_ptr != MAP_FAILED && mapped_data_ptr != nullptr) {
            if (curr_used_size_.load() > 0 && fd_ != -1) {
                if (msync(mapped_data_ptr, curr_used_size_.load(), MS_SYNC) != 0) {
                    std::cerr << "Mmapfile::reset: ERROR: msync data failed for '" << filename_ << "': " << std::strerror(errno) << std::endl;
                }
            }
            if (munmap(mapped_data_ptr, file_size_) != 0) {
                throw std::runtime_error("Failed to munmap during reset for " + filename_ + ": " + std::strerror(errno));
            }
            mapped_data_ptr = nullptr;
        }

        if (fd_ != -1) {
            size_t final_size_to_truncate = curr_used_size_.load();
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
            if (ftruncate(fd_, target_size) != 0) {
                std::cerr << "Warning: Failed to ftruncate physical file '" << filename_ << "' to "
                          << target_size << " bytes: " << std::strerror(errno) << std::endl;
            }
        }
    }

    std::string get_filename() const { return filename_; }

    void take_ownership_of_internal(Mmapfile&& other) noexcept {
        if (this == &other) { return; }

        reset();

        fd_ = other.fd_;
        file_size_ = other.file_size_;
        curr_used_size_.store(other.curr_used_size_.load());
        mapped_data_ptr = other.mapped_data_ptr;
        filename_ = std::move(other.filename_);

        other.fd_ = -1;
        other.file_size_ = 0;
        other.curr_used_size_.store(0);
        other.mapped_data_ptr = nullptr;
        other.filename_ = "";
    }

private:
    void remap(size_t new_size) {
        const size_t MAX_ALLOWED_FILE_SIZE = MYMQ::MAX_ALLOWED_FILE_SIZE;

        if (new_size > MAX_ALLOWED_FILE_SIZE) {
            throw std::runtime_error("Attempted to remap file beyond maximum allowed size (" +
                                     std::to_string(MAX_ALLOWED_FILE_SIZE / (1024.0 * 1024.0 * 1024.0)) + " GB). Current requested size: " +
                                     std::to_string(new_size / (1024.0 * 1024.0 * 1024.0)) + " GB.");
        }

        // 在 munmap 前同步当前数据
        if (mapped_data_ptr != MAP_FAILED && mapped_data_ptr != nullptr && fd_ != -1) {
            if (curr_used_size_.load() > 0) {
                if (msync(mapped_data_ptr, curr_used_size_.load(), MS_SYNC) != 0) {
                    std::cerr << "Mmapfile::remap: ERROR: msync data failed for '" << filename_ << "': " << std::strerror(errno) << std::endl;
                }
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

    }

    std::string filename_;
    int fd_{-1};
    size_t file_size_;
    std::atomic<size_t> curr_used_size_{0};
    void* mapped_data_ptr{nullptr};
};
#endif // MMAPFILE_H
