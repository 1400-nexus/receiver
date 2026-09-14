#include "receiver/rs_codec.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <vector>

// =============================================================================
// Minimal test harness (same shape as the other test_*.cpp files).
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

// Two scenarios from the brief's own Phase 10 test list deliberately have
// no dedicated test below, and it's worth saying why rather than looking
// like an oversight:
//
//   - "duplicate symbols" -- RsCodec::decode()'s `present` array is one
//     bool per symbol_id, so a duplicate isn't representable at this
//     layer at all -- there's no "twice" to give it. Duplicate
//     suppression is BlockView::register_symbol()'s job (Phase 5,
//     tests/test_shm_manager.cpp), proven there; nothing to add here.
//   - "final zero-padded block" -- decode() reconstructs exactly
//     block_bytes worth of symbols regardless of whether the file's real
//     content stops partway through the last one. Clipping the write to
//     file_size is Phase 11's job (mmap output), per
//     docs/ANSWERS_FROM_A.md §11 -- this class is agnostic to padding.

// =============================================================================
// Test helpers
// =============================================================================

struct TestBlock {
    uint32_t k, n, symbol_bytes;
    std::vector<std::vector<uint8_t>> data;   // k symbols, the "ground truth"
    std::vector<std::vector<uint8_t>> full;   // n symbols: data + parity, systematic
};

static TestBlock make_test_block(uint32_t k, uint32_t n, uint32_t symbol_bytes,
                                  const RsCodec& codec, uint32_t seed) {
    TestBlock b{k, n, symbol_bytes, {}, {}};
    std::mt19937 rng(seed);

    b.data.assign(k, std::vector<uint8_t>(symbol_bytes));
    std::vector<const uint8_t*> data_ptrs(k);
    for (uint32_t i = 0; i < k; ++i) {
        for (auto& byte : b.data[i]) byte = static_cast<uint8_t>(rng());
        data_ptrs[i] = b.data[i].data();
    }

    std::vector<uint8_t> parity(uint64_t(n - k) * symbol_bytes);
    codec.encode_for_testing(data_ptrs.data(), parity.data());

    b.full.assign(n, std::vector<uint8_t>(symbol_bytes));
    for (uint32_t i = 0; i < k; ++i) b.full[i] = b.data[i];
    for (uint32_t i = 0; i < n - k; ++i) {
        std::memcpy(b.full[k + i].data(), parity.data() + uint64_t(i) * symbol_bytes,
                    symbol_bytes);
    }
    return b;
}

// Runs decode() with exactly `present` marking which of the block's n
// symbols are available, and checks (if `expect_ok`) that every
// reconstructed data symbol matches the ground truth exactly.
static bool decode_and_check(const RsCodec& codec, const TestBlock& b,
                              const std::unique_ptr<bool[]>& present,
                              bool expect_ok) {
    std::vector<const uint8_t*> symptrs(b.n);
    for (uint32_t i = 0; i < b.n; ++i) symptrs[i] = b.full[i].data();

    std::vector<uint8_t> out(uint64_t(b.k) * b.symbol_bytes, 0xCC); // poison, not zero
    const bool ok = codec.decode(symptrs.data(), present.get(), out.data());

    if (ok != expect_ok) return false;
    if (!ok) return true; // nothing more to check on an expected failure

    for (uint32_t i = 0; i < b.k; ++i) {
        if (std::memcmp(out.data() + uint64_t(i) * b.symbol_bytes,
                         b.data[i].data(), b.symbol_bytes) != 0) {
            return false;
        }
    }
    return true;
}

static std::unique_ptr<bool[]> all_present(uint32_t n) {
    auto p = std::make_unique<bool[]>(n);
    for (uint32_t i = 0; i < n; ++i) p[i] = true;
    return p;
}

// =============================================================================
// Tests
// =============================================================================

