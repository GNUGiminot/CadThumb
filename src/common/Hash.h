#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

namespace ct {

// Small non-cryptographic 128-bit hasher (two independent 64-bit lanes).
class Hasher {
public:
    explicit Hasher(uint64_t seed = 0);
    void Update(const void* data, size_t len);
    template <class T>
    void Add(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        Update(&v, sizeof(v));
    }
    uint64_t Digest64() const;
    std::string HexDigest128() const; // 32 hex chars

private:
    uint64_t a_, b_;
    uint64_t len_ = 0;
};

} // namespace ct
