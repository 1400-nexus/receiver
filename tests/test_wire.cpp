#include "common/wire.hpp"

#include <cstring>
#include <iostream>
#include <vector>

// =============================================================================
// Minimal test harness (same shape as test_shm_manager.cpp/test_crc.cpp).
// =============================================================================

static int  g_pass = 0;
static int  g_fail = 0;
static const char* g_current_test = nullptr;

#define TEST(name)                                                      \
    do { g_current_test = #name; } while(0)

#define ASSERT_TRUE(expr)                                               \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::cerr << "  FAIL: " << #expr                            \
                      << "  [" << g_current_test << "]\n";              \
            ++g_fail;                                                   \
            return;                                                     \
        }                                                               \
    } while(0)

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

static void test_build_then_validate_round_trip() {
    TEST(build_then_validate_round_trip);

    const char* body = "hello uniflow";
    const size_t body_len = std::strlen(body);

    uint8_t frame[64];
    const size_t frame_len = build_frame(
        frame, sizeof(frame), reinterpret_cast<const uint8_t*>(body), body_len);
    ASSERT_EQ(frame_len, WIRE_PREFIX_SIZE + body_len);

    const uint8_t* out_body = nullptr;
    size_t out_body_len = 0;
    auto result = validate_frame_prefix(frame, frame_len, &out_body, &out_body_len);

    ASSERT_TRUE(result == WireValidateResult::Ok);
    ASSERT_EQ(out_body_len, body_len);
    ASSERT_TRUE(out_body != nullptr);
    ASSERT_TRUE(std::memcmp(out_body, body, body_len) == 0);

    // out_body must point INSIDE frame, not a copy -- that's the whole
    // point of returning a span instead of a std::vector.
    ASSERT_TRUE(out_body == frame + WIRE_PREFIX_SIZE);
}

static void test_build_empty_body_round_trip() {
    TEST(build_empty_body_round_trip);

    uint8_t frame[WIRE_PREFIX_SIZE];
    const size_t frame_len = build_frame(frame, sizeof(frame), nullptr, 0);
    ASSERT_EQ(frame_len, WIRE_PREFIX_SIZE);

    const uint8_t* out_body = nullptr;
    size_t out_body_len = 123; // deliberately non-zero, must be reset to 0
    auto result = validate_frame_prefix(frame, frame_len, &out_body, &out_body_len);

    ASSERT_TRUE(result == WireValidateResult::Ok);
    ASSERT_EQ(out_body_len, 0u);
}

static void test_build_rejects_undersized_buffer() {
    TEST(build_rejects_undersized_buffer);

    uint8_t tiny[WIRE_PREFIX_SIZE]; // no room for even 1 body byte
    const uint8_t body[1] = {0x42};
    ASSERT_EQ(build_frame(tiny, sizeof(tiny), body, 1), 0u);
}

static void test_validate_rejects_short_datagram() {
    TEST(validate_rejects_short_datagram);

    uint8_t tiny[4] = {'U', 'N', 'I', 'F'}; // shorter than the 12-byte prefix itself
    const uint8_t* out_body = nullptr;
    size_t out_body_len = 0;
    auto result = validate_frame_prefix(tiny, sizeof(tiny), &out_body, &out_body_len);

    ASSERT_TRUE(result == WireValidateResult::BadMagic);
    ASSERT_TRUE(out_body == nullptr);
    ASSERT_EQ(out_body_len, 0u);
}

static void test_validate_rejects_bad_magic() {
    TEST(validate_rejects_bad_magic);

    const char* body = "x";
    uint8_t frame[WIRE_PREFIX_SIZE + 1];
    const size_t frame_len = build_frame(
        frame, sizeof(frame), reinterpret_cast<const uint8_t*>(body), 1);
    ASSERT_TRUE(frame_len > 0);

    frame[0] = 'X'; // corrupt magic byte 0: 'U' -> 'X'

    auto result = validate_frame_prefix(frame, frame_len, nullptr, nullptr);
    ASSERT_TRUE(result == WireValidateResult::BadMagic);
}