static void test_decode_all_data_present_fast_path() {
    TEST(decode_all_data_present_fast_path);

    RsCodec codec(8, 12, 16);
    auto block = make_test_block(8, 12, 16, codec, 1);
    auto present = all_present(12);

    ASSERT_TRUE(decode_and_check(codec, block, present, /*expect_ok=*/true));
}

static void test_decode_missing_parity_only_still_fast_path() {
    TEST(decode_missing_parity_only_still_fast_path);

    RsCodec codec(8, 12, 16);
    auto block = make_test_block(8, 12, 16, codec, 2);
    auto present = all_present(12);
    present[9] = present[11] = false; // drop 2 of 4 parity symbols -- data still complete

    ASSERT_TRUE(decode_and_check(codec, block, present, /*expect_ok=*/true));
}

static void test_decode_recovers_missing_data_from_parity() {
    TEST(decode_recovers_missing_data_from_parity);

    RsCodec codec(8, 12, 16);
    auto block = make_test_block(8, 12, 16, codec, 3);
    auto present = all_present(12);
    present[0] = present[3] = present[7] = false; // 3 of 8 data symbols missing, 4 parity available

    ASSERT_TRUE(decode_and_check(codec, block, present, /*expect_ok=*/true));
}

static void test_decode_exact_k_present_mixed() {
    TEST(decode_exact_k_present_mixed);

    // Exactly k of the n symbols present -- no slack at all, the tightest
    // case decode() has to handle correctly.
    RsCodec codec(8, 12, 16);
    auto block = make_test_block(8, 12, 16, codec, 4);
    auto present = std::make_unique<bool[]>(12);
    for (uint32_t i = 0; i < 12; ++i) present[i] = false;
    // Pick a mixed set of exactly 8: symbols 1,2,4,6,7 (data) + 8,9,11 (parity).
    for (uint32_t i : {1u, 2u, 4u, 6u, 7u, 8u, 9u, 11u}) present[i] = true;

    ASSERT_TRUE(decode_and_check(codec, block, present, /*expect_ok=*/true));
}

static void test_decode_insufficient_symbols_fails() {
    TEST(decode_insufficient_symbols_fails);

    RsCodec codec(8, 12, 16);
    auto block = make_test_block(8, 12, 16, codec, 5);
    auto present = std::make_unique<bool[]>(12);
    for (uint32_t i = 0; i < 12; ++i) present[i] = false;
    for (uint32_t i : {2u, 3u, 4u, 5u, 6u, 7u, 9u}) present[i] = true; // exactly 7 < k=8

    ASSERT_TRUE(decode_and_check(codec, block, present, /*expect_ok=*/false));
}

static void test_decode_zero_symbols_present_fails() {
    TEST(decode_zero_symbols_present_fails);

    RsCodec codec(8, 12, 16);
    auto block = make_test_block(8, 12, 16, codec, 6);
    auto present = std::make_unique<bool[]>(12);
    for (uint32_t i = 0; i < 12; ++i) present[i] = false;

    ASSERT_TRUE(decode_and_check(codec, block, present, /*expect_ok=*/false));
}

static void test_decode_every_erasure_count_up_to_max_recoverable() {
    TEST(decode_every_erasure_count_up_to_max_recoverable);

    // n-k=4 parity symbols means up to 4 ERASURES are always recoverable,
    // regardless of whether the erased symbols are data or parity -- the
    // core erasure-code guarantee. Sweep every count from 0 to 4,
    // dropping DATA symbols specifically (the harder case -- dropping
    // parity is what the fast path already covers).
    RsCodec codec(8, 12, 16);
    auto block = make_test_block(8, 12, 16, codec, 7);

    for (uint32_t drop = 0; drop <= 4; ++drop) {
        auto present = all_present(12);
        for (uint32_t i = 0; i < drop; ++i) present[i] = false;
        ASSERT_TRUE(decode_and_check(codec, block, present, /*expect_ok=*/true));
    }
}

