#include "receiver/rx_envelope.hpp"
#include "receiver/proto_hash.hpp"

#include <cstring>
#include <iostream>

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
// Tests -- outbound (receiver -> manager)
// =============================================================================

static void test_build_receiver_hello() {
    TEST(build_receiver_hello);

    std::string raw = build_receiver_hello(2, 12345, 9102);

    nexus::rx::RxEnvelope env;
    ASSERT_TRUE(env.ParseFromString(raw));
    ASSERT_TRUE(env.msg_case() == nexus::rx::RxEnvelope::kReceiverHello);
    ASSERT_EQ(env.receiver_hello().receiver_id(), 2u);
    ASSERT_EQ(env.receiver_hello().pid(), 12345u);
    ASSERT_EQ(env.receiver_hello().listen_port(), 9102u);

    // proto_hash must be exactly the build-time-generated constant, raw
    // bytes -- not hex, not truncated, not zero-padded.
    const std::string& hash = env.receiver_hello().proto_hash();
    ASSERT_EQ(hash.size(), NEXUS_PROTO_HASH_SIZE);
    ASSERT_TRUE(std::memcmp(hash.data(), NEXUS_PROTO_HASH, NEXUS_PROTO_HASH_SIZE) == 0);
}

static void test_build_heartbeat() {
    TEST(build_heartbeat);

    std::string raw = build_heartbeat(999, 1234567890123ull);

    nexus::rx::RxEnvelope env;
    ASSERT_TRUE(env.ParseFromString(raw));
    ASSERT_TRUE(env.msg_case() == nexus::rx::RxEnvelope::kHeartbeat);
    ASSERT_EQ(env.heartbeat().process_id(), 999u);
    ASSERT_EQ(env.heartbeat().timestamp_unix_ms(), 1234567890123ull);
}

static void test_build_manifest_seen() {
    TEST(build_manifest_seen);

    nexus::common::Manifest manifest;
    manifest.set_session_id("sess-xyz");
    manifest.set_filepath("some/file.bin");
    manifest.set_file_size(1073741824ull);
    manifest.set_k(200);
    manifest.set_n(255);
    manifest.set_block_bytes(1400); // per docs/ANSWERS_FROM_C.md §0: the true symbol size
    manifest.set_total_blocks(3835);

    std::string raw = build_manifest_seen(1, manifest);

    nexus::rx::RxEnvelope env;
    ASSERT_TRUE(env.ParseFromString(raw));
    ASSERT_TRUE(env.msg_case() == nexus::rx::RxEnvelope::kManifestSeen);
    ASSERT_EQ(env.manifest_seen().receiver_id(), 1u);
    ASSERT_TRUE(env.manifest_seen().manifest().session_id() == "sess-xyz");
    ASSERT_EQ(env.manifest_seen().manifest().k(), 200u);
    ASSERT_EQ(env.manifest_seen().manifest().total_blocks(), 3835u);
}

static void test_build_receiver_stats() {
    TEST(build_receiver_stats);

    nexus::rx::ReceiverStats stats;
    stats.set_receiver_id(0); // per docs/ANSWERS_FROM_C.md §15 this is ignored by the manager anyway
    stats.set_pkts_ok(1000);
    stats.set_crc_fail(3);
    stats.set_bad_magic(1);
    stats.set_unparsable(2);
    stats.set_duplicates(7);
    stats.set_no_session(0);
    stats.set_arena_exhausted(0);
    stats.set_kernel_drops(0);
    stats.set_arena_high_water_pct(42);

    std::string raw = build_receiver_stats(stats);

    nexus::rx::RxEnvelope env;
    ASSERT_TRUE(env.ParseFromString(raw));
    ASSERT_TRUE(env.msg_case() == nexus::rx::RxEnvelope::kReceiverStats);
    ASSERT_EQ(env.receiver_stats().pkts_ok(), 1000ull);
    ASSERT_EQ(env.receiver_stats().crc_fail(), 3ull);
    ASSERT_EQ(env.receiver_stats().duplicates(), 7ull);
    ASSERT_EQ(env.receiver_stats().arena_high_water_pct(), 42u);
}

static void test_build_block_decoded_is_batched() {
    TEST(build_block_decoded_is_batched);

    std::string raw = build_block_decoded("sess-1", 2, {5, 6, 7, 100});

    nexus::rx::RxEnvelope env;
    ASSERT_TRUE(env.ParseFromString(raw));
    ASSERT_TRUE(env.msg_case() == nexus::rx::RxEnvelope::kBlockDecoded);
    ASSERT_TRUE(env.block_decoded().session_id() == "sess-1");
    ASSERT_EQ(env.block_decoded().receiver_id(), 2u);

    // Confirms this is genuinely ONE message carrying MULTIPLE block ids
    // (docs/ANSWERS_FROM_C.md §15), not something that only happens to
    // work for one id at a time.
    ASSERT_EQ(env.block_decoded().block_ids_size(), 4);
    ASSERT_EQ(env.block_decoded().block_ids(0), 5u);
    ASSERT_EQ(env.block_decoded().block_ids(1), 6u);
    ASSERT_EQ(env.block_decoded().block_ids(2), 7u);
    ASSERT_EQ(env.block_decoded().block_ids(3), 100u);
}

// =============================================================================
// Tests -- inbound (manager -> receiver)
// =============================================================================

