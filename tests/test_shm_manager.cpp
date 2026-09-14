#include "receiver/shm_manager.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

// =============================================================================
// Minimal test harness
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

#define ASSERT_STREQ(a, b)                                              \
    do {                                                                \
        if (std::strcmp((a), (b)) != 0) {                               \
            std::cerr << "  FAIL: strcmp(" << #a << ", " << #b << ")"  \
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

// --- Layout computation (no SHM needed) ---

static void test_layout_calculation() {
    TEST(layout_calculation);

    const uint64_t arena_bytes = DEFAULT_ARENA_BYTES; // 256 MB

    uint64_t arena_off = ShmManager::compute_arena_offset();
    uint64_t total      = ShmManager::compute_total_size(arena_bytes);

    // Session table right after the header.
    uint64_t expected_session = sizeof(ShmHeader);
    // The whole per-session block-table+bitmap region right after the
    // session table (aligned) -- MAX_SESSIONS copies of per_session_stride().
    uint64_t expected_block_region = ALIGN8(expected_session
                                            + MAX_SESSIONS * sizeof(SessionEntry));
    uint64_t expected_arena = expected_block_region
                             + uint64_t(MAX_SESSIONS) * per_session_stride();

    ASSERT_EQ(arena_off, expected_arena);

    uint64_t expected_total = expected_arena + arena_bytes;
    ASSERT_EQ(total, expected_total);

    // All offsets must be 8-byte aligned (BlockEntry leads with atomics).
    ASSERT_TRUE((expected_session       % 8) == 0);
    ASSERT_TRUE((expected_block_region  % 8) == 0);
    ASSERT_TRUE((expected_arena         % 8) == 0);
    ASSERT_TRUE((per_session_stride()   % 8) == 0);

    std::cout << "    layout: session_off=" << expected_session
              << " block_region_off=" << expected_block_region
              << " per_session_stride=" << per_session_stride()
              << " arena_off=" << expected_arena
              << " total=" << total << "\n";
}

static void test_tiny_arena_layout() {
    TEST(tiny_arena_layout);

    // A tiny arena (5 slots) -- the per-session block-table capacity is a
    // fixed cost regardless (MAX_BLOCKS_PER_SESSION), so this mostly
    // proves compute_total_size() composes the two costs correctly at a
    // small scale, not that block-table capacity shrinks with it.
    const uint64_t arena_bytes = SLOT_SIZE * 5;

    uint64_t total = ShmManager::compute_total_size(arena_bytes);
    uint64_t arena = ShmManager::compute_arena_offset();

    ASSERT_TRUE(total >= sizeof(ShmHeader) + 5 * SLOT_SIZE);
    ASSERT_TRUE(arena >= sizeof(ShmHeader));
    ASSERT_EQ(total, arena + arena_bytes);
}

// --- SHM create / open / close ---

static void test_create_open_close() {
    TEST(create_open_close);

    ShmManager mgr;
    ASSERT_TRUE(mgr.create(SLOT_SIZE * 10));

    // Header should be valid immediately after create
    ASSERT_TRUE(mgr.validate());
    ASSERT_EQ(mgr.header()->magic, SHM_MAGIC);
    ASSERT_EQ(mgr.header()->version, SHM_VERSION);
    ASSERT_EQ(mgr.header()->slot_count, 10u);

    // Close and reopen
    mgr.close();
    ASSERT_TRUE(mgr.open());
    ASSERT_TRUE(mgr.validate());
    ASSERT_EQ(mgr.header()->magic, SHM_MAGIC);

    mgr.close();
}

static void test_create_default_config() {
    TEST(create_default_config);

    ShmManager mgr;
    ASSERT_TRUE(mgr.create(DEFAULT_ARENA_BYTES));
    ASSERT_TRUE(mgr.validate());

    const ShmHeader* h = mgr.header();
    ASSERT_EQ(h->slot_size, SLOT_SIZE);

    uint64_t expected_total = ShmManager::compute_total_size(DEFAULT_ARENA_BYTES);
    ASSERT_EQ(h->total_size, expected_total);

    uint32_t expected_slots =
        static_cast<uint32_t>(DEFAULT_ARENA_BYTES / SLOT_SIZE);
    ASSERT_EQ(h->slot_count, expected_slots);

    ASSERT_TRUE(mgr.raw_base() != nullptr);
    ASSERT_TRUE(mgr.header()   != nullptr);

    mgr.close();
}

// --- Slot access ---

static void test_slot_access() {
    TEST(slot_access);

    ShmManager mgr;
    ASSERT_TRUE(mgr.create(SLOT_SIZE * 10));

    SlotView sv0 = mgr.slot(0);
    std::memset(sv0.data(), 0xAB, SLOT_SIZE);

    SlotView sv5 = mgr.slot(5);
    std::memset(sv5.data(), 0xCD, SLOT_SIZE);

    const uint8_t* p0 = static_cast<const uint8_t*>(sv0.data());
    const uint8_t* p5 = static_cast<const uint8_t*>(sv5.data());

    ASSERT_EQ(p0[0], 0xAB);
    ASSERT_EQ(p0[SLOT_SIZE - 1], 0xAB);
    ASSERT_EQ(p5[0], 0xCD);
    ASSERT_EQ(p5[SLOT_SIZE - 1], 0xCD);

    ASSERT_EQ(sv0.index(), 0u);
    ASSERT_EQ(sv5.index(), 5u);
    ASSERT_TRUE(sv0.data() != sv5.data());

    uint64_t off0 = mgr.slot_offset(0);
    uint64_t off5 = mgr.slot_offset(5);
    ASSERT_TRUE(off5 > off0);
    ASSERT_EQ(off5 - off0, 5 * SLOT_SIZE);

    uint64_t off1 = mgr.slot_offset(1);
    ASSERT_TRUE(off1 - off0 >= SLOT_SIZE);

    mgr.close();
}

static void test_slot_out_of_bounds() {
    TEST(slot_out_of_bounds);

    ShmManager mgr;
    ASSERT_TRUE(mgr.create(SLOT_SIZE * 10));

    uint64_t off = mgr.slot_offset(10); // one past last
    uint64_t expected = mgr.header()->arena_offset + 10 * SLOT_SIZE;
    ASSERT_EQ(off, expected);

    mgr.close();
}

// --- Session table ---

static void test_session_table_access() {
    TEST(session_table_access);

    ShmManager mgr;
    ASSERT_TRUE(mgr.create(SLOT_SIZE * 10));

    // All sessions should start as EMPTY
    for (uint32_t i = 0; i < MAX_SESSIONS; ++i) {
        SessionView sv = mgr.session(i);
        ASSERT_EQ(sv.state(), SESSION_STATE_EMPTY);
    }

    // open_session() is now the only correct way to populate a slot --
    // it's what claims a session_idx, computes that session's
    // block_table_offset/bitmap_offset, and publishes the entry.
    auto idx = mgr.open_session("test-session-42", 200, 255, 1400, 100,
                                 1024ull * 1024, "/staging/test-session-42/out.bin");
    ASSERT_TRUE(idx.has_value());

    SessionView sv0 = mgr.session(*idx);
    ASSERT_STREQ(sv0.session_id(), "test-session-42");
    ASSERT_EQ(sv0.state(), SESSION_STATE_OPEN);
    ASSERT_EQ(sv0.k(), 200u);
    ASSERT_EQ(sv0.n(), 255u);
    ASSERT_EQ(sv0.symbol_bytes(), 1400u);
    ASSERT_EQ(sv0.total_blocks(), 100u);
    ASSERT_EQ(sv0.file_size(), 1024ULL * 1024);
    ASSERT_STREQ(sv0.dest_path(), "/staging/test-session-42/out.bin");

    SessionEntry* found = mgr.find_session("test-session-42");
    ASSERT_TRUE(found != nullptr);
    ASSERT_EQ(found->state.load(), SESSION_STATE_OPEN);

    ASSERT_TRUE(mgr.find_session("nonexistent") == nullptr);

    mgr.close();
}

static void test_session_index_out_of_range() {
    TEST(session_index_out_of_range);

    ShmManager mgr;
    ASSERT_TRUE(mgr.create(SLOT_SIZE * 10));

    ASSERT_TRUE(mgr.session_entry(MAX_SESSIONS) == nullptr);
    ASSERT_TRUE(mgr.session_entry(MAX_SESSIONS + 100) == nullptr);

    mgr.close();
}

// --- open_session() (Phase 9) ---

static void test_open_session_basic_fields() {
    TEST(open_session_basic_fields);

    ShmManager mgr;
    ASSERT_TRUE(mgr.create(SLOT_SIZE * 10));

    auto idx = mgr.open_session("s1", 200, 255, 1400, 50, 12345, "/out/s1.bin");
    ASSERT_TRUE(idx.has_value());
    ASSERT_TRUE(*idx < MAX_SESSIONS);

    SessionEntry* se = mgr.session_entry(*idx);
    ASSERT_TRUE(se != nullptr);
    ASSERT_EQ(se->state.load(), SESSION_STATE_OPEN);
    // This session's regions must land inside the block-table capacity
    // region and before the arena -- not overlapping either neighbor.
    ASSERT_TRUE(se->block_table_offset >= mgr.header()->block_table_region_offset);
    ASSERT_TRUE(se->bitmap_offset > se->block_table_offset);
    ASSERT_TRUE(se->bitmap_offset < mgr.header()->arena_offset);

    mgr.close();
}

static void test_open_session_idempotent_same_id() {
    TEST(open_session_idempotent_same_id);

    ShmManager mgr;
    ASSERT_TRUE(mgr.create(SLOT_SIZE * 10));

    auto idx1 = mgr.open_session("s1", 200, 255, 1400, 50, 0, "/out/s1.bin");
    auto idx2 = mgr.open_session("s1", 200, 255, 1400, 50, 0, "/out/s1.bin");
    ASSERT_TRUE(idx1.has_value());
    ASSERT_TRUE(idx2.has_value());
    ASSERT_EQ(*idx1, *idx2); // same session_id -> same slot, not a second entry

    // Only one slot should actually be OPEN.
    uint32_t open_count = 0;
    for (uint32_t i = 0; i < MAX_SESSIONS; ++i) {
        if (mgr.session_entry(i)->state.load() == SESSION_STATE_OPEN) ++open_count;
    }
    ASSERT_EQ(open_count, 1u);

    mgr.close();
}

static void test_open_session_two_distinct_sessions_get_distinct_regions() {
    TEST(open_session_two_distinct_sessions_get_distinct_regions);

    ShmManager mgr;
    ASSERT_TRUE(mgr.create(SLOT_SIZE * 10));

    auto a = mgr.open_session("session-a", 200, 255, 1400, 10, 0, "/out/a.bin");
    auto b = mgr.open_session("session-b", 200, 255, 1400, 10, 0, "/out/b.bin");
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    ASSERT_TRUE(*a != *b);

    SessionEntry* sa = mgr.session_entry(*a);
    SessionEntry* sb = mgr.session_entry(*b);
    ASSERT_TRUE(sa->block_table_offset != sb->block_table_offset);

    // Registering into session A's block 0 must not be visible through
    // session B's block 0 -- the actual correctness property all of this
    // exists for (this is exactly the bug the old flat, single-array
    // block_entry() had: two sessions colliding on the same block_id).
    BlockView bva = mgr.block(*a, 0).value();
    BlockView bvb = mgr.block(*b, 0).value();
    ASSERT_TRUE(bva.register_symbol(0, 111) == SymbolRegisterResult::NewSymbol);
    ASSERT_TRUE(!bvb.is_present(0));
    ASSERT_EQ(bvb.seen(), 0u);

    mgr.close();
}

static void test_open_session_rejects_over_capacity() {
    TEST(open_session_rejects_over_capacity);

    ShmManager mgr;
    ASSERT_TRUE(mgr.create(SLOT_SIZE * 10));

    auto idx = mgr.open_session("too-big", 200, 255, 1400,
                                 MAX_BLOCKS_PER_SESSION + 1, 0, "/out/big.bin");
    ASSERT_TRUE(!idx.has_value());

    // Exactly at the cap must still succeed.
    auto ok = mgr.open_session("at-cap", 200, 255, 1400,
                                MAX_BLOCKS_PER_SESSION, 0, "/out/cap.bin");
    ASSERT_TRUE(ok.has_value());

    mgr.close();
}

static void test_open_session_exhausts_max_sessions() {
    TEST(open_session_exhausts_max_sessions);

    ShmManager mgr;
    ASSERT_TRUE(mgr.create(SLOT_SIZE * 10));

    for (uint32_t i = 0; i < MAX_SESSIONS; ++i) {
        char id[32];
        std::snprintf(id, sizeof(id), "sess-%u", i);
        auto idx = mgr.open_session(id, 200, 255, 1400, 10, 0, "/out/x.bin");
        ASSERT_TRUE(idx.has_value());
    }

    // MAX_SESSIONS slots are all taken now -- one more, a genuinely new
    // session_id, must fail cleanly rather than overwrite one.
    auto overflow = mgr.open_session("one-too-many", 200, 255, 1400, 10, 0, "/out/y.bin");
    ASSERT_TRUE(!overflow.has_value());

    mgr.close();
}

static void test_open_session_concurrent_same_id_one_entry() {
    TEST(open_session_concurrent_same_id_one_entry);

    // Simulates the REAL scenario this whole claim-then-verify design
    // exists for: session_manager broadcasts one SessionOpen to every
    // connected receiver, so multiple receiver PROCESSES (here, threads
    // racing the same ShmManager -- the cross-process case goes through
    // identical atomic operations on the same mapped memory, so this is
    // the right level to prove the algorithm at) call open_session() for
    // the SAME session_id at effectively the same instant.
    constexpr int kThreads = 8;

    ShmManager mgr;
    ASSERT_TRUE(mgr.create(SLOT_SIZE * 10));

    std::vector<std::optional<uint32_t>> results(kThreads);
    {
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t]() {
                results[t] = mgr.open_session("race-session", 200, 255, 1400,
                                               10, 0, "/out/race.bin");
            });
        }
        for (auto& th : threads) th.join();
    }

    // Every thread must have succeeded, and every thread must have gotten
    // the SAME slot index -- not "any valid index," the SAME one, since
    // there must be exactly one entry for this session_id.
    for (int t = 0; t < kThreads; ++t) {
        ASSERT_TRUE(results[t].has_value());
        ASSERT_EQ(*results[t], *results[0]);
    }

    uint32_t open_count = 0;
    for (uint32_t i = 0; i < MAX_SESSIONS; ++i) {
        if (mgr.session_entry(i)->state.load() == SESSION_STATE_OPEN) ++open_count;
    }
    ASSERT_EQ(open_count, 1u); // never two entries for one session_id

    mgr.close();
}