static void test_random_erasure_patterns() {
    TEST(random_erasure_patterns);

    // Not exhaustive (C(12,8) = 495 combinations, small enough to actually
    // BE exhaustive, but random sampling here doubles as a template for
    // the realistic-scale test below, where exhaustive is not an option).
    RsCodec codec(8, 12, 16);
    auto block = make_test_block(8, 12, 16, codec, 8);

    std::mt19937 rng(123);
    for (int trial = 0; trial < 50; ++trial) {
        std::vector<uint32_t> order(12);
        for (uint32_t i = 0; i < 12; ++i) order[i] = i;
        std::shuffle(order.begin(), order.end(), rng);

        auto present = std::make_unique<bool[]>(12);
        for (uint32_t i = 0; i < 12; ++i) present[i] = false;
        for (uint32_t i = 0; i < 8; ++i) present[order[i]] = true; // exactly k present, random which

        ASSERT_TRUE(decode_and_check(codec, block, present, /*expect_ok=*/true));
    }
}

static void test_different_geometry_not_hardcoded() {
    TEST(different_geometry_not_hardcoded);

    // A's answer (docs/ANSWERS_FROM_A.md §10) is explicit: k/n/symbol_bytes
    // are per-session, never hardcoded. Running the exact same test shape
    // against a second, different geometry is what actually proves that,
    // rather than just asserting it in a comment.
    RsCodec codec(5, 9, 64); // different k, different n, different symbol size
    auto block = make_test_block(5, 9, 64, codec, 9);

    auto present = all_present(9);
    present[0] = present[2] = false; // 2 of 5 data symbols missing, 4 parity available (n-k=4)

    ASSERT_TRUE(decode_and_check(codec, block, present, /*expect_ok=*/true));
}

static void test_realistic_scale_geometry() {
    TEST(realistic_scale_geometry);

    // AGENT_IMPLEMENTATION.md's default geometry (K=200, N=255,
    // symbol_bytes=1400) -- confirmed by A as a real, currently-shipped
    // default (docs/ANSWERS_FROM_A.md §10), though per-session and not
    // locked forever. Proves this isn't just correct at toy scale.
    const uint32_t k = 200, n = 255, symbol_bytes = 1400;

    const auto t0 = std::chrono::steady_clock::now();
    RsCodec codec(k, n, symbol_bytes);
    auto block = make_test_block(k, n, symbol_bytes, codec, 10);

    auto present = all_present(n);
    // Erase 55 data symbols (the maximum possible: n-k=55 parity symbols
    // means exactly 55 erasures is the recoverable limit) -- the worst
    // case this geometry is specified to tolerate.
    for (uint32_t i = 0; i < 55; ++i) present[i] = false;

    const bool result_ok = decode_and_check(codec, block, present, /*expect_ok=*/true);
    const auto t1 = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::cout << "  (realistic-scale decode: " << ms << " ms for k=" << k
              << " n=" << n << " symbol_bytes=" << symbol_bytes << ")\n";

    ASSERT_TRUE(result_ok);

    // One more than the recoverable limit must fail, not silently return
    // a wrong/partial answer.
    auto present_one_too_many = all_present(n);
    for (uint32_t i = 0; i < 56; ++i) present_one_too_many[i] = false;
    ASSERT_TRUE(decode_and_check(codec, block, present_one_too_many, /*expect_ok=*/false));
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "=== RS Codec Tests (Phase 10) ===\n\n";

    RUN(test_decode_all_data_present_fast_path);
    RUN(test_decode_missing_parity_only_still_fast_path);
    RUN(test_decode_recovers_missing_data_from_parity);
    RUN(test_decode_exact_k_present_mixed);
    RUN(test_decode_insufficient_symbols_fails);
    RUN(test_decode_zero_symbols_present_fails);
    RUN(test_decode_every_erasure_count_up_to_max_recoverable);
    RUN(test_random_erasure_patterns);
    RUN(test_different_geometry_not_hardcoded);
    RUN(test_realistic_scale_geometry);

    std::cout << "\n=== Results: " << g_pass << " run, "
              << g_fail << " failed ===\n";

    return g_fail == 0 ? 0 : 1;
}
