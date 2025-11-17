#include <iostream>
#include <string>
#include <vector>
#include <stdexcept>

namespace Base64 {

// Base64 编码表
static const std::string base64_chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";


std::string base64_encode(const std::string& in) {
    std::string out;
    // 预分配内存以提高性能
    out.reserve(((in.length() / 3) + (in.length() % 3 > 0)) * 4);

    int val = 0;
    int valb = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(base64_chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) {
        out.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
    }
    while (out.length() % 4) {
        out.push_back('=');
    }
    return out;
}



std::string base64_decode(const std::string& in) {
    // 构建反向查找表
    static const std::vector<int> b64_inv_map = []{
        std::vector<int> v(256, -1);
        for (int i = 0; i < 64; i++) {
            v[base64_chars[i]] = i;
        }
        return v;
    }();

    // 错误检查：输入长度必须是 4 的倍数
    if (in.length() % 4 != 0) {
        throw std::invalid_argument("Invalid Base64 input length. Must be a multiple of 4.");
    }

    std::string out;
    out.reserve(in.length() * 3 / 4);
    int val = 0;
    int valb = -8;

    for (unsigned char c : in) {
        if (b64_inv_map[c] == -1) {
            // 如果是 '=' 填充符，则跳出循环
            if (c == '=') {
                break;
            }
            // 否则是无效字符
            throw std::invalid_argument("Invalid character in Base64 input.");
        }
        val = (val << 6) + b64_inv_map[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }

    // 进一步检查填充符的合法性
    size_t padding_count = 0;
    if (in.length() > 0) {
        for (auto it = in.rbegin(); it != in.rend() && *it == '='; ++it) {
            padding_count++;
        }
    }

    if (padding_count > 0) {
        // 检查非填充字符后是否紧跟着填充字符
        if (in.length() < padding_count) { // 不应该发生，但作为安全检查
            throw std::invalid_argument("Invalid Base64 padding.");
        }
        // 检查填充符前的字符是否有效
        for (size_t i = 0; i < in.length() - padding_count; ++i) {
            if (in[i] == '=') {
                throw std::invalid_argument("Invalid Base64 padding: '=' found in the middle of the string.");
            }
        }
    }

    return out;
}

} // namespace Base64