static void test_open_session_concurrent_distinct_ids_all_succeed() {
    TEST(open_session_concurrent_distinct_ids_all_succeed);

    constexpr int kThreads = MAX_SESSIONS; // exactly fills the table

    ShmManager mgr;
    ASSERT_TRUE(mgr.create(SLOT_SIZE * 10));

    std::vector<std::optional<uint32_t>> results(kThreads);
    {
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t]() {
                char id[32];
                std::snprintf(id, sizeof(id), "distinct-%d", t);
                results[t] = mgr.open_session(id, 200, 255, 1400, 10, 0, "/out/d.bin");
            });
        }
        for (auto& th : threads) th.join();
    }

    // All must succeed, and all must land on DISTINCT slots -- no two
    // different session_ids sharing a slot (which would silently corrupt
    // whichever one lost).
    std::vector<bool> slot_used(MAX_SESSIONS, false);
    for (int t = 0; t < kThreads; ++t) {
        ASSERT_TRUE(results[t].has_value());
        ASSERT_TRUE(!slot_used[*results[t]]);
        slot_used[*results[t]] = true;
    }

    mgr.close();
}

// --- Block table ---

static void test_block_table_access() {
    TEST(block_table_access);

    ShmManager mgr;
    ASSERT_TRUE(mgr.create(SLOT_SIZE * 10));

    auto idx = mgr.open_session("s1", 200, 255, 1400, 50, 0, "/out/s1.bin");
    ASSERT_TRUE(idx.has_value());
    const uint32_t session_idx = *idx;

    BlockEntry* be = mgr.block_entry(session_idx, 0);
    ASSERT_TRUE(be != nullptr);

    BlockEntry* be49 = mgr.block_entry(session_idx, 49);
    ASSERT_TRUE(be49 != nullptr);

    // Block 50 is out of range (total_blocks=50, valid: 0..49)
    ASSERT_TRUE(mgr.block_entry(session_idx, 50) == nullptr);

    // An unopened session index.
    uint32_t unopened = (session_idx + 1) % MAX_SESSIONS;
    ASSERT_TRUE(mgr.block_entry(unopened, 0) == nullptr);

    ASSERT_TRUE(mgr.block(session_idx, 0).has_value());
    ASSERT_TRUE(mgr.block(session_idx, 49).has_value());
    ASSERT_TRUE(!mgr.block(session_idx, 50).has_value());
    ASSERT_TRUE(!mgr.block(unopened, 0).has_value());

    mgr.close();
}

