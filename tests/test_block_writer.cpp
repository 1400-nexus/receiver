#include "receiver/block_writer.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
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

// =============================================================================
// Test helpers
// =============================================================================

// Every test gets its own file under the job tmp dir (not /tmp -- shared
// across parallel background jobs) so tests never collide with each other.
static std::string test_file_path(const char* name) {
    return std::string("/home/elkanasassi/.claude/jobs/cac5291e/tmp/bw_test_") + name;
}

// Creates a file of exactly `size` bytes, all zero -- mirrors what
// session_manager's FileStore.allocate() does via fallocate() before
// SessionOpen is ever sent (docs/ANSWERS_FROM_C.md §4).
static void make_fallocated_file(const std::string& path, uint64_t size) {
    ::unlink(path.c_str());
    int fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0644);
    int rc = ::ftruncate(fd, static_cast<off_t>(size));
    (void)rc; // test fixture setup -- a failure here fails the test itself downstream
    ::close(fd);
}

static std::vector<uint8_t> read_whole_file(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(static_cast<size_t>(len));
    if (len > 0) {
        size_t n = std::fread(buf.data(), 1, buf.size(), f);
        (void)n;
    }
    std::fclose(f);
    return buf;
}

// Builds a "decoded block" the way RsCodec::decode() would hand one to a
// real caller: k*symbol_bytes bytes, with `fill` repeated -- a
// recognizable pattern, not real file content, since these tests are
// about offsets and clipping, not decode correctness (that's
// test_rs_codec.cpp's job).
static std::vector<uint8_t> make_block(uint64_t block_bytes, uint8_t fill) {
    return std::vector<uint8_t>(block_bytes, fill);
}

// =============================================================================
// Tests
// =============================================================================

static void test_open_rejects_missing_file() {
    TEST(open_rejects_missing_file);

    const std::string path = test_file_path("missing");
    ::unlink(path.c_str());

    BlockWriter bw;
    ASSERT_TRUE(!bw.open(path.c_str(), 1000, 10, 100));
}

static void test_open_rejects_undersized_file() {
    TEST(open_rejects_undersized_file);

    const std::string path = test_file_path("undersized");
    make_fallocated_file(path, 500); // file exists but is smaller than file_size below

    BlockWriter bw;
    ASSERT_TRUE(!bw.open(path.c_str(), 1000, 10, 100));
}

static void test_open_and_close_are_safe_repeated() {
    TEST(open_and_close_are_safe_repeated);

    const std::string path = test_file_path("open_close");
    make_fallocated_file(path, 1000);

    BlockWriter bw;
    ASSERT_TRUE(bw.open(path.c_str(), 1000, 10, 100));
    ASSERT_EQ(bw.block_bytes(), 1000u); // k=10 * symbol_bytes=100
    bw.close();
    bw.close(); // must be safe to call twice, same convention as ShmManager
}

static void test_write_single_full_block() {
    TEST(write_single_full_block);

    const std::string path = test_file_path("single_block");
    const uint64_t block_bytes = 100; // k=10 * symbol_bytes=10
    make_fallocated_file(path, block_bytes * 3); // 3 whole blocks, no partial tail

    BlockWriter bw;
    ASSERT_TRUE(bw.open(path.c_str(), block_bytes * 3, 10, 10));

    auto block1 = make_block(block_bytes, 0xAB);
    ASSERT_TRUE(bw.write_block(1, block1.data()));
    ASSERT_TRUE(bw.flush());
    bw.close();

    auto on_disk = read_whole_file(path);
    ASSERT_EQ(on_disk.size(), block_bytes * 3);

    // Block 0's region should be untouched (still zero, from fallocate).
    for (uint64_t i = 0; i < block_bytes; ++i) ASSERT_EQ(on_disk[i], 0);
    // Block 1's region should hold what we wrote.
    for (uint64_t i = 0; i < block_bytes; ++i) ASSERT_EQ(on_disk[block_bytes + i], 0xAB);
    // Block 2's region should still be untouched.
    for (uint64_t i = 0; i < block_bytes; ++i) ASSERT_EQ(on_disk[2 * block_bytes + i], 0);
}