static void test_validate_rejects_bad_header_crc() {
    TEST(validate_rejects_bad_header_crc);

    const char* body = "x";
    uint8_t frame[WIRE_PREFIX_SIZE + 1];
    const size_t frame_len = build_frame(
        frame, sizeof(frame), reinterpret_cast<const uint8_t*>(body), 1);
    ASSERT_TRUE(frame_len > 0);

    // Corrupt proto_len (covered by hdr_crc16) without touching the CRC
    // bytes themselves -- magic still checks out, so this must be caught
    // at the header-CRC step, not slip through as a bogus length.
    frame[4] ^= 0xFF;

    auto result = validate_frame_prefix(frame, frame_len, nullptr, nullptr);
    ASSERT_TRUE(result == WireValidateResult::BadHeaderCrc);
}

static void test_validate_rejects_out_of_bounds_proto_len() {
    TEST(validate_rejects_out_of_bounds_proto_len);

    const char* body = "hello";
    uint8_t frame[WIRE_PREFIX_SIZE + 5];
    const size_t frame_len = build_frame(
        frame, sizeof(frame), reinterpret_cast<const uint8_t*>(body), 5);
    ASSERT_TRUE(frame_len > 0);

    // Truncate what we hand to the validator to fewer bytes than
    // proto_len promises -- as if recvmmsg() only delivered a partial
    // datagram. Bounds-check must catch this BEFORE any body CRC read,
    // i.e. before touching memory past what was actually received.
    auto result = validate_frame_prefix(frame, frame_len - 2, nullptr, nullptr);
    ASSERT_TRUE(result == WireValidateResult::ProtoLenOutOfBounds);
}

static void test_validate_rejects_bad_body_crc() {
    TEST(validate_rejects_bad_body_crc);

    const char* body = "hello uniflow";
    const size_t body_len = std::strlen(body);
    uint8_t frame[64];
    const size_t frame_len = build_frame(
        frame, sizeof(frame), reinterpret_cast<const uint8_t*>(body), body_len);
    ASSERT_TRUE(frame_len > 0);

    // Corrupt one body byte -- magic, header CRC, and proto_len all still
    // check out, so this must be caught specifically at the body-CRC step.
    frame[WIRE_PREFIX_SIZE] ^= 0x01;

    auto result = validate_frame_prefix(frame, frame_len, nullptr, nullptr);
    ASSERT_TRUE(result == WireValidateResult::BadBodyCrc);
}

static void test_validate_accepts_trailing_garbage_after_body() {
    TEST(validate_accepts_trailing_garbage_after_body);

    // recvmmsg() reports datagram_len for exactly what arrived; a valid
    // frame followed by extra bytes beyond proto_len shouldn't happen in
    // practice, but the validator's job is only to check what proto_len
    // claims -- trailing bytes past that are simply not part of this
    // frame and must not cause a spurious failure.
    const char* body = "x";
    uint8_t frame[WIRE_PREFIX_SIZE + 1 + 4]; // 4 bytes of trailing "garbage"
    const size_t frame_len = build_frame(
        frame, WIRE_PREFIX_SIZE + 1, reinterpret_cast<const uint8_t*>(body), 1);
    ASSERT_TRUE(frame_len > 0);
    frame[WIRE_PREFIX_SIZE + 1 + 0] = 0xDE;
    frame[WIRE_PREFIX_SIZE + 1 + 1] = 0xAD;
    frame[WIRE_PREFIX_SIZE + 1 + 2] = 0xBE;
    frame[WIRE_PREFIX_SIZE + 1 + 3] = 0xEF;

    const uint8_t* out_body = nullptr;
    size_t out_body_len = 0;
    auto result = validate_frame_prefix(
        frame, sizeof(frame), &out_body, &out_body_len);

    ASSERT_TRUE(result == WireValidateResult::Ok);
    ASSERT_EQ(out_body_len, 1u);
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "=== Wire Framing Tests (Phase 7) ===\n\n";

    RUN(test_build_then_validate_round_trip);
    RUN(test_build_empty_body_round_trip);
    RUN(test_build_rejects_undersized_buffer);

    RUN(test_validate_rejects_short_datagram);
    RUN(test_validate_rejects_bad_magic);
    RUN(test_validate_rejects_bad_header_crc);
    RUN(test_validate_rejects_out_of_bounds_proto_len);
    RUN(test_validate_rejects_bad_body_crc);
    RUN(test_validate_accepts_trailing_garbage_after_body);

    std::cout << "\n=== Results: " << g_pass << " run, "
              << g_fail << " failed ===\n";

    return g_fail == 0 ? 0 : 1;
}
