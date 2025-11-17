#ifndef MURMURHASH2_H
#define MURMURHASH2_H

#include <string>
#include <cstdint> // For uint32_t, uint8_t

namespace MurmurHash2 {

// MurmurHash2 32-bit implementation
// key: Pointer to the data to be hashed.
// len: Length of the data in bytes.
// seed: Initial seed for the hash.
inline uint32_t calculate_32(const void* key, int len, uint32_t seed) {
    // 'm' and 'r' are mixing constants generated offline.
    // They're not really "magic", they just happen to work well.
    const uint32_t m = 0x5bd1e995;
    const int r = 24;

    // Initialize the hash to the seed
    uint32_t h = seed ^ (uint32_t)len;

    // Mix 4 bytes at a time into the hash
    const unsigned char* data = (const unsigned char*)key;

    while (len >= 4) {
        uint32_t k = *(uint32_t*)data;

        k *= m;
        k ^= k >> r;
        k *= m;

        h *= m;
        h ^= k;

        data += 4;
        len -= 4;
    }

    // Handle the last few bytes of the input array
    switch (len) {
    case 3:
        h ^= data[2] << 16;
    case 2:
        h ^= data[1] << 8;
    case 1:
        h ^= data[0];
        h *= m;
    };

    // Do a final mix of the hash to ensure all bits are spread out
    h ^= h >> 13;
    h *= m;
    h ^= h >> 15;

    return h;
}

/**
     * @brief Computes the 32-bit MurmurHash2 of a std::string.
     *
     * This function treats the std::string as a sequence of bytes and
     * computes its MurmurHash2.
     *
     * @param s The input string to hash.
     * @param seed An optional seed for the hash function. Defaults to 0.
     * @return The 32-bit hash value.
     */
inline uint32_t hash(const std::string& s, uint32_t seed = 0) {
    // Use s.data() to get a pointer to the underlying character array
    // and s.length() for its size in bytes.
    // Note: s.data() returns const char*, which is compatible with const void*.
    return calculate_32(s.data(), static_cast<int>(s.length()), seed);
}

} // namespace MurmurHash2

#endif // MURMURHASH2_H