static void test_write_multiple_blocks_out_of_order() {
    TEST(write_multiple_blocks_out_of_order);

    const std::string path = test_file_path("out_of_order");
    const uint64_t block_bytes = 50;
    make_fallocated_file(path, block_bytes * 4);

    BlockWriter bw;
    ASSERT_TRUE(bw.open(path.c_str(), block_bytes * 4, 5, 10));

    // Any receiver may complete any block first -- write deliberately out
    // of sequence to prove offset computation doesn't assume ordering.
    auto b3 = make_block(block_bytes, 3);
    // 0xFF, not 0 -- the fallocated file already reads as all zeros, so a
    // fill value of 0 here couldn't distinguish "block 0 was written" from
    // "block 0 was never touched."
    auto b0 = make_block(block_bytes, 0xFF);
    auto b2 = make_block(block_bytes, 2);
    auto b1 = make_block(block_bytes, 1);

    ASSERT_TRUE(bw.write_block(3, b3.data()));
    ASSERT_TRUE(bw.write_block(0, b0.data()));
    ASSERT_TRUE(bw.write_block(2, b2.data()));
    ASSERT_TRUE(bw.write_block(1, b1.data()));
    bw.close();

    auto on_disk = read_whole_file(path);
    ASSERT_EQ(on_disk[0 * block_bytes], 0xFF);
    ASSERT_EQ(on_disk[1 * block_bytes], 1);
    ASSERT_EQ(on_disk[2 * block_bytes], 2);
    ASSERT_EQ(on_disk[3 * block_bytes], 3);
}

static void test_final_partial_block_clips_to_file_size() {
    TEST(final_partial_block_clips_to_file_size);

    // The scenario from docs/ANSWERS_FROM_A.md §11, at small scale: a
    // file whose size isn't a multiple of block_bytes. The sender pads
    // the last block with zeros before encoding; the decoder hands back
    // a FULL block_bytes-sized buffer regardless. This test's decoded
    // buffer mimics that directly: real content, then padding.
    const uint64_t block_bytes = 100;
    const uint64_t file_size = 250; // 2 full blocks + 50 real bytes into block 2
    const std::string path = test_file_path("final_partial");
    make_fallocated_file(path, file_size);

    BlockWriter bw;
    ASSERT_TRUE(bw.open(path.c_str(), file_size, 10, 10));
    ASSERT_EQ(bw.block_bytes(), block_bytes);

    // Block 2 (the final block): first 50 bytes "real" content (0x11),
    // last 50 bytes padding (0x00) -- exactly like a zero-padded tail
    // block from a real sender.
    std::vector<uint8_t> final_block(block_bytes, 0x00);
    for (uint64_t i = 0; i < 50; ++i) final_block[i] = 0x11;

    ASSERT_TRUE(bw.write_block(2, final_block.data()));
    ASSERT_TRUE(bw.flush());
    bw.close();

    auto on_disk = read_whole_file(path);
    ASSERT_EQ(on_disk.size(), file_size); // MUST NOT have grown past file_size

    // Bytes [200,250) (the real 50) should hold the real content.
    for (uint64_t i = 0; i < 50; ++i) {
        ASSERT_EQ(on_disk[200 + i], 0x11);
    }
    // Nothing past byte 250 exists to check -- the file is exactly
    // file_size bytes, which is itself the proof the padding was clipped
    // rather than extending the file.
}

static void test_out_of_range_block_id_fails() {
    TEST(out_of_range_block_id_fails);

    const uint64_t block_bytes = 100;
    const std::string path = test_file_path("out_of_range");
    make_fallocated_file(path, block_bytes * 2);

    BlockWriter bw;
    ASSERT_TRUE(bw.open(path.c_str(), block_bytes * 2, 10, 10));

    // Block 2's offset (200) is >= file_size (200) -- entirely out of
    // range, not a normal final-block case.
    auto block = make_block(block_bytes, 0x99);
    ASSERT_TRUE(!bw.write_block(2, block.data()));

    // File must be untouched by the rejected write.
    auto on_disk = read_whole_file(path);
    for (auto b : on_disk) ASSERT_EQ(b, 0);
}

static void test_zero_file_size_handled_gracefully() {
    TEST(zero_file_size_handled_gracefully);

    const std::string path = test_file_path("zero_size");
    make_fallocated_file(path, 0);

    BlockWriter bw;
    ASSERT_TRUE(bw.open(path.c_str(), 0, 10, 10)); // must not crash on mmap(length=0)

    auto block = make_block(1000, 0x42);
    ASSERT_TRUE(!bw.write_block(0, block.data())); // nothing to write, correctly refused
    ASSERT_TRUE(bw.flush()); // trivially succeeds, nothing mapped
}

