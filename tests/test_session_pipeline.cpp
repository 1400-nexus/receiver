#include "receiver/session_pipeline.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
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

static std::string test_file_path(const char* name) {
    // Fixture files go under $TMPDIR (dev.sh sets TMPDIR=/tmp in the
    // container); never a hardcoded home directory, which does not exist
    // on other machines or inside containers.
    const char* tmpdir = std::getenv("TMPDIR");
    return std::string(tmpdir ? tmpdir : "/tmp") + "/sp_test_" + name;
}

static void make_fallocated_file(const std::string& path, uint64_t size) {
    ::unlink(path.c_str());
    int fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0644);
    int rc = ::ftruncate(fd, static_cast<off_t>(size));
    (void)rc;
    ::close(fd);
}

static std::vector<uint8_t> read_whole_file(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(static_cast<size_t>(len));
    if (len > 0) { size_t n = std::fread(buf.data(), 1, buf.size(), f); (void)n; }
    std::fclose(f);
    return buf;
}

static nexus::rx::SessionOpen make_session_open(const std::string& id, const std::string& path,
                                                  uint32_t total_blocks, uint32_t k, uint32_t n,
                                                  uint32_t symbol_bytes) {
    nexus::rx::SessionOpen so;
    so.set_session_id(id);
    so.set_dest_path(path);
    so.set_total_blocks(total_blocks);
    so.set_k(k);
    so.set_n(n);
    so.set_block_bytes(symbol_bytes); // wire field IS the symbol size (docs/ANSWERS_FROM_C.md §0)
    so.set_block_table_offset(0);     // session_manager's own nxrx offsets -- unused by this segment
    so.set_bitmap_offset(0);
    return so;
}

// Builds one fully-encoded block (k data + (n-k) parity symbols) from
// random data, using a throwaway RsCodec matching the session's geometry
// -- exactly mirrors test_rs_codec.cpp's own fixture approach, and for
// the same reason: a decoder needs a real encoder to test against, and
// duplicating the matrix logic in yet another place would be the thing
// this whole project keeps avoiding.
struct EncodedBlock {
    std::vector<std::vector<uint8_t>> symbols; // n symbols, each symbol_bytes long
};

static EncodedBlock make_encoded_block(uint32_t k, uint32_t n, uint32_t symbol_bytes, uint32_t seed) {
    RsCodec codec(k, n, symbol_bytes);
    std::mt19937 rng(seed);

    std::vector<std::vector<uint8_t>> data(k, std::vector<uint8_t>(symbol_bytes));
    std::vector<const uint8_t*> ptrs(k);
    for (uint32_t i = 0; i < k; ++i) {
        for (auto& b : data[i]) b = static_cast<uint8_t>(rng());
        ptrs[i] = data[i].data();
    }

    std::vector<uint8_t> parity(uint64_t(n - k) * symbol_bytes);
    codec.encode_for_testing(ptrs.data(), parity.data());

    EncodedBlock block;
    block.symbols.assign(n, std::vector<uint8_t>(symbol_bytes));
    for (uint32_t i = 0; i < k; ++i) block.symbols[i] = data[i];
    for (uint32_t i = 0; i < n - k; ++i) {
        std::memcpy(block.symbols[k + i].data(), parity.data() + uint64_t(i) * symbol_bytes,
                    symbol_bytes);
    }
    return block;
}

// =============================================================================
// Tests
// =============================================================================

static void test_session_open_creates_context_and_is_idempotent() {
    TEST(session_open_creates_context_and_is_idempotent);

    const std::string path = test_file_path("open.bin");
    make_fallocated_file(path, 1400 * 200 * 2); // 2 blocks worth

    ShmManager shm;
    ASSERT_TRUE(shm.create(SLOT_SIZE * 100));
    SessionPipeline pipeline(shm);

    auto so = make_session_open("sess-open", path, 2, 200, 255, 1400);
    ASSERT_TRUE(pipeline.handle_session_open(so, 1400ull * 200 * 2));

    // Idempotent -- a second SessionOpen for the same session_id must
    // not fail or re-initialize anything (RECEIVER_CONTRACT.md §5
    // property 2).
    ASSERT_TRUE(pipeline.handle_session_open(so, 1400ull * 200 * 2));
}

