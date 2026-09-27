#include "common/Hash.h"

namespace ct {

static inline uint64_t Mix(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

Hasher::Hasher(uint64_t seed) : a_(0xcbf29ce484222325ULL ^ seed), b_(0x84222325cbf29ce4ULL ^ Mix(seed + 1)) {}

void Hasher::Update(const void* data, size_t len) {
    const auto* p = static_cast<const unsigned char*>(data);
    uint64_t a = a_, b = b_;
    for (size_t i = 0; i < len; ++i) {
        a = (a ^ p[i]) * 0x100000001b3ULL;
        b = (b ^ p[i]) * 0x9E3779B97F4A7C15ULL + 0x632BE59BD9B4E019ULL;
        b ^= b >> 29;
    }
    a_ = a;
    b_ = b;
    len_ += len;
}

uint64_t Hasher::Digest64() const { return Mix(a_ ^ Mix(b_ + len_)); }

std::string Hasher::HexDigest128() const {
    uint64_t h1 = Mix(a_ + len_);
    uint64_t h2 = Mix(b_ ^ (h1 * 31));
    static const char* hex = "0123456789abcdef";
    std::string s(32, '0');
    for (int i = 0; i < 16; ++i) {
        s[15 - i] = hex[(h1 >> (i * 4)) & 0xF];
        s[31 - i] = hex[(h2 >> (i * 4)) & 0xF];
    }
    return s;
}

} // namespace ct