static void test_block_view_operations() {
    TEST(block_view_operations);

    ShmManager mgr;
    ASSERT_TRUE(mgr.create(SLOT_SIZE * 10));

    auto idx = mgr.open_session("s1", 200, 255, 1400, 10, 0, "/out/s1.bin");
    ASSERT_TRUE(idx.has_value());

    BlockView bv = mgr.block(*idx, 0).value();

    ASSERT_EQ(bv.decode_state(), DECODE_PENDING);
    ASSERT_EQ(bv.seen(), 0u);

    ASSERT_TRUE(bv.try_claim_decode());
    ASSERT_EQ(bv.decode_state(), DECODE_CLAIMED);

    ASSERT_EQ(bv.slot_idx(MAX_N), SLOT_IDX_FREE);
    ASSERT_TRUE(bv.register_symbol(MAX_N, 0) == SymbolRegisterResult::InvalidSymbolId);

    ASSERT_TRUE(!bv.is_present(0));

    ASSERT_TRUE(bv.register_symbol(0, 42) == SymbolRegisterResult::NewSymbol);
    ASSERT_EQ(bv.slot_idx(0), 42u);
    ASSERT_TRUE(bv.is_present(0));

    mgr.close();
}

// --- Symbol registration & duplicate suppression (Phase 5) ---

