#include "common/crc.hpp"

#include <cstring>

#if defined(__x86_64__) || defined(__i386__)
#include <nmmintrin.h> // _mm_crc32_u8 / _mm_crc32_u64
#define UNIFLOW_X86 1
#endif

// =============================================================================
// CRC16/CCITT-FALSE
// =============================================================================
//
// Textbook bit-at-a-time implementation -- called once per 12-byte prefix
// (6 bytes, actually: see wire.cpp), never per-payload-byte, so there's no
// performance case for a table here the way there is for CRC32C below.

uint16_t crc16_ccitt_false(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 0x8000) {
                crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
            } else {
                crc = static_cast<uint16_t>(crc << 1);
            }
        }
    }
    return crc;
}

// =============================================================================
// CRC32C (Castagnoli)
// =============================================================================

namespace {

// Portable fallback: reflected bit-at-a-time algorithm using the
// Castagnoli polynomial in its reflected form (0x82F63B78). This is what
// runs on non-x86 builds, or an x86 CPU old enough to lack SSE4.2 (nothing
// this team runs on, but the check costs one branch and the code costs
// nothing to keep correct).
uint32_t crc32c_sw(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0u - (crc & 1u); // all-1s if the low bit is set, else 0
            crc = (crc >> 1) ^ (0x82F63B78u & mask);
        }
    }
    return ~crc;
}

#if defined(UNIFLOW_X86)

// `target("sse4.2")` compiles just this one function against the SSE4.2
// instruction set, without needing -msse4.2 on the whole translation
// unit (or the whole project, which would make the binary crash-on-launch
// on any older CPU instead of falling back cleanly). GCC/Clang both
// support this as a function attribute.
__attribute__((target("sse4.2")))
uint32_t crc32c_hw(const uint8_t* p, size_t n) {
    uint64_t crc = 0xFFFFFFFFu;
    while (n >= 8) {
        uint64_t v;
        std::memcpy(&v, p, 8); // memcpy, not a cast: p isn't guaranteed 8-byte aligned
        crc = _mm_crc32_u64(crc, v);
        p += 8;
        n -= 8;
    }
    while (n--) {
        crc = _mm_crc32_u8(static_cast<uint32_t>(crc), *p++);
    }
    return ~static_cast<uint32_t>(crc);
}

bool cpu_has_sse42() {
    static const bool has = __builtin_cpu_supports("sse4.2");
    return has;
}

#endif // UNIFLOW_X86

} // namespace

uint32_t crc32c(const uint8_t* data, size_t len) {
#if defined(UNIFLOW_X86)
    if (cpu_has_sse42()) {
        return crc32c_hw(data, len);
    }
#endif
    return crc32c_sw(data, len);
}