static void test_session_open_fails_on_missing_file() {
    TEST(session_open_fails_on_missing_file);

    const std::string path = test_file_path("missing.bin");
    ::unlink(path.c_str());

    ShmManager shm;
    ASSERT_TRUE(shm.create(SLOT_SIZE * 100));
    SessionPipeline pipeline(shm);

    auto so = make_session_open("sess-missing", path, 1, 200, 255, 1400);
    ASSERT_TRUE(!pipeline.handle_session_open(so, 280000));
}

static void test_purge_session_drops_context_and_unknown_id_is_noop() {
    TEST(purge_session_drops_context_and_unknown_id_is_noop);

    const std::string path = test_file_path("purge.bin");
    make_fallocated_file(path, 1400 * 200);

    ShmManager shm;
    ASSERT_TRUE(shm.create(SLOT_SIZE * 100));
    SessionPipeline pipeline(shm);

    auto so = make_session_open("sess-purge", path, 1, 200, 255, 1400);
    ASSERT_TRUE(pipeline.handle_session_open(so, 1400ull * 200));

    // Unknown id first: must not fail or disturb the open session.
    pipeline.purge_session("never-opened");

    std::vector<uint8_t> payload(1400, 0xAB);
    uint32_t decoded_id = 0;
    ASSERT_TRUE(pipeline.handle_data_packet("sess-purge", 0, 0,
                                            payload.data(), payload.size(), &decoded_id)
                == DataPacketOutcome::RegisteredOnly);

    pipeline.purge_session("sess-purge");

    // Context is gone: packets now report UnknownSession, and a second
    // purge is still a no-op.
    ASSERT_TRUE(pipeline.handle_data_packet("sess-purge", 0, 1,
                                            payload.data(), payload.size(), &decoded_id)
                == DataPacketOutcome::UnknownSession);
    pipeline.purge_session("sess-purge");

    // A fresh SessionOpen reopens the session afterwards.
    ASSERT_TRUE(pipeline.handle_session_open(so, 1400ull * 200));
    ASSERT_TRUE(pipeline.handle_data_packet("sess-purge", 0, 1,
                                            payload.data(), payload.size(), &decoded_id)
                == DataPacketOutcome::RegisteredOnly);
}

static void test_data_packet_unknown_session() {
    TEST(data_packet_unknown_session);

    ShmManager shm;
    ASSERT_TRUE(shm.create(SLOT_SIZE * 100));
    SessionPipeline pipeline(shm);

    std::vector<uint8_t> payload(1400, 0xAB);
    uint32_t decoded_id = 0;
    auto outcome = pipeline.handle_data_packet("never-opened", 0, 0,
                                                payload.data(), payload.size(), &decoded_id);
    ASSERT_TRUE(outcome == DataPacketOutcome::UnknownSession);
}

static void test_data_packet_invalid_symbol_and_block() {
    TEST(data_packet_invalid_symbol_and_block);

    const std::string path = test_file_path("invalid.bin");
    make_fallocated_file(path, 1400ull * 200);

    ShmManager shm;
    ASSERT_TRUE(shm.create(SLOT_SIZE * 100));
    SessionPipeline pipeline(shm);
    auto so = make_session_open("sess-invalid", path, 1, 200, 255, 1400);
    ASSERT_TRUE(pipeline.handle_session_open(so, 1400ull * 200));

    std::vector<uint8_t> payload(1400, 0xAB);
    uint32_t decoded_id = 0;

    // symbol_id >= n (255)
    ASSERT_TRUE(pipeline.handle_data_packet("sess-invalid", 0, 255,
                                             payload.data(), payload.size(), &decoded_id)
                == DataPacketOutcome::InvalidBlockOrSymbol);

    // block_id >= total_blocks (1)
    ASSERT_TRUE(pipeline.handle_data_packet("sess-invalid", 1, 0,
                                             payload.data(), payload.size(), &decoded_id)
                == DataPacketOutcome::InvalidBlockOrSymbol);

    // wrong payload size
    std::vector<uint8_t> short_payload(100, 0xAB);
    ASSERT_TRUE(pipeline.handle_data_packet("sess-invalid", 0, 0,
                                             short_payload.data(), short_payload.size(), &decoded_id)
                == DataPacketOutcome::InvalidBlockOrSymbol);
}