static void test_register_sequence_increments_seen() {
    TEST(register_sequence_increments_seen);

    ShmManager mgr;
    ASSERT_TRUE(mgr.create(SLOT_SIZE * 4));
    auto idx = mgr.open_session("s1", 200, 255, 1400, 1, 0, "/out/s1.bin");
    ASSERT_TRUE(idx.has_value());

    BlockView bv = mgr.block(*idx, 0).value();

    ASSERT_TRUE(bv.register_symbol(0, 100) == SymbolRegisterResult::NewSymbol);
    ASSERT_EQ(bv.seen(), 1u);
    ASSERT_TRUE(bv.register_symbol(1, 101) == SymbolRegisterResult::NewSymbol);
    ASSERT_EQ(bv.seen(), 2u);
    ASSERT_TRUE(bv.register_symbol(2, 102) == SymbolRegisterResult::NewSymbol);
    ASSERT_EQ(bv.seen(), 3u);

    ASSERT_TRUE(bv.is_present(0));
    ASSERT_TRUE(bv.is_present(1));
    ASSERT_TRUE(bv.is_present(2));
    ASSERT_TRUE(!bv.is_present(3));

    ASSERT_TRUE(bv.register_symbol(0, 999) == SymbolRegisterResult::Duplicate);
    ASSERT_EQ(bv.seen(), 3u);

    mgr.close();
}

