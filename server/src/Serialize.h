#ifndef SERIALIZE_H
#define SERIALIZE_H

#include <vector>
#include <string>
#include <cstdint>
#include <stdexcept>
#include <algorithm>
#include <iostream>
#include <limits>
#include <chrono>
#include<cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include"zstd.h"
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "zstd_static.lib")
#else
#include <arpa/inet.h>
#include <byteswap.h>

// 原始代码中的 htonll/ntohll 宏，它们没有被 MessageBuilder::hton64 和 MessageParser::ntoh64 使用
// 这两个自定义函数通过运行时检查字节序和 htonl/ntohl 实现 64 位转换。
// 因此，这些宏可以保留，但它们实际上并未被当前代码中的 64 位转换函数使用。
#ifndef htonll
#if __BYTE_ORDER == __LITTLE_ENDIAN
#define htonll(x) __bswap_64(x)
#define ntohll(x) __bswap_64(x)
#else
#define htonll(x) (x)
#define ntohll(x) (x)
#endif
#endif
#endif


class MessageBuilder {
public:
    std::vector<unsigned char> data;

    std::string dump() const {
        return std::string(data.begin(), data.end());
    }

    void append(){
    }

    template <typename T, typename ... Args>
    void append(T first_arg, Args ... rest_args){
        _append_single(first_arg);
        append(rest_args ...);
    }

    void reserve(size_t size){
        if(size <= data.size()){
            return ;
        }
        data.reserve(size);
    }

    // --- 整数追加方法 ---
    void append_short(short value) {
        uint16_t network_value = htons(static_cast<uint16_t>(value));
        append_bytes(&network_value, sizeof(network_value));
    }

    void append_int(int value) {
        uint32_t network_value = htonl(static_cast<uint32_t>(value));
        append_bytes(&network_value, sizeof(network_value));
    }

    void append_uint32(uint32_t value) {
        uint32_t network_value = htonl(value);
        append_bytes(&network_value, sizeof(network_value));
    }

    void append_uint16(uint16_t value){
        uint16_t network_value = htons(value);
        append_bytes(&network_value, sizeof(network_value));
    }

    void append_bool(bool value) {
        unsigned char byte_value = value ? 1 : 0;
        data.push_back(byte_value);
    }

    void append_uint64(uint64_t value) {
        uint64_t network_value = hton64(value);
        append_bytes(&network_value, sizeof(network_value));
    }

    void append_int64(int64_t value) {
        uint64_t network_value = hton64(static_cast<uint64_t>(value));
        append_bytes(&network_value, sizeof(network_value));
    }



    void append_size_t(size_t value) {
        append_uint64(static_cast<uint64_t>(value));
    }
    // --- 向量/字符串追加方法 ---
    void append_uchar_vector(const std::vector<unsigned char>& str) {
        if (str.size() > std::numeric_limits<uint32_t>::max()) {
            throw std::length_error("Vector size exceeds maximum for uint32_t prefix.");
        }
        uint32_t length = static_cast<uint32_t>(str.size());
        uint32_t network_length = htonl(length);
        append_bytes(&network_length, sizeof(network_length));
        append_bytes(str.data(), str.size());
    }

    void append_string(const std::string& str) {
        if (str.length() > std::numeric_limits<uint32_t>::max()) {
            throw std::length_error("String length exceeds maximum for uint32_t prefix.");
        }
        uint32_t length = static_cast<uint32_t>(str.length());
        uint32_t network_length = htonl(length);
        append_bytes(&network_length, sizeof(network_length));
        append_bytes(str.data(), str.length());
    }


private:
    // --- _append_single 重载 ---
    void _append_single(short value) {
        append_short(value);
    }
    void _append_single(int value) {
        append_int(value);
    }
    void _append_single(uint32_t value) {
        append_uint32(value);
    }
    void _append_single(uint16_t value) {
        append_uint16(value);
    }
    void _append_single(bool value) {
        append_bool(value);
    }
    void _append_single(uint64_t value) {
        append_uint64(value);
    }
    void _append_single(int64_t value) {
        append_int64(value);
    }

    void _append_single(const std::string& str) {
        append_string(str);
    }
    void _append_single(const std::vector<unsigned char>& str) {
        append_uchar_vector(str);
    }

    // --- 核心工具函数 ---
    void append_bytes(const void* bytes, size_t count) {
        if (count == 0) return;

        size_t current_size = data.size();

        if (data.capacity() < current_size + count) {
            data.reserve(current_size + count + (current_size / 2)); // 增长因子1.5
        }
        data.resize(current_size + count);
        std::memcpy(data.data() + current_size, bytes, count);
    }

    // hton64 的实现保持不变，因为它处理的是 uint64_t
    uint64_t hton64(uint64_t value) {
        static const int num = 42;
        if (*reinterpret_cast<const char*>(&num) == num) { // 小端系统
            const uint32_t high = htonl(static_cast<uint32_t>(value >> 32));
            const uint32_t low = htonl(static_cast<uint32_t>(value & 0xFFFFFFFF));
            return (static_cast<uint64_t>(low) << 32) | high;
        } else { // 大端系统
            return value;
        }
    }
};