static void test_single_registration_then_duplicate() {
    TEST(single_registration_then_duplicate);

    const std::string path = test_file_path("dup.bin");
    make_fallocated_file(path, 1400ull * 200);

    ShmManager shm;
    ASSERT_TRUE(shm.create(SLOT_SIZE * 100));
    SessionPipeline pipeline(shm);
    auto so = make_session_open("sess-dup", path, 1, 200, 255, 1400);
    ASSERT_TRUE(pipeline.handle_session_open(so, 1400ull * 200));

    std::vector<uint8_t> payload(1400, 0x11);
    uint32_t decoded_id = 0;

    auto first = pipeline.handle_data_packet("sess-dup", 0, 0,
                                              payload.data(), payload.size(), &decoded_id);
    ASSERT_TRUE(first == DataPacketOutcome::RegisteredOnly); // 1 of 200 needed -- nowhere near k

    auto second = pipeline.handle_data_packet("sess-dup", 0, 0,
                                               payload.data(), payload.size(), &decoded_id);
    ASSERT_TRUE(second == DataPacketOutcome::Duplicate);
}

static void test_arena_exhaustion() {
    TEST(arena_exhaustion);

    const std::string path = test_file_path("exhaust.bin");
    make_fallocated_file(path, 1400ull * 200);

    ShmManager shm;
    ASSERT_TRUE(shm.create(SLOT_SIZE * 2)); // only 2 slots -- exhausts fast
    SessionPipeline pipeline(shm);
    auto so = make_session_open("sess-exhaust", path, 1, 200, 255, 1400);
    ASSERT_TRUE(pipeline.handle_session_open(so, 1400ull * 200));

    std::vector<uint8_t> payload(1400, 0x22);
    uint32_t decoded_id = 0;

    // Two distinct symbols fill the 2-slot arena.
    ASSERT_TRUE(pipeline.handle_data_packet("sess-exhaust", 0, 0,
                                             payload.data(), payload.size(), &decoded_id)
                == DataPacketOutcome::RegisteredOnly);
    ASSERT_TRUE(pipeline.handle_data_packet("sess-exhaust", 0, 1,
                                             payload.data(), payload.size(), &decoded_id)
                == DataPacketOutcome::RegisteredOnly);

    // A third, distinct symbol has nowhere to go.
    ASSERT_TRUE(pipeline.handle_data_packet("sess-exhaust", 0, 2,
                                             payload.data(), payload.size(), &decoded_id)
                == DataPacketOutcome::ArenaExhausted);
}

static void test_full_block_decode_all_symbols_present() {
    TEST(full_block_decode_all_symbols_present);

    // Small geometry for a fast test -- k/n/symbol_bytes are per-session
    // and this class must not care what they are (docs/ANSWERS_FROM_A.md
    // §10), so a small, easy-to-follow geometry proves the wiring just as
    // well as the real defaults would.
    const uint32_t k = 8, n = 12, symbol_bytes = 32;
    const std::string path = test_file_path("full_decode.bin");
    make_fallocated_file(path, uint64_t(k) * symbol_bytes); // exactly 1 block

    ShmManager shm;
    ASSERT_TRUE(shm.create(SLOT_SIZE * 100));
    SessionPipeline pipeline(shm);
    auto so = make_session_open("sess-full", path, 1, k, n, symbol_bytes);
    ASSERT_TRUE(pipeline.handle_session_open(so, uint64_t(k) * symbol_bytes));

    EncodedBlock block = make_encoded_block(k, n, symbol_bytes, 1);

    uint32_t decoded_id = 0;
    DataPacketOutcome last = DataPacketOutcome::RegisteredOnly;
    for (uint32_t s = 0; s < k; ++s) { // send exactly the k data symbols, in order
        last = pipeline.handle_data_packet("sess-full", 0, s,
                                            block.symbols[s].data(), symbol_bytes, &decoded_id);
    }
    ASSERT_TRUE(last == DataPacketOutcome::BlockDecoded);
    ASSERT_EQ(decoded_id, 0u);

    auto on_disk = read_whole_file(path);
    ASSERT_EQ(on_disk.size(), uint64_t(k) * symbol_bytes);
    for (uint32_t s = 0; s < k; ++s) {
        ASSERT_TRUE(std::memcmp(on_disk.data() + uint64_t(s) * symbol_bytes,
                                 block.symbols[s].data(), symbol_bytes) == 0);
    }
}

