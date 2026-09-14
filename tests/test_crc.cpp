#include "common/crc.hpp"

#include <cstring>
#include <iostream>

// =============================================================================
// Minimal test harness (same shape as test_shm_manager.cpp -- kept
// self-contained per file rather than factored into a shared header, to
// match the existing style in this repo).
// =============================================================================

static int  g_pass = 0;
static int  g_fail = 0;
static const char* g_current_test = nullptr;

#define TEST(name)                                                      \
    do { g_current_test = #name; } while(0)

#define ASSERT_EQ(a, b)                                                 \
    do {                                                                \
        if ((a) != (b)) {                                               \
            std::cerr << "  FAIL: " << #a << " == " << #b              \
                      << "  [" << g_current_test << "]\n";              \
            ++g_fail;                                                   \
            return;                                                     \
        }                                                               \
    } while(0)

#define RUN(fn)                                                         \
    do {                                                                \
        ++g_pass;                                                       \
        fn();                                                           \
    } while(0)

// =============================================================================
// Tests
// =============================================================================

// The industry-standard sanity check for any CRC32C implementation --
// A's_guide.txt §8 calls this out explicitly and says to hand the same
// vector to Person B, which is exactly what this is. If this fails, the
// polynomial/init/reflection/xorout are wrong somewhere -- fix that before
// trusting anything else in this file.
static void test_crc32c_check_vector() {
    TEST(crc32c_check_vector);
    const char* s = "123456789";
    ASSERT_EQ(crc32c(reinterpret_cast<const uint8_t*>(s), 9), 0xE3069283u);
}

static void test_crc32c_empty() {
    TEST(crc32c_empty);
    ASSERT_EQ(crc32c(nullptr, 0), 0x00000000u);
}

static void test_crc32c_single_bit_flip_changes_result() {
    TEST(crc32c_single_bit_flip_changes_result);
    uint8_t data[16];
    for (int i = 0; i < 16; ++i) data[i] = static_cast<uint8_t>(i);
    const uint32_t original = crc32c(data, sizeof(data));

    data[7] ^= 0x01; // flip one bit
    const uint32_t flipped = crc32c(data, sizeof(data));

    // Not a mathematical proof of avalanche, just confirms the obvious:
    // a real implementation cannot leave a bit flip undetected on such a
    // short, otherwise-identical buffer.
    ASSERT_EQ(original == flipped, false);
}

// This project's chosen CRC16 variant (crc.hpp: CCITT-FALSE) still needs
// confirming against Person A's implementation -- see the header comment
// and tests/vectors/crc_vectors.md. This test pins down *this side's*
// implementation against the well-known CCITT-FALSE check value so a
// future accidental change here (e.g. swapping in a different variant) is
// caught immediately, even before that cross-team confirmation happens.
static void test_crc16_ccitt_false_check_vector() {
    TEST(crc16_ccitt_false_check_vector);
    const char* s = "123456789";
    ASSERT_EQ(crc16_ccitt_false(reinterpret_cast<const uint8_t*>(s), 9), 0x29B1u);
}

static void test_crc16_empty() {
    TEST(crc16_empty);
    ASSERT_EQ(crc16_ccitt_false(nullptr, 0), 0xFFFFu); // init value, no bytes folded in
}

static void test_crc16_single_bit_flip_changes_result() {
    TEST(crc16_single_bit_flip_changes_result);
    uint8_t data[6] = {'U', 'N', 'I', 'F', 0x2C, 0x01}; // a plausible magic+proto_len prefix
    const uint16_t original = crc16_ccitt_false(data, sizeof(data));

    data[4] ^= 0x01;
    const uint16_t flipped = crc16_ccitt_false(data, sizeof(data));

    ASSERT_EQ(original == flipped, false);
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "=== CRC Tests (Phase 7) ===\n\n";

    RUN(test_crc32c_check_vector);
    RUN(test_crc32c_empty);
    RUN(test_crc32c_single_bit_flip_changes_result);

    RUN(test_crc16_ccitt_false_check_vector);
    RUN(test_crc16_empty);
    RUN(test_crc16_single_bit_flip_changes_result);

    std::cout << "\n=== Results: " << g_pass << " run, "
              << g_fail << " failed ===\n";

    return g_fail == 0 ? 0 : 1;
}
