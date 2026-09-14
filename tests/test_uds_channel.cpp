#include "receiver/uds_channel.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

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
// Test helpers -- a minimal SOCK_SEQPACKET server, standing in for
// session_manager's real UDS listener, so UdsChannel::connect() (the
// client half) gets tested against a real bind()/listen()/accept(), not
// just a pre-connected socketpair().
// =============================================================================

static std::string test_socket_path(const char* name) {
    // Socket files go under $TMPDIR (dev.sh sets TMPDIR=/tmp in the
    // container); never a hardcoded home directory, which does not exist
    // on other machines or inside containers.
    const char* tmpdir = std::getenv("TMPDIR");
    return std::string(tmpdir ? tmpdir : "/tmp") + "/uds_test_" + name + ".sock";
}

// Binds and listens; returns the listening fd, or -1 on failure.
static int start_listener(const std::string& path) {
    ::unlink(path.c_str());

    int fd = ::socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) return -1;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 1) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// =============================================================================
// Tests
// =============================================================================

static void test_connect_and_close() {
    TEST(connect_and_close);

    const std::string path = test_socket_path("connect_close");
    int listen_fd = start_listener(path);
    ASSERT_TRUE(listen_fd >= 0);

    std::thread server([&]() {
        int conn = ::accept(listen_fd, nullptr, nullptr);
        if (conn >= 0) ::close(conn);
    });

    UdsChannel ch;
    ASSERT_TRUE(ch.connect(path.c_str()));
    ASSERT_TRUE(ch.fd() >= 0);
    ch.close();
    ch.close(); // must be safe to call twice, same convention as ShmManager/UdpReceiver

    server.join();
    ::close(listen_fd);
    ::unlink(path.c_str());
}

static void test_connect_rejects_missing_socket() {
    TEST(connect_rejects_missing_socket);

    const std::string path = test_socket_path("missing");
    ::unlink(path.c_str());

    UdsChannel ch;
    ASSERT_TRUE(!ch.connect(path.c_str()));
    ASSERT_EQ(ch.fd(), -1);
}

static void test_connect_rejects_path_too_long() {
    TEST(connect_rejects_path_too_long);

    // sockaddr_un::sun_path is 108 bytes on Linux; anything at or past
    // that must be rejected cleanly (not truncated silently, not a
    // buffer overrun).
    std::string long_path(200, 'x');

    UdsChannel ch;
    ASSERT_TRUE(!ch.connect(long_path.c_str()));
    ASSERT_EQ(ch.fd(), -1);
}

static void test_client_send_server_receives() {
    TEST(client_send_server_receives);

    const std::string path = test_socket_path("send_to_server");
    int listen_fd = start_listener(path);
    ASSERT_TRUE(listen_fd >= 0);

    std::string received;
    std::thread server([&]() {
        int conn = ::accept(listen_fd, nullptr, nullptr);
        char buf[256];
        ssize_t n = ::recv(conn, buf, sizeof(buf), 0);
        if (n > 0) received.assign(buf, static_cast<size_t>(n));
        ::close(conn);
    });

    UdsChannel ch;
    ASSERT_TRUE(ch.connect(path.c_str()));
    ASSERT_TRUE(ch.send("hello session_manager"));
    ch.close();

    server.join();
    ASSERT_TRUE(received == "hello session_manager");

    ::close(listen_fd);
    ::unlink(path.c_str());
}

static void test_server_send_client_receives() {
    TEST(server_send_client_receives);

    const std::string path = test_socket_path("send_to_client");
    int listen_fd = start_listener(path);
    ASSERT_TRUE(listen_fd >= 0);

    std::thread server([&]() {
        int conn = ::accept(listen_fd, nullptr, nullptr);
        const std::string msg = "SessionOpen-shaped-bytes";
        ::send(conn, msg.data(), msg.size(), 0);
        ::close(conn);
    });

    UdsChannel ch;
    ASSERT_TRUE(ch.connect(path.c_str()));
    auto received = ch.receive();
    ch.close();

    server.join();
    ASSERT_TRUE(received.has_value());
    ASSERT_TRUE(*received == "SessionOpen-shaped-bytes");

    ::close(listen_fd);
    ::unlink(path.c_str());
}