static void test_full_block_decode_recovered_from_parity() {
    TEST(full_block_decode_recovered_from_parity);

    // The scenario Phase 10 exists for: real symbol loss, recovered via
    // erasure coding, not just a pass-through of already-complete data.
    const uint32_t k = 8, n = 12, symbol_bytes = 32; // n-k=4 parity, tolerates up to 4 losses
    const std::string path = test_file_path("recovered.bin");
    make_fallocated_file(path, uint64_t(k) * symbol_bytes);

    ShmManager shm;
    ASSERT_TRUE(shm.create(SLOT_SIZE * 100));
    SessionPipeline pipeline(shm);
    auto so = make_session_open("sess-recovered", path, 1, k, n, symbol_bytes);
    ASSERT_TRUE(pipeline.handle_session_open(so, uint64_t(k) * symbol_bytes));

    EncodedBlock block = make_encoded_block(k, n, symbol_bytes, 2);

    // "Received" set: symbols 3,4,5,6 (4 of the 8 data symbols) are
    // "lost" -- send everything else: data symbols 0,1,2,7 plus all 4
    // parity symbols (8,9,10,11). That's exactly k=8 symbols, a genuine
    // mix of data and parity, requiring real matrix inversion to recover
    // the missing data symbols -- not the "all data present" fast path.
    std::vector<uint32_t> to_send = {0, 1, 2, 7, 8, 9, 10, 11};
    ASSERT_EQ(to_send.size(), static_cast<size_t>(k));

    uint32_t decoded_id = 0;
    DataPacketOutcome last = DataPacketOutcome::RegisteredOnly;
    for (uint32_t s : to_send) {
        last = pipeline.handle_data_packet("sess-recovered", 0, s,
                                            block.symbols[s].data(), symbol_bytes, &decoded_id);
    }
    ASSERT_TRUE(last == DataPacketOutcome::BlockDecoded);

    auto on_disk = read_whole_file(path);
    for (uint32_t s = 0; s < k; ++s) {
        ASSERT_TRUE(std::memcmp(on_disk.data() + uint64_t(s) * symbol_bytes,
                                 block.symbols[s].data(), symbol_bytes) == 0);
    }
}