static void test_register_duplicate_preserves_original_slot() {
    TEST(register_duplicate_preserves_original_slot);

    ShmManager mgr;
    ASSERT_TRUE(mgr.create(SLOT_SIZE * 4));
    auto idx = mgr.open_session("s1", 200, 255, 1400, 1, 0, "/out/s1.bin");
    ASSERT_TRUE(idx.has_value());

    BlockView bv = mgr.block(*idx, 0).value();

    ASSERT_TRUE(bv.register_symbol(5, 10) == SymbolRegisterResult::NewSymbol);
    ASSERT_EQ(bv.slot_idx(5), 10u);

    ASSERT_TRUE(bv.register_symbol(5, 77) == SymbolRegisterResult::Duplicate);
    ASSERT_EQ(bv.slot_idx(5), 10u);
    ASSERT_EQ(bv.seen(), 1u);

    mgr.close();
}

static void test_register_invalid_symbol_id() {
    TEST(register_invalid_symbol_id);

    ShmManager mgr;
    ASSERT_TRUE(mgr.create(SLOT_SIZE * 4));
    auto idx = mgr.open_session("s1", 200, 255, 1400, 1, 0, "/out/s1.bin");
    ASSERT_TRUE(idx.has_value());

    BlockView bv = mgr.block(*idx, 0).value();

    ASSERT_TRUE(bv.register_symbol(MAX_N, 0) == SymbolRegisterResult::InvalidSymbolId);
    ASSERT_TRUE(bv.register_symbol(0xFFFFFFFFu, 0) == SymbolRegisterResult::InvalidSymbolId);
    ASSERT_EQ(bv.seen(), 0u);

    mgr.close();
}

// --- Concurrent registration: many threads racing on one block ---

static void test_register_concurrent_same_symbol_one_winner() {
    TEST(register_concurrent_same_symbol_one_winner);

    constexpr int kThreads = 8;

    ShmManager mgr;
    ASSERT_TRUE(mgr.create(SLOT_SIZE * 4));
    auto idx = mgr.open_session("s1", 200, 255, 1400, 1, 0, "/out/s1.bin");
    ASSERT_TRUE(idx.has_value());

    BlockView bv = mgr.block(*idx, 0).value();

    std::atomic<int> new_count{0};
    std::vector<uint32_t> winning_slot(kThreads, SLOT_IDX_FREE);

    {
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t]() {
                auto result = bv.register_symbol(17, static_cast<uint32_t>(t));
                if (result == SymbolRegisterResult::NewSymbol) {
                    new_count.fetch_add(1, std::memory_order_relaxed);
                    winning_slot[t] = static_cast<uint32_t>(t);
                }
            });
        }
        for (auto& th : threads) th.join();
    }

    ASSERT_EQ(new_count.load(), 1);
    ASSERT_EQ(bv.seen(), 1u);
    ASSERT_TRUE(bv.is_present(17));

    uint32_t recorded = bv.slot_idx(17);
    bool matches_a_winner = false;
    for (int t = 0; t < kThreads; ++t) {
        if (winning_slot[t] == recorded) matches_a_winner = true;
    }
    ASSERT_TRUE(matches_a_winner);

    mgr.close();
}