static void test_message_boundaries_preserved() {
    TEST(message_boundaries_preserved);

    // The core SOCK_SEQPACKET property this whole design leans on
    // (docs/CROSS_TEAM_ANSWERS.md: "no extra framing needed... preserves
    // message boundaries natively"). Send two distinct messages
    // back-to-back; each receive() must return exactly one, not a
    // concatenation and not a truncation.
    const std::string path = test_socket_path("boundaries");
    int listen_fd = start_listener(path);
    ASSERT_TRUE(listen_fd >= 0);

    std::thread server([&]() {
        int conn = ::accept(listen_fd, nullptr, nullptr);
        const std::string first = "first-message";
        const std::string second = "second-message-different-length";
        ::send(conn, first.data(), first.size(), 0);
        ::send(conn, second.data(), second.size(), 0);
        ::close(conn);
    });

    UdsChannel ch;
    ASSERT_TRUE(ch.connect(path.c_str()));
    auto m1 = ch.receive();
    auto m2 = ch.receive();
    ch.close();

    server.join();
    ASSERT_TRUE(m1.has_value());
    ASSERT_TRUE(m2.has_value());
    ASSERT_TRUE(*m1 == "first-message");
    ASSERT_TRUE(*m2 == "second-message-different-length");
}

static void test_receive_returns_nullopt_on_peer_close() {
    TEST(receive_returns_nullopt_on_peer_close);

    const std::string path = test_socket_path("peer_close");
    int listen_fd = start_listener(path);
    ASSERT_TRUE(listen_fd >= 0);

    std::thread server([&]() {
        int conn = ::accept(listen_fd, nullptr, nullptr);
        ::close(conn); // close immediately, without sending anything
    });

    UdsChannel ch;
    ASSERT_TRUE(ch.connect(path.c_str()));
    auto received = ch.receive(); // blocks until the peer closes, then must return nullopt cleanly
    ch.close();

    server.join();
    ASSERT_TRUE(!received.has_value());

    ::close(listen_fd);
    ::unlink(path.c_str());
}

static void test_nonblocking_receive_returns_nullopt_when_empty() {
    TEST(nonblocking_receive_returns_nullopt_when_empty);

    const std::string path = test_socket_path("nonblocking_empty");
    int listen_fd = start_listener(path);
    ASSERT_TRUE(listen_fd >= 0);

    std::thread server([&]() {
        int conn = ::accept(listen_fd, nullptr, nullptr);
        ::usleep(50000); // hold the connection open without sending anything
        ::close(conn);
    });

    UdsChannel ch;
    ASSERT_TRUE(ch.connect(path.c_str()));
    ASSERT_TRUE(ch.set_nonblocking(true));

    auto received = ch.receive(); // nothing sent yet -- must not block
    ASSERT_TRUE(!received.has_value());

    ch.close();
    server.join();
    ::close(listen_fd);
    ::unlink(path.c_str());
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "=== UDS Channel Tests (Phase 12) ===\n\n";

    RUN(test_connect_and_close);
    RUN(test_connect_rejects_missing_socket);
    RUN(test_connect_rejects_path_too_long);
    RUN(test_client_send_server_receives);
    RUN(test_server_send_client_receives);
    RUN(test_message_boundaries_preserved);
    RUN(test_receive_returns_nullopt_on_peer_close);
    RUN(test_nonblocking_receive_returns_nullopt_when_empty);

    std::cout << "\n=== Results: " << g_pass << " run, "
              << g_fail << " failed ===\n";

    return g_fail == 0 ? 0 : 1;
}