static void test_final_partial_block_end_to_end() {
    TEST(final_partial_block_end_to_end);

    // Two blocks; the file ends partway through block 1 -- exercises the
    // Property 5 / clip-to-file_size path through the WHOLE pipeline, not
    // just BlockWriter in isolation (test_block_writer.cpp already covers
    // that directly).
    const uint32_t k = 4, n = 6, symbol_bytes = 16;
    const uint64_t block_bytes = uint64_t(k) * symbol_bytes; // 64
    const uint64_t file_size = block_bytes + 20; // block 1 has only 20 real bytes
    const std::string path = test_file_path("partial_e2e.bin");
    make_fallocated_file(path, file_size);

    ShmManager shm;
    ASSERT_TRUE(shm.create(SLOT_SIZE * 100));
    SessionPipeline pipeline(shm);
    auto so = make_session_open("sess-partial", path, 2, k, n, symbol_bytes);
    ASSERT_TRUE(pipeline.handle_session_open(so, file_size));

    EncodedBlock block0 = make_encoded_block(k, n, symbol_bytes, 3);
    EncodedBlock block1 = make_encoded_block(k, n, symbol_bytes, 4);

    uint32_t decoded_id = 0;
    for (uint32_t s = 0; s < k; ++s) {
        pipeline.handle_data_packet("sess-partial", 0, s, block0.symbols[s].data(), symbol_bytes, &decoded_id);
    }
    DataPacketOutcome last = DataPacketOutcome::RegisteredOnly;
    for (uint32_t s = 0; s < k; ++s) {
        last = pipeline.handle_data_packet("sess-partial", 1, s, block1.symbols[s].data(), symbol_bytes, &decoded_id);
    }
    ASSERT_TRUE(last == DataPacketOutcome::BlockDecoded);
    ASSERT_EQ(decoded_id, 1u);

    auto on_disk = read_whole_file(path);
    ASSERT_EQ(on_disk.size(), file_size); // must NOT have grown past file_size

    // Block 0 fully real.
    for (uint32_t s = 0; s < k; ++s) {
        ASSERT_TRUE(std::memcmp(on_disk.data() + uint64_t(s) * symbol_bytes,
                                 block0.symbols[s].data(), symbol_bytes) == 0);
    }
    // Block 1: only the first 20 bytes are real content.
    ASSERT_TRUE(std::memcmp(on_disk.data() + block_bytes, block1.symbols[0].data(), 16) == 0);
    ASSERT_TRUE(std::memcmp(on_disk.data() + block_bytes + 16, block1.symbols[1].data(), 4) == 0);
}

static void test_two_sessions_independent() {
    TEST(two_sessions_independent);

    const uint32_t k = 4, n = 6, symbol_bytes = 16;
    const std::string path_a = test_file_path("indep_a.bin");
    const std::string path_b = test_file_path("indep_b.bin");
    make_fallocated_file(path_a, uint64_t(k) * symbol_bytes);
    make_fallocated_file(path_b, uint64_t(k) * symbol_bytes);

    ShmManager shm;
    ASSERT_TRUE(shm.create(SLOT_SIZE * 100));
    SessionPipeline pipeline(shm);

    auto so_a = make_session_open("sess-a", path_a, 1, k, n, symbol_bytes);
    auto so_b = make_session_open("sess-b", path_b, 1, k, n, symbol_bytes);
    ASSERT_TRUE(pipeline.handle_session_open(so_a, uint64_t(k) * symbol_bytes));
    ASSERT_TRUE(pipeline.handle_session_open(so_b, uint64_t(k) * symbol_bytes));

    std::vector<uint8_t> payload(symbol_bytes, 0x77);
    uint32_t decoded_id = 0;

    // Register block 0 symbol 0 in session A only.
    ASSERT_TRUE(pipeline.handle_data_packet("sess-a", 0, 0, payload.data(), symbol_bytes, &decoded_id)
                == DataPacketOutcome::RegisteredOnly);

    // Session B's block 0 symbol 0 must be entirely unaffected -- the
    // real correctness property the ShmManager multi-session fix exists
    // for, proven here at the pipeline level too, not just inside
    // ShmManager's own tests.
    ASSERT_TRUE(pipeline.handle_data_packet("sess-b", 0, 0, payload.data(), symbol_bytes, &decoded_id)
                == DataPacketOutcome::RegisteredOnly); // still NEW in session B, not a duplicate
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "=== Session Pipeline Tests (Phase 9) ===\n\n";

    RUN(test_session_open_creates_context_and_is_idempotent);
    RUN(test_session_open_fails_on_missing_file);
    RUN(test_purge_session_drops_context_and_unknown_id_is_noop);
    RUN(test_data_packet_unknown_session);
    RUN(test_data_packet_invalid_symbol_and_block);
    RUN(test_single_registration_then_duplicate);
    RUN(test_arena_exhaustion);
    RUN(test_full_block_decode_all_symbols_present);
    RUN(test_full_block_decode_recovered_from_parity);
    RUN(test_final_partial_block_end_to_end);
    RUN(test_two_sessions_independent);

    std::cout << "\n=== Results: " << g_pass << " run, "
              << g_fail << " failed ===\n";

    ::shm_unlink(SHM_NAME);

    return g_fail == 0 ? 0 : 1;
}
