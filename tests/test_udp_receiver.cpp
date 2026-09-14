#include "receiver/udp_receiver.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
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
// Allocation counter -- proves receive_batch() doesn't allocate, instead
// of just asserting it in a comment (same rigor as TSan/ASan elsewhere in
// this test suite: verify the property, don't just claim it).
//
// Global operator new/delete overrides apply process-wide once linked in,
// which is fine here since this whole binary exists only to test
// UdpReceiver. Real allocations still happen (std::vector setup in
// bind(), iostream buffering, etc.) -- the counter is only read around
// the specific calls under test, not for the whole program.
// =============================================================================

static std::atomic<long> g_alloc_count{0};
static bool g_counting = false;

void* operator new(std::size_t size) {
    if (g_counting) g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

// =============================================================================
// Test helpers
// =============================================================================

// A bare UDP client socket for sending test datagrams to a UdpReceiver
// bound on loopback.
static int make_client_socket() {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    return fd;
}

static void send_to(int client_fd, uint16_t port, const void* data, size_t len) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    ssize_t n = ::sendto(client_fd, data, len, 0,
                          reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    (void)n; // best-effort in tests; a short write would fail the assertions below anyway
}

// Repeatedly calls receive_batch() (non-blocking) until `want` datagrams
// have been collected or a generous retry budget is exhausted -- loopback
// delivery is fast but not synchronous with sendto() returning, so a
// single receive_batch() call right after sending isn't guaranteed to see
// everything yet.
static int collect(UdpReceiver& rx, ReceivedDatagram* out, int want) {
    int total = 0;
    for (int attempt = 0; attempt < 2000 && total < want; ++attempt) {
        int n = rx.receive_batch(out + total);
        if (n > 0) {
            total += n;
        } else if (n == 0) {
            ::usleep(500); // nothing pending yet -- brief backoff, not a busy-spin
        } else {
            break; // real error
        }
    }
    return total;
}

// =============================================================================
// Tests
// =============================================================================

static void test_bind_ephemeral_and_close() {
    TEST(bind_ephemeral_and_close);

    UdpReceiver rx;
    ASSERT_TRUE(rx.bind(0, "127.0.0.1")); // port 0 -> kernel picks one
    ASSERT_TRUE(rx.fd() >= 0);
    ASSERT_TRUE(rx.local_port() != 0);

    rx.close();
    ASSERT_EQ(rx.fd(), -1);
    rx.close(); // must be safe to call twice, same convention as ShmManager::close()
}

static void test_bind_invalid_address_fails() {
    TEST(bind_invalid_address_fails);

    UdpReceiver rx;
    ASSERT_TRUE(!rx.bind(0, "not-an-ip-address"));
    ASSERT_EQ(rx.fd(), -1);
}

static void test_single_datagram_round_trip() {
    TEST(single_datagram_round_trip);

    UdpReceiver rx;
    ASSERT_TRUE(rx.bind(0, "127.0.0.1"));
    rx.set_nonblocking(true);

    int client = make_client_socket();
    ASSERT_TRUE(client >= 0);

    const char* payload = "hello uniflow receiver";
    const size_t payload_len = std::strlen(payload);
    send_to(client, rx.local_port(), payload, payload_len);

    ReceivedDatagram batch[RECV_BATCH_SIZE];
    int n = collect(rx, batch, 1);

    ASSERT_EQ(n, 1);
    ASSERT_EQ(batch[0].len, payload_len);
    ASSERT_TRUE(std::memcmp(batch[0].data, payload, payload_len) == 0);

    // Source address should be loopback, on the client's own ephemeral
    // port (nonzero, since we never bound the client explicitly).
    ASSERT_EQ(batch[0].src_addr.sin_family, AF_INET);
    ASSERT_TRUE(ntohs(batch[0].src_addr.sin_port) != 0);
    char ip[INET_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET, &batch[0].src_addr.sin_addr, ip, sizeof(ip));
    ASSERT_TRUE(std::strcmp(ip, "127.0.0.1") == 0);

    ::close(client);
}

static void test_max_size_datagram_preserved() {
    TEST(max_size_datagram_preserved);

    UdpReceiver rx;
    ASSERT_TRUE(rx.bind(0, "127.0.0.1"));
    rx.set_nonblocking(true);

    int client = make_client_socket();
    ASSERT_TRUE(client >= 0);

    // A realistic full-size frame: 12-byte prefix + ~30 bytes protobuf
    // overhead + 1400-byte symbol payload, well under MAX_DATAGRAM_SIZE.
    std::vector<uint8_t> payload(1442);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>(i & 0xFF);
    }
    send_to(client, rx.local_port(), payload.data(), payload.size());

    ReceivedDatagram batch[RECV_BATCH_SIZE];
    int n = collect(rx, batch, 1);

    ASSERT_EQ(n, 1);
    ASSERT_EQ(batch[0].len, payload.size());
    ASSERT_TRUE(std::memcmp(batch[0].data, payload.data(), payload.size()) == 0);

    ::close(client);
}