static void test_parse_incoming_session_open() {
    TEST(parse_incoming_session_open);

    // Simulates what session_manager actually sends
    // (services/authority.py:_broadcast_session_open) -- built directly
    // here, not through any build_*() helper, since SessionOpen is
    // manager-originated and this class deliberately has no build
    // function for it.
    nexus::rx::RxEnvelope in;
    auto* so = in.mutable_session_open();
    so->set_session_id("sess-open-1");
    so->set_dest_path("/staging/sess-open-1/file.bin");
    so->set_total_blocks(10);
    so->set_k(200);
    so->set_n(255);
    so->set_block_bytes(1400);
    so->set_block_table_offset(4160);
    so->set_bitmap_offset(4640);
    std::string raw;
    in.SerializeToString(&raw);

    nexus::rx::RxEnvelope out;
    RxEnvelopeCase kase{};
    ASSERT_TRUE(parse_incoming(raw, &out, &kase));
    ASSERT_TRUE(kase == RxEnvelopeCase::SessionOpen);
    ASSERT_TRUE(out.session_open().session_id() == "sess-open-1");
    ASSERT_TRUE(out.session_open().dest_path() == "/staging/sess-open-1/file.bin");
    ASSERT_EQ(out.session_open().k(), 200u);
}

static void test_parse_incoming_purge_session() {
    TEST(parse_incoming_purge_session);

    nexus::rx::RxEnvelope in;
    auto* ps = in.mutable_purge_session();
    ps->set_session_id("sess-purge-1");
    ps->set_reason("manifest_hash_mismatch");
    std::string raw;
    in.SerializeToString(&raw);

    nexus::rx::RxEnvelope out;
    RxEnvelopeCase kase{};
    ASSERT_TRUE(parse_incoming(raw, &out, &kase));
    ASSERT_TRUE(kase == RxEnvelopeCase::PurgeSession);
    ASSERT_TRUE(out.purge_session().session_id() == "sess-purge-1");
    ASSERT_TRUE(out.purge_session().reason() == "manifest_hash_mismatch");
}

static void test_parse_incoming_config() {
    TEST(parse_incoming_config);

    nexus::rx::RxEnvelope in;
    auto* cfg = in.mutable_config();
    cfg->set_shm_name("nexus-rx");
    cfg->set_staging_dir("/staging");
    cfg->set_journal_dir("/journal");
    std::string raw;
    in.SerializeToString(&raw);

    nexus::rx::RxEnvelope out;
    RxEnvelopeCase kase{};
    ASSERT_TRUE(parse_incoming(raw, &out, &kase));
    ASSERT_TRUE(kase == RxEnvelopeCase::Config);
    ASSERT_TRUE(out.config().shm_name() == "nexus-rx");
}

static void test_parse_incoming_heartbeat() {
    TEST(parse_incoming_heartbeat);

    nexus::rx::RxEnvelope in;
    auto* hb = in.mutable_heartbeat();
    hb->set_process_id(42);
    hb->set_timestamp_unix_ms(555);
    std::string raw;
    in.SerializeToString(&raw);

    nexus::rx::RxEnvelope out;
    RxEnvelopeCase kase{};
    ASSERT_TRUE(parse_incoming(raw, &out, &kase));
    ASSERT_TRUE(kase == RxEnvelopeCase::Heartbeat);
    ASSERT_EQ(out.heartbeat().process_id(), 42u);
}

static void test_parse_incoming_receiver_originated_reported_distinctly() {
    TEST(parse_incoming_receiver_originated_reported_distinctly);

    // A ReceiverHello arriving AT the receiver would be the wrong
    // direction entirely -- must not be silently treated as valid
    // inbound data, nor folded into Unset as if nothing were there.
    std::string raw = build_receiver_hello(0, 1, 9100);

    nexus::rx::RxEnvelope out;
    RxEnvelopeCase kase{};
    ASSERT_TRUE(parse_incoming(raw, &out, &kase));
    ASSERT_TRUE(kase == RxEnvelopeCase::OtherReceiverOriginated);
}

static void test_parse_incoming_empty_envelope_is_unset() {
    TEST(parse_incoming_empty_envelope_is_unset);

    nexus::rx::RxEnvelope in; // nothing set -- MSG_NOT_SET
    std::string raw;
    in.SerializeToString(&raw);

    nexus::rx::RxEnvelope out;
    RxEnvelopeCase kase{};
    ASSERT_TRUE(parse_incoming(raw, &out, &kase)); // parses fine, just an empty oneof
    ASSERT_TRUE(kase == RxEnvelopeCase::Unset);
}

static void test_parse_incoming_rejects_garbage() {
    TEST(parse_incoming_rejects_garbage);

    // Same non-terminating-varint garbage used in
    // test_frame_dispatcher.cpp's equivalent case -- reliably fails to
    // parse as any protobuf message.
    const std::string garbage = "\xFF\xFF\xFF\xFF\xFF";

    nexus::rx::RxEnvelope out;
    RxEnvelopeCase kase{};
    ASSERT_TRUE(!parse_incoming(garbage, &out, &kase));
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "=== RX Envelope Tests (Phase 12) ===\n\n";

    RUN(test_build_receiver_hello);
    RUN(test_build_heartbeat);
    RUN(test_build_manifest_seen);
    RUN(test_build_receiver_stats);
    RUN(test_build_block_decoded_is_batched);

    RUN(test_parse_incoming_session_open);
    RUN(test_parse_incoming_purge_session);
    RUN(test_parse_incoming_config);
    RUN(test_parse_incoming_heartbeat);
    RUN(test_parse_incoming_receiver_originated_reported_distinctly);
    RUN(test_parse_incoming_empty_envelope_is_unset);
    RUN(test_parse_incoming_rejects_garbage);

    std::cout << "\n=== Results: " << g_pass << " run, "
              << g_fail << " failed ===\n";

    return g_fail == 0 ? 0 : 1;
}