static void test_register_concurrent_distinct_symbols_all_new() {
    TEST(register_concurrent_distinct_symbols_all_new);

    constexpr int      kThreads = 8;
    constexpr uint32_t kPerThread = 25;

    ShmManager mgr;
    ASSERT_TRUE(mgr.create(SLOT_SIZE * 4));
    auto idx = mgr.open_session("s1", 200, 255, 1400, 1, 0, "/out/s1.bin");
    ASSERT_TRUE(idx.has_value());

    BlockView bv = mgr.block(*idx, 0).value();

    std::atomic<uint32_t> new_count{0};

    {
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t]() {
                const uint32_t base = static_cast<uint32_t>(t) * kPerThread;
                for (uint32_t i = 0; i < kPerThread; ++i) {
                    const uint32_t symbol_id = base + i;
                    auto result = bv.register_symbol(symbol_id, symbol_id);
                    if (result == SymbolRegisterResult::NewSymbol) {
                        new_count.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (auto& th : threads) th.join();
    }

    const uint32_t total = kThreads * kPerThread;
    ASSERT_EQ(new_count.load(), total);
    ASSERT_EQ(bv.seen(), total);

    for (uint32_t i = 0; i < total; ++i) {
        ASSERT_TRUE(bv.is_present(i));
        ASSERT_EQ(bv.slot_idx(i), i);
    }
    ASSERT_TRUE(!bv.is_present(total));

    mgr.close();
}

// --- Decode claim (Phase 6) ---

static void test_claim_decode_single_winner_sequential() {
    TEST(claim_decode_single_winner_sequential);

    ShmManager mgr;
    ASSERT_TRUE(mgr.create(SLOT_SIZE * 4));
    auto idx = mgr.open_session("s1", 200, 255, 1400, 1, 0, "/out/s1.bin");
    ASSERT_TRUE(idx.has_value());

    BlockView bv = mgr.block(*idx, 0).value();

    ASSERT_EQ(bv.decode_state(), DECODE_PENDING);
    ASSERT_TRUE(bv.try_claim_decode());
    ASSERT_EQ(bv.decode_state(), DECODE_CLAIMED);
    ASSERT_TRUE(!bv.try_claim_decode());
    ASSERT_EQ(bv.decode_state(), DECODE_CLAIMED);

    mgr.close();
}

static void test_claim_decode_lifecycle_and_misuse() {
    TEST(claim_decode_lifecycle_and_misuse);

    ShmManager mgr;
    ASSERT_TRUE(mgr.create(SLOT_SIZE * 4));
    auto idx = mgr.open_session("s1", 200, 255, 1400, 1, 0, "/out/s1.bin");
    ASSERT_TRUE(idx.has_value());

    BlockView bv = mgr.block(*idx, 0).value();

    ASSERT_TRUE(!bv.mark_decode_complete());
    ASSERT_EQ(bv.decode_state(), DECODE_PENDING);

    ASSERT_TRUE(bv.try_claim_decode());
    ASSERT_EQ(bv.decode_state(), DECODE_CLAIMED);

    ASSERT_TRUE(bv.mark_decode_complete());
    ASSERT_EQ(bv.decode_state(), DECODE_COMPLETE);

    ASSERT_TRUE(!bv.mark_decode_complete());
    ASSERT_EQ(bv.decode_state(), DECODE_COMPLETE);

    ASSERT_TRUE(!bv.try_claim_decode());
    ASSERT_EQ(bv.decode_state(), DECODE_COMPLETE);

    mgr.close();
}

static void test_claim_decode_concurrent_one_winner() {
    TEST(claim_decode_concurrent_one_winner);

    constexpr int kThreads = 8;

    ShmManager mgr;
    ASSERT_TRUE(mgr.create(SLOT_SIZE * 4));
    auto idx = mgr.open_session("s1", 200, 255, 1400, 1, 0, "/out/s1.bin");
    ASSERT_TRUE(idx.has_value());

    BlockView bv = mgr.block(*idx, 0).value();

    std::atomic<int> claim_count{0};

    {
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&]() {
                if (bv.try_claim_decode()) {
                    claim_count.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& th : threads) th.join();
    }

    ASSERT_EQ(claim_count.load(), 1);
    ASSERT_EQ(bv.decode_state(), DECODE_CLAIMED);

    ASSERT_TRUE(bv.mark_decode_complete());
    ASSERT_EQ(bv.decode_state(), DECODE_COMPLETE);

    mgr.close();
}

// --- Bounds validation ---

static void test_bounds_validation() {
    TEST(bounds_validation);

    ShmManager mgr;
    ASSERT_TRUE(mgr.create(SLOT_SIZE * 10));

    const ShmHeader* h = mgr.header();

    ASSERT_TRUE(h->session_table_offset      < h->total_size);
    ASSERT_TRUE(h->block_table_region_offset < h->total_size);
    ASSERT_TRUE(h->arena_offset              < h->total_size);

    uint64_t arena_end = h->arena_offset + uint64_t(h->slot_count) * h->slot_size;
    ASSERT_EQ(arena_end, h->total_size);

    // The whole per-session capacity region must fit before the arena.
    uint64_t block_region_end = h->block_table_region_offset
                               + uint64_t(MAX_SESSIONS) * per_session_stride();
    ASSERT_EQ(block_region_end, h->arena_offset);

    ASSERT_TRUE(mgr.offsets_valid());

    mgr.close();
}

// --- Arena capacity ---

static void test_arena_capacity() {
    TEST(arena_capacity);

    ShmManager mgr;
    ASSERT_TRUE(mgr.create(SLOT_SIZE * 10));

    ASSERT_TRUE(mgr.arena_has_capacity());
    ASSERT_TRUE(mgr.free_slot_count() > 0);

    mgr.close();
}

// --- Slot allocator (Phase 4): allocate/exhaust/release/reuse ---

static void test_alloc_exhaustion() {
    TEST(alloc_exhaustion);

    ShmManager mgr;
    ASSERT_TRUE(mgr.create(SLOT_SIZE * 5)); // 5 slots

    ASSERT_EQ(mgr.used_slot_count(), 0u);
    ASSERT_EQ(mgr.free_slot_count(), 5u);

    uint32_t seen[5];
    for (int i = 0; i < 5; ++i) {
        uint32_t idx = mgr.alloc_slot();
        ASSERT_TRUE(idx != SLOT_IDX_FREE);
        seen[i] = idx;
    }

    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(seen[i] < 5u);
        for (int j = i + 1; j < 5; ++j) {
            ASSERT_TRUE(seen[i] != seen[j]);
        }
    }

    ASSERT_EQ(mgr.used_slot_count(), 5u);
    ASSERT_EQ(mgr.free_slot_count(), 0u);
    ASSERT_TRUE(!mgr.arena_has_capacity());

    ASSERT_EQ(mgr.alloc_slot(), SLOT_IDX_FREE);
    ASSERT_EQ(mgr.alloc_slot(), SLOT_IDX_FREE);

    mgr.close();
}

static void test_alloc_free_reuse() {
    TEST(alloc_free_reuse);

    ShmManager mgr;
    ASSERT_TRUE(mgr.create(SLOT_SIZE * 3)); // 3 slots

    uint32_t a = mgr.alloc_slot();
    uint32_t b = mgr.alloc_slot();
    uint32_t c = mgr.alloc_slot();
    ASSERT_TRUE(a != SLOT_IDX_FREE && b != SLOT_IDX_FREE && c != SLOT_IDX_FREE);
    ASSERT_EQ(mgr.alloc_slot(), SLOT_IDX_FREE);

    mgr.free_slot(b);
    ASSERT_EQ(mgr.free_slot_count(), 1u);
    ASSERT_EQ(mgr.used_slot_count(), 2u);

    uint32_t reused = mgr.alloc_slot();
    ASSERT_TRUE(reused != SLOT_IDX_FREE);
    ASSERT_EQ(mgr.alloc_slot(), SLOT_IDX_FREE);

    SlotView sv = mgr.slot(reused);
    std::memset(sv.data(), 0x5A, 16);
    const uint8_t* p = static_cast<const uint8_t*>(mgr.slot(reused).data());
    for (int i = 0; i < 16; ++i) ASSERT_EQ(p[i], 0x5A);

    (void)a; (void)c;
    mgr.close();
}

static void test_high_water_tracking() {
    TEST(high_water_tracking);

    ShmManager mgr;
    ASSERT_TRUE(mgr.create(SLOT_SIZE * 5)); // 5 slots

    ASSERT_EQ(mgr.high_water_used_count(), 0u);

    uint32_t s0 = mgr.alloc_slot();
    uint32_t s1 = mgr.alloc_slot();
    mgr.alloc_slot();
    ASSERT_EQ(mgr.used_slot_count(), 3u);
    ASSERT_EQ(mgr.high_water_used_count(), 3u);

    mgr.free_slot(s0);
    mgr.free_slot(s1);
    ASSERT_EQ(mgr.used_slot_count(), 1u);
    ASSERT_EQ(mgr.high_water_used_count(), 3u);

    mgr.alloc_slot();
    ASSERT_EQ(mgr.used_slot_count(), 2u);
    ASSERT_EQ(mgr.high_water_used_count(), 3u);

    mgr.alloc_slot();
    mgr.alloc_slot();
    ASSERT_EQ(mgr.used_slot_count(), 4u);
    ASSERT_EQ(mgr.high_water_used_count(), 4u);

    mgr.close();
}

// --- Concurrent allocation: many threads racing on the same free list ---

static void test_concurrent_alloc_no_duplicates() {
    TEST(concurrent_alloc_no_duplicates);

    constexpr uint32_t kSlots   = 2000;
    constexpr int       kThreads = 8;

    ShmManager mgr;
    ASSERT_TRUE(mgr.create(uint64_t(kSlots) * SLOT_SIZE));

    std::vector<std::atomic<int>> owned(kSlots);
    for (auto& c : owned) c.store(0, std::memory_order_relaxed);

    std::atomic<uint32_t> total_allocated{0};
    std::vector<std::vector<uint32_t>> per_thread_owned(kThreads);

    auto drain = [&]() {
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t]() {
                for (;;) {
                    uint32_t idx = mgr.alloc_slot();
                    if (idx == SLOT_IDX_FREE) break;
                    per_thread_owned[t].push_back(idx);
                    owned.at(idx).fetch_add(1, std::memory_order_relaxed);
                    total_allocated.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& th : threads) th.join();
    };

    drain();

    ASSERT_EQ(total_allocated.load(), kSlots);
    for (uint32_t i = 0; i < kSlots; ++i) {
        ASSERT_EQ(owned[i].load(), 1);
    }
    ASSERT_EQ(mgr.used_slot_count(), kSlots);
    ASSERT_EQ(mgr.free_slot_count(), 0u);
    ASSERT_EQ(mgr.high_water_used_count(), kSlots);

    {
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t]() {
                for (uint32_t idx : per_thread_owned[t]) mgr.free_slot(idx);
            });
        }
        for (auto& th : threads) th.join();
    }

    ASSERT_EQ(mgr.used_slot_count(), 0u);
    ASSERT_EQ(mgr.free_slot_count(), kSlots);
    ASSERT_EQ(mgr.high_water_used_count(), kSlots);

    for (auto& c : owned) c.store(0, std::memory_order_relaxed);
    total_allocated.store(0);
    for (auto& v : per_thread_owned) v.clear();

    drain();

    ASSERT_EQ(total_allocated.load(), kSlots);
    for (uint32_t i = 0; i < kSlots; ++i) {
        ASSERT_EQ(owned[i].load(), 1);
    }

    mgr.close();
}