class MessageParser {
private:
    const unsigned char* m_data;
    size_t m_size;
    size_t m_offset;


    uint64_t ntoh64(uint64_t value) {
        static const int num = 42;
        if (*reinterpret_cast<const char*>(&num) == num) {
            const uint32_t high = ntohl(static_cast<uint32_t>(value >> 32));
            const uint32_t low = ntohl(static_cast<uint32_t>(value & 0xFFFFFFFF));
            return (static_cast<uint64_t>(low) << 32) | high;
        } else {
            return value;
        }
    }

public:

    MessageParser(const void* data, size_t size)
        : m_data(static_cast<const unsigned char*>(data))
        , m_size(size)
        , m_offset(0)
    {
        if (!m_data && size > 0) {
            throw std::invalid_argument("MessageParser received nullptr with non-zero size.");
        }
    }

    MessageParser(const std::vector<unsigned char>&) = delete;

    bool eof() const { return m_offset >= m_size; }

    size_t remaining() const { return m_size - m_offset; }


    void read_bytes(void* buffer, size_t count) {
        if (m_offset + count > m_size) {
            throw std::out_of_range("Not enough data to read.");
        }
        std::memcpy(buffer, m_data + m_offset, count);
        m_offset += count;
    }

    void skip(size_t count) {
        if (m_offset + count > m_size) {
            throw std::out_of_range("Not enough data to skip.");
        }
        m_offset += count;
    }

    short read_short() {
        uint16_t network_value;
        read_bytes(&network_value, sizeof(network_value));
        return static_cast<short>(ntohs(network_value));
    }

    int read_int() {
        uint32_t network_value;
        read_bytes(&network_value, sizeof(network_value));
        return static_cast<int>(ntohl(network_value));
    }

    uint32_t read_uint32() {
        uint32_t network_value;
        read_bytes(&network_value, sizeof(network_value));
        return ntohl(network_value);
    }

    int32_t read_int32() {
        uint32_t network_value;
        read_bytes(&network_value, sizeof(network_value));
        return static_cast<int32_t>(ntohl(network_value));
    }

    uint16_t read_uint16() {
        uint16_t network_value;
        read_bytes(&network_value, sizeof(network_value));
        return ntohs(network_value);
    }

    uint64_t read_uint64() {
        uint64_t network_value;
        read_bytes(&network_value, sizeof(network_value));
        return ntoh64(network_value);
    }

    int64_t read_int64() {
        uint64_t network_value;
        read_bytes(&network_value, sizeof(network_value));
        return static_cast<int64_t>(ntoh64(network_value));
    }

    long long read_ll() {
        return static_cast<long long>(read_int64());
    }

    size_t read_size_t() {
        return static_cast<size_t>(read_uint64());
    }

    bool read_bool() {
        if (m_offset + 1 > m_size) {
            throw std::out_of_range("Not enough data to read bool.");
        }
        unsigned char byte_value = m_data[m_offset];
        m_offset += 1;
        return byte_value != 0;
    }

    // ==========================================
    // 字符串与容器读取
    // ==========================================

    std::string read_string() {
        uint32_t length = read_uint32(); // 复用 read_uint32

        if (m_offset + length > m_size) {
            throw std::out_of_range("Not enough data to read string.");
        }

        // 直接从原始指针构造 string
        std::string str(reinterpret_cast<const char*>(m_data + m_offset), length);
        m_offset += length;
        return str;
    }

    std::vector<unsigned char> read_uchar_vector() {
        uint32_t length = read_uint32();

        if (m_offset + length > m_size) {
            throw std::out_of_range("Not enough data to read vector<unsigned char>.");
        }

        const unsigned char* start = m_data + m_offset;
        std::vector<unsigned char> vec(start, start + length);

        m_offset += length;
        return vec;
    }

    // ==========================================
    // 零拷贝 / View API / 嵌套解析
    // ==========================================

    std::string_view read_string_view() {
        uint32_t length = read_uint32();

        if (m_offset + length > m_size) {
            throw std::out_of_range("Not enough data to read string_view.");
        }

        const char* ptr = reinterpret_cast<const char*>(m_data + m_offset);
        m_offset += length;

        return std::string_view(ptr, length);
    }

    // 返回 {指针, 长度}，用于 ZSTD 解压或 CRC 校验
    std::pair<const unsigned char*, uint32_t> read_bytes_view() {
        uint32_t length = read_uint32();

        if (m_offset + length > m_size) {
            throw std::out_of_range("Not enough data to read bytes view.");
        }

        const unsigned char* ptr = m_data + m_offset;
        m_offset += length;

        return {ptr, length};
    }


};





#endif