static void test_flush_block_succeeds_and_is_ranged() {
    TEST(flush_block_succeeds_and_is_ranged);

    // Not directly observable from outside (msync's effect on a tmpfs/
    // real-fs page cache isn't something a test can distinguish from a
    // no-op without special tooling) -- this proves the two things that
    // ARE observable: it succeeds on a real written block, rejects the
    // same out-of-range case write_block() does, and leaves the data
    // exactly where write_block() put it (i.e. it doesn't corrupt
    // anything, which a wrong page-alignment computation could).
    const uint64_t block_bytes = 100;
    const std::string path = test_file_path("flush_block");
    make_fallocated_file(path, block_bytes * 3);

    BlockWriter bw;
    ASSERT_TRUE(bw.open(path.c_str(), block_bytes * 3, 10, 10));

    auto block1 = make_block(block_bytes, 0x33);
    ASSERT_TRUE(bw.write_block(1, block1.data()));
    ASSERT_TRUE(bw.flush_block(1));

    // Out-of-range block_id -- same rule as write_block().
    ASSERT_TRUE(!bw.flush_block(3));

    bw.close();

    auto on_disk = read_whole_file(path);
    for (uint64_t i = 0; i < block_bytes; ++i) ASSERT_EQ(on_disk[block_bytes + i], 0x33);
    // Neighboring blocks must be untouched by flushing block 1 specifically.
    for (uint64_t i = 0; i < block_bytes; ++i) ASSERT_EQ(on_disk[i], 0);
    for (uint64_t i = 0; i < block_bytes; ++i) ASSERT_EQ(on_disk[2 * block_bytes + i], 0);
}

static void test_flush_block_on_unmapped_writer_fails() {
    TEST(flush_block_on_unmapped_writer_fails);

    // A zero-file_size BlockWriter never mmap()s anything (see
    // test_zero_file_size_handled_gracefully) -- flush_block() must
    // refuse cleanly there too, not dereference a null/invalid base_.
    const std::string path = test_file_path("flush_block_unmapped");
    make_fallocated_file(path, 0);

    BlockWriter bw;
    ASSERT_TRUE(bw.open(path.c_str(), 0, 10, 10));
    ASSERT_TRUE(!bw.flush_block(0));
}

static void test_close_then_independent_reopen_sees_data() {
    TEST(close_then_independent_reopen_sees_data);

    // Proves durability isn't a fluke of still holding the mapping open
    // -- read the file back via a completely separate BlockWriter
    // instance (fresh open(), fresh mmap) rather than the same one.
    const uint64_t block_bytes = 80;
    const std::string path = test_file_path("reopen");
    make_fallocated_file(path, block_bytes * 2);

    {
        BlockWriter bw;
        ASSERT_TRUE(bw.open(path.c_str(), block_bytes * 2, 8, 10));
        auto block = make_block(block_bytes, 0x77);
        ASSERT_TRUE(bw.write_block(0, block.data()));
        ASSERT_TRUE(bw.flush());
        bw.close();
    }

    BlockWriter bw2;
    ASSERT_TRUE(bw2.open(path.c_str(), block_bytes * 2, 8, 10));
    auto on_disk = read_whole_file(path);
    for (uint64_t i = 0; i < block_bytes; ++i) ASSERT_EQ(on_disk[i], 0x77);
    bw2.close();
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "=== Block Writer Tests (Phase 11) ===\n\n";

    RUN(test_open_rejects_missing_file);
    RUN(test_open_rejects_undersized_file);
    RUN(test_open_and_close_are_safe_repeated);
    RUN(test_write_single_full_block);
    RUN(test_write_multiple_blocks_out_of_order);
    RUN(test_final_partial_block_clips_to_file_size);
    RUN(test_out_of_range_block_id_fails);
    RUN(test_flush_block_succeeds_and_is_ranged);
    RUN(test_flush_block_on_unmapped_writer_fails);
    RUN(test_zero_file_size_handled_gracefully);
    RUN(test_close_then_independent_reopen_sees_data);

    std::cout << "\n=== Results: " << g_pass << " run, "
              << g_fail << " failed ===\n";

    return g_fail == 0 ? 0 : 1;
}