// --- Reopen persistence ---

static void test_data_persists_across_reopen() {
    TEST(data_persists_across_reopen);

    // Phase 1: create, write, close
    {
        ShmManager mgr;
        ASSERT_TRUE(mgr.create(SLOT_SIZE * 20));

        auto idx = mgr.open_session("persist-test", 200, 255, 1400, 20, 0,
                                     "/out/persist-test.bin");
        ASSERT_TRUE(idx.has_value());

        BlockView bv = mgr.block(*idx, 0).value();
        ASSERT_TRUE(bv.register_symbol(0, 7) == SymbolRegisterResult::NewSymbol);
        ASSERT_TRUE(bv.register_symbol(1, 3) == SymbolRegisterResult::NewSymbol);

        SlotView sv = mgr.slot(7);
        std::memset(sv.data(), 0xBE, 16);

        mgr.close();
    }

    // Phase 2: reopen and verify
    {
        ShmManager mgr;
        ASSERT_TRUE(mgr.open());
        ASSERT_TRUE(mgr.validate());

        SessionEntry* se = mgr.find_session("persist-test");
        ASSERT_TRUE(se != nullptr);
        ASSERT_EQ(se->state.load(), SESSION_STATE_OPEN);
        ASSERT_EQ(se->k, 200u);
        ASSERT_EQ(se->n, 255u);

        // Need the session's index for block()/block_entry() -- find it
        // by scanning rather than assuming 0 (defensive; it happens to be
        // 0 here since this is the only session opened in a fresh
        // segment, but the test shouldn't rely on that coincidence).
        uint32_t session_idx = MAX_SESSIONS;
        for (uint32_t i = 0; i < MAX_SESSIONS; ++i) {
            if (mgr.session_entry(i) == se) { session_idx = i; break; }
        }
        ASSERT_TRUE(session_idx < MAX_SESSIONS);

        BlockView bv = mgr.block(session_idx, 0).value();
        ASSERT_EQ(bv.slot_idx(0), 7u);
        ASSERT_EQ(bv.slot_idx(1), 3u);
        ASSERT_EQ(bv.seen(), 2u);
        ASSERT_TRUE(bv.is_present(0));
        ASSERT_TRUE(bv.is_present(1));
        ASSERT_TRUE(!bv.is_present(2));
        ASSERT_TRUE(bv.register_symbol(0, 99) == SymbolRegisterResult::Duplicate);
        ASSERT_EQ(bv.slot_idx(0), 7u);

        const uint8_t* p = static_cast<const uint8_t*>(mgr.slot(7).data());
        for (int i = 0; i < 16; ++i) {
            ASSERT_EQ(p[i], 0xBE);
        }

        mgr.close();
    }
}

