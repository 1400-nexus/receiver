#include "receiver/frame_dispatcher.hpp"
#include "common/wire.hpp"

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
// Test helpers
// =============================================================================

// Serializes `msg` and wraps it in a valid wire frame via build_frame()
// (wire.hpp) -- exactly what a real sender does, minus the UDP hop.
static size_t make_frame(const google::protobuf::MessageLite& msg,
                          uint8_t* out, size_t cap) {
    std::string body;
    if (!msg.SerializeToString(&body)) return 0;
    return build_frame(out, cap,
                        reinterpret_cast<const uint8_t*>(body.data()),
                        body.size());
}

// =============================================================================
// Tests
// =============================================================================

static void test_decode_data_packet() {
    TEST(decode_data_packet);

    nexus::net::Frame in;
    auto* data = in.mutable_data();
    data->set_session_id("sess-1");
    data->set_block_id(42);
    data->set_symbol_id(7);
    data->set_payload("some symbol bytes");

    uint8_t frame[256];
    size_t frame_len = make_frame(in, frame, sizeof(frame));
    ASSERT_TRUE(frame_len > 0);

    nexus::net::Frame out;
    auto result = decode_frame(frame, frame_len, &out);

    ASSERT_TRUE(result == FrameDecodeResult::Ok);
    ASSERT_TRUE(out.msg_case() == nexus::net::Frame::kData);
    ASSERT_TRUE(out.data().session_id() == "sess-1");
    ASSERT_EQ(out.data().block_id(), 42u);
    ASSERT_EQ(out.data().symbol_id(), 7u);
    ASSERT_TRUE(out.data().payload() == "some symbol bytes");
}

static void test_decode_manifest() {
    TEST(decode_manifest);

    nexus::net::Frame in;
    auto* manifest = in.mutable_manifest();
    manifest->set_session_id("sess-2");
    manifest->set_filepath("dir/file.bin");
    manifest->set_file_size(123456789);
    manifest->set_k(200);
    manifest->set_n(255);

    uint8_t frame[256];
    size_t frame_len = make_frame(in, frame, sizeof(frame));
    ASSERT_TRUE(frame_len > 0);

    nexus::net::Frame out;
    auto result = decode_frame(frame, frame_len, &out);

    ASSERT_TRUE(result == FrameDecodeResult::Ok);
    ASSERT_TRUE(out.msg_case() == nexus::net::Frame::kManifest);
    ASSERT_TRUE(out.manifest().session_id() == "sess-2");
    ASSERT_EQ(out.manifest().k(), 200u);
    ASSERT_EQ(out.manifest().n(), 255u);
}

static void test_decode_session_end() {
    TEST(decode_session_end);

    nexus::net::Frame in;
    auto* end = in.mutable_end();
    end->set_session_id("sess-3");
    end->set_total_blocks(3835);

    uint8_t frame[256];
    size_t frame_len = make_frame(in, frame, sizeof(frame));
    ASSERT_TRUE(frame_len > 0);

    nexus::net::Frame out;
    auto result = decode_frame(frame, frame_len, &out);

    ASSERT_TRUE(result == FrameDecodeResult::Ok);
    ASSERT_TRUE(out.msg_case() == nexus::net::Frame::kEnd);
    ASSERT_TRUE(out.end().session_id() == "sess-3");
    ASSERT_EQ(out.end().total_blocks(), 3835u);
}

static void test_decode_rejects_bad_magic() {
    TEST(decode_rejects_bad_magic);

    nexus::net::Frame in;
    in.mutable_end()->set_session_id("x");
    uint8_t frame[128];
    size_t frame_len = make_frame(in, frame, sizeof(frame));
    ASSERT_TRUE(frame_len > 0);

    frame[0] = 'X'; // corrupt magic

    nexus::net::Frame out;
    auto result = decode_frame(frame, frame_len, &out);
    ASSERT_TRUE(result == FrameDecodeResult::BadMagic);
}

static void test_decode_rejects_bad_header_crc() {
    TEST(decode_rejects_bad_header_crc);

    nexus::net::Frame in;
    in.mutable_end()->set_session_id("x");
    uint8_t frame[128];
    size_t frame_len = make_frame(in, frame, sizeof(frame));
    ASSERT_TRUE(frame_len > 0);

    frame[4] ^= 0xFF; // corrupt proto_len, covered by hdr_crc16

    nexus::net::Frame out;
    auto result = decode_frame(frame, frame_len, &out);
    ASSERT_TRUE(result == FrameDecodeResult::CrcFail);
}