static void test_batch_receives_multiple_datagrams() {
    TEST(batch_receives_multiple_datagrams);

    UdpReceiver rx;
    ASSERT_TRUE(rx.bind(0, "127.0.0.1"));
    rx.set_nonblocking(true);

    int client = make_client_socket();
    ASSERT_TRUE(client >= 0);

    constexpr int kCount = 10;
    for (int i = 0; i < kCount; ++i) {
        char msg[32];
        int len = std::snprintf(msg, sizeof(msg), "packet-%02d", i);
        send_to(client, rx.local_port(), msg, static_cast<size_t>(len));
    }

    ReceivedDatagram batch[RECV_BATCH_SIZE];
    int n = collect(rx, batch, kCount);

    ASSERT_EQ(n, kCount);

    // Each of the 10 distinct payloads should show up exactly once,
    // regardless of exactly how recvmmsg() chose to batch them across
    // calls (UDP doesn't guarantee ordering, though loopback usually
    // preserves it).
    bool seen[kCount] = {};
    for (int i = 0; i < n; ++i) {
        int idx = -1;
        std::sscanf(reinterpret_cast<const char*>(batch[i].data), "packet-%d", &idx);
        ASSERT_TRUE(idx >= 0 && idx < kCount);
        ASSERT_TRUE(!seen[idx]); // no duplicates
        seen[idx] = true;
    }
    for (int i = 0; i < kCount; ++i) ASSERT_TRUE(seen[i]);

    ::close(client);
}

static void test_receive_batch_caps_at_batch_size() {
    TEST(receive_batch_caps_at_batch_size);

    UdpReceiver rx;
    ASSERT_TRUE(rx.bind(0, "127.0.0.1"));
    rx.set_nonblocking(true);

    int client = make_client_socket();
    ASSERT_TRUE(client >= 0);

    const int kTotal = static_cast<int>(RECV_BATCH_SIZE) + 20;
    for (int i = 0; i < kTotal; ++i) {
        char msg[8] = {'p'};
        send_to(client, rx.local_port(), msg, sizeof(msg));
    }

    ReceivedDatagram batch[RECV_BATCH_SIZE];
    int total = 0;
    int calls = 0;
    for (int attempt = 0; attempt < 2000 && total < kTotal; ++attempt) {
        int n = rx.receive_batch(batch);
        if (n > 0) {
            ASSERT_TRUE(n <= static_cast<int>(RECV_BATCH_SIZE)); // never more than one batch's worth
            total += n;
            ++calls;
        } else if (n == 0) {
            ::usleep(500);
        } else {
            break;
        }
    }

    ASSERT_EQ(total, kTotal);
    ASSERT_TRUE(calls >= 2); // kTotal > RECV_BATCH_SIZE, so this MUST have taken more than one call

    ::close(client);
}

static void test_nonblocking_returns_zero_when_empty() {
    TEST(nonblocking_returns_zero_when_empty);

    UdpReceiver rx;
    ASSERT_TRUE(rx.bind(0, "127.0.0.1"));
    ASSERT_TRUE(rx.set_nonblocking(true));

    ReceivedDatagram batch[RECV_BATCH_SIZE];
    int n = rx.receive_batch(batch); // nothing was ever sent to this socket
    ASSERT_EQ(n, 0);
}

static void test_receive_batch_does_not_allocate() {
    TEST(receive_batch_does_not_allocate);

    UdpReceiver rx;
    ASSERT_TRUE(rx.bind(0, "127.0.0.1")); // allocation-counting is OFF during setup
    rx.set_nonblocking(true);

    int client = make_client_socket();
    ASSERT_TRUE(client >= 0);
    const char* payload = "no heap traffic please";
    send_to(client, rx.local_port(), payload, std::strlen(payload));
    ::usleep(2000); // give loopback delivery a moment before the counted call

    ReceivedDatagram batch[RECV_BATCH_SIZE];

    g_alloc_count.store(0, std::memory_order_relaxed);
    g_counting = true;
    int n = rx.receive_batch(batch); // the ONLY call counted
    g_counting = false;

    ASSERT_EQ(n, 1);
    ASSERT_EQ(g_alloc_count.load(std::memory_order_relaxed), 0);

    ::close(client);
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "=== UDP Receiver Tests (Phase 7) ===\n\n";

    RUN(test_bind_ephemeral_and_close);
    RUN(test_bind_invalid_address_fails);
    RUN(test_single_datagram_round_trip);
    RUN(test_max_size_datagram_preserved);
    RUN(test_batch_receives_multiple_datagrams);
    RUN(test_receive_batch_caps_at_batch_size);
    RUN(test_nonblocking_returns_zero_when_empty);
    RUN(test_receive_batch_does_not_allocate);

    std::cout << "\n=== Results: " << g_pass << " run, "
              << g_fail << " failed ===\n";

    return g_fail == 0 ? 0 : 1;
}