// --- Header boot_id and owner_pid ---

static void test_header_metadata() {
    TEST(header_metadata);

    ShmManager mgr;
    ASSERT_TRUE(mgr.create(SLOT_SIZE * 10));

    const ShmHeader* h = mgr.header();
    ASSERT_EQ(h->owner_pid, static_cast<uint32_t>(::getpid()));
    ASSERT_TRUE(h->boot_id[0] != 0);

    mgr.close();
}

// --- Error cases ---

static void test_open_nonexistent() {
    TEST(open_nonexistent);

    ::shm_unlink(SHM_NAME);

    ShmManager mgr;
    ASSERT_TRUE(!mgr.open());
    ASSERT_EQ(mgr.fd(), -1);
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "=== SHM Manager Tests (Phase 2-6, 9) ===\n\n";

    // Layout (pure math, no SHM needed)
    RUN(test_layout_calculation);
    RUN(test_tiny_arena_layout);

    // Lifecycle
    RUN(test_create_open_close);
    RUN(test_create_default_config);

    // Slot access
    RUN(test_slot_access);
    RUN(test_slot_out_of_bounds);

    // Session table
    RUN(test_session_table_access);
    RUN(test_session_index_out_of_range);

    // open_session() (Phase 9)
    RUN(test_open_session_basic_fields);
    RUN(test_open_session_idempotent_same_id);
    RUN(test_open_session_two_distinct_sessions_get_distinct_regions);
    RUN(test_open_session_rejects_over_capacity);
    RUN(test_open_session_exhausts_max_sessions);
    RUN(test_open_session_concurrent_same_id_one_entry);
    RUN(test_open_session_concurrent_distinct_ids_all_succeed);

    // Block table
    RUN(test_block_table_access);
    RUN(test_block_view_operations);

    // Symbol registration & duplicate suppression (Phase 5)
    RUN(test_register_sequence_increments_seen);
    RUN(test_register_duplicate_preserves_original_slot);
    RUN(test_register_invalid_symbol_id);
    RUN(test_register_concurrent_same_symbol_one_winner);
    RUN(test_register_concurrent_distinct_symbols_all_new);

    // Decode claim (Phase 6)
    RUN(test_claim_decode_single_winner_sequential);
    RUN(test_claim_decode_lifecycle_and_misuse);
    RUN(test_claim_decode_concurrent_one_winner);

    // Validation
    RUN(test_bounds_validation);
    RUN(test_arena_capacity);

    // Slot allocator (Phase 4)
    RUN(test_alloc_exhaustion);
    RUN(test_alloc_free_reuse);
    RUN(test_high_water_tracking);
    RUN(test_concurrent_alloc_no_duplicates);

    // Persistence
    RUN(test_data_persists_across_reopen);

    // Metadata
    RUN(test_header_metadata);

    // Error cases
    RUN(test_open_nonexistent);

    std::cout << "\n=== Results: " << g_pass << " run, "
              << g_fail << " failed ===\n";

    // Clean up SHM
    ::shm_unlink(SHM_NAME);

    return g_fail == 0 ? 0 : 1;
}