static void test_decode_rejects_bad_body_crc() {
    TEST(decode_rejects_bad_body_crc);

    nexus::net::Frame in;
    auto* data = in.mutable_data();
    data->set_session_id("sess-4");
    data->set_payload("payload bytes");
    uint8_t frame[128];
    size_t frame_len = make_frame(in, frame, sizeof(frame));
    ASSERT_TRUE(frame_len > 0);

    frame[WIRE_PREFIX_SIZE] ^= 0x01; // corrupt one body byte

    nexus::net::Frame out;
    auto result = decode_frame(frame, frame_len, &out);
    ASSERT_TRUE(result == FrameDecodeResult::CrcFail);
}

static void test_decode_rejects_truncated_datagram() {
    TEST(decode_rejects_truncated_datagram);

    nexus::net::Frame in;
    in.mutable_end()->set_session_id("x");
    uint8_t frame[128];
    size_t frame_len = make_frame(in, frame, sizeof(frame));
    ASSERT_TRUE(frame_len > 0);

    nexus::net::Frame out;
    // Hand over fewer bytes than proto_len promises -- as if recvmmsg()
    // only delivered a partial datagram.
    auto result = decode_frame(frame, frame_len - 3, &out);
    ASSERT_TRUE(result == FrameDecodeResult::Unparsable);
}

static void test_decode_rejects_garbage_protobuf_body() {
    TEST(decode_rejects_garbage_protobuf_body);

    // A body that's well-framed (correct magic, correct CRCs for whatever
    // bytes are actually there) but isn't valid protobuf at all -- e.g. a
    // stray non-Uniflow UDP sender on the same port, or bit corruption
    // that happens to survive both CRCs (astronomically unlikely for a
    // real bit flip, but this is exactly the failure mode CRCs reduce the
    // odds of, not eliminate).
    const uint8_t garbage[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t frame[64];
    size_t frame_len = build_frame(frame, sizeof(frame), garbage, sizeof(garbage));
    ASSERT_TRUE(frame_len > 0);

    nexus::net::Frame out;
    auto result = decode_frame(frame, frame_len, &out);
    ASSERT_TRUE(result == FrameDecodeResult::Unparsable);
}

static void test_decode_rejects_empty_oneof() {
    TEST(decode_rejects_empty_oneof);

    // A syntactically valid Frame with none of data/manifest/end set --
    // net.proto's own comment on why Frame needs a discriminator: this
    // must not be silently treated as any of the three real cases.
    nexus::net::Frame in; // mutate nothing
    uint8_t frame[64];
    size_t frame_len = make_frame(in, frame, sizeof(frame));
    ASSERT_TRUE(frame_len > 0);

    nexus::net::Frame out;
    auto result = decode_frame(frame, frame_len, &out);
    ASSERT_TRUE(result == FrameDecodeResult::Unparsable);
}

static void test_out_frame_cleared_between_calls() {
    TEST(out_frame_cleared_between_calls);

    // Same Frame instance reused across two decode_frame() calls (the
    // intended usage -- avoids a per-packet heap allocation for the
    // message itself). The second, different-case frame must not leak
    // any field from the first.
    nexus::net::Frame reused;

    nexus::net::Frame in1;
    in1.mutable_data()->set_session_id("first");
    in1.mutable_data()->set_payload("payload-1");
    uint8_t frame1[128];
    size_t len1 = make_frame(in1, frame1, sizeof(frame1));
    ASSERT_TRUE(decode_frame(frame1, len1, &reused) == FrameDecodeResult::Ok);
    ASSERT_TRUE(reused.msg_case() == nexus::net::Frame::kData);

    nexus::net::Frame in2;
    in2.mutable_manifest()->set_session_id("second");
    uint8_t frame2[128];
    size_t len2 = make_frame(in2, frame2, sizeof(frame2));
    ASSERT_TRUE(decode_frame(frame2, len2, &reused) == FrameDecodeResult::Ok);

    ASSERT_TRUE(reused.msg_case() == nexus::net::Frame::kManifest);
    ASSERT_TRUE(reused.manifest().session_id() == "second");
    // The oneof means has_data() would already be false, but explicitly
    // confirm the old case's own field is gone too, not just switched.
    ASSERT_TRUE(!reused.has_data());
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "=== Frame Dispatcher Tests (Phase 8) ===\n\n";

    RUN(test_decode_data_packet);
    RUN(test_decode_manifest);
    RUN(test_decode_session_end);

    RUN(test_decode_rejects_bad_magic);
    RUN(test_decode_rejects_bad_header_crc);
    RUN(test_decode_rejects_bad_body_crc);
    RUN(test_decode_rejects_truncated_datagram);
    RUN(test_decode_rejects_garbage_protobuf_body);
    RUN(test_decode_rejects_empty_oneof);

    RUN(test_out_frame_cleared_between_calls);

    std::cout << "\n=== Results: " << g_pass << " run, "
              << g_fail << " failed ===\n";

    return g_fail == 0 ? 0 : 1;
}
