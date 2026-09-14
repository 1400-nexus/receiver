#include "receiver/receiver_args.hpp"

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

// Builds a char*[] from string literals for parse_receiver_args(), which
// takes argv the same way main() receives it (argv[0] = program name).
static std::optional<ReceiverArgs> parse(std::vector<const char*> args,
                                          std::string* error = nullptr) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>("receiver")); // argv[0]
    for (const char* a : args) argv.push_back(const_cast<char*>(a));
    return parse_receiver_args(static_cast<int>(argv.size()), argv.data(), error);
}

// =============================================================================
// Tests
// =============================================================================

static void test_parses_both_flags() {
    TEST(parses_both_flags);

    auto args = parse({"--receiver-id", "1", "--listen-port", "9101"});
    ASSERT_TRUE(args.has_value());
    ASSERT_EQ(args->receiver_id, 1u);
    ASSERT_EQ(args->listen_port, 9101u);
}

static void test_socket_defaults_when_not_given() {
    TEST(socket_defaults_when_not_given);

    auto args = parse({"--receiver-id", "0", "--listen-port", "9100"});
    ASSERT_TRUE(args.has_value());
    ASSERT_TRUE(args->socket_path == DEFAULT_SESSION_MANAGER_SOCKET_PATH);
}

static void test_socket_override_accepted() {
    TEST(socket_override_accepted);

    auto args = parse({"--receiver-id", "0", "--listen-port", "9100",
                        "--socket", "/tmp/native-test.sock"});
    ASSERT_TRUE(args.has_value());
    ASSERT_TRUE(args->socket_path == "/tmp/native-test.sock");
}

static void test_order_independent() {
    TEST(order_independent);

    // C's supervisor always emits --receiver-id before --listen-port, but
    // nothing about the contract says a caller can rely on that order.
    auto args = parse({"--listen-port", "9102", "--receiver-id", "2"});
    ASSERT_TRUE(args.has_value());
    ASSERT_EQ(args->receiver_id, 2u);
    ASSERT_EQ(args->listen_port, 9102u);
}

static void test_matches_real_invocation_shape() {
    TEST(matches_real_invocation_shape);

    // Exactly what Session-Manager/src/session_manager/main.py:
    // _build_receiver_specs() actually emits for receiver index 0 with
    // config.toml's real ports = [9100, 9101, 9102].
    auto args = parse({"--receiver-id", "0", "--listen-port", "9100"});
    ASSERT_TRUE(args.has_value());
    ASSERT_EQ(args->receiver_id, 0u);
    ASSERT_EQ(args->listen_port, 9100u);
}

static void test_missing_receiver_id_fails() {
    TEST(missing_receiver_id_fails);

    std::string error;
    auto args = parse({"--listen-port", "9100"}, &error);
    ASSERT_TRUE(!args.has_value());
    ASSERT_TRUE(error.find("--receiver-id") != std::string::npos);
}

static void test_missing_listen_port_fails() {
    TEST(missing_listen_port_fails);

    std::string error;
    auto args = parse({"--receiver-id", "0"}, &error);
    ASSERT_TRUE(!args.has_value());
    ASSERT_TRUE(error.find("--listen-port") != std::string::npos);
}

static void test_flag_with_no_value_fails() {
    TEST(flag_with_no_value_fails);

    std::string error;
    auto args = parse({"--receiver-id", "0", "--listen-port"}, &error);
    ASSERT_TRUE(!args.has_value());
    ASSERT_TRUE(error.find("--listen-port") != std::string::npos);
}

static void test_non_numeric_value_fails() {
    TEST(non_numeric_value_fails);

    std::string error;
    auto args = parse({"--receiver-id", "not-a-number", "--listen-port", "9100"}, &error);
    ASSERT_TRUE(!args.has_value());
    ASSERT_TRUE(error.find("--receiver-id") != std::string::npos);
}

static void test_trailing_garbage_in_number_fails() {
    TEST(trailing_garbage_in_number_fails);

    // "9100garbage" must NOT silently parse as 9100 -- std::from_chars
    // reports how much it consumed, and parse_uint() checks that the
    // whole string was consumed.
    std::string error;
    auto args = parse({"--receiver-id", "0", "--listen-port", "9100garbage"}, &error);
    ASSERT_TRUE(!args.has_value());
}

static void test_port_out_of_range_fails() {
    TEST(port_out_of_range_fails);

    // 70000 doesn't fit in a uint16_t (max 65535).
    std::string error;
    auto args = parse({"--receiver-id", "0", "--listen-port", "70000"}, &error);
    ASSERT_TRUE(!args.has_value());
}

static void test_negative_value_fails() {
    TEST(negative_value_fails);

    // Both fields are unsigned -- a negative value must be rejected, not
    // silently wrapped into a huge unsigned number.
    std::string error;
    auto args = parse({"--receiver-id", "-1", "--listen-port", "9100"}, &error);
    ASSERT_TRUE(!args.has_value());
}

static void test_unrecognized_argument_fails() {
    TEST(unrecognized_argument_fails);

    std::string error;
    auto args = parse({"--receiver-id", "0", "--listen-port", "9100", "--bogus"}, &error);
    ASSERT_TRUE(!args.has_value());
    ASSERT_TRUE(error.find("--bogus") != std::string::npos);
}

static void test_no_arguments_fails() {
    TEST(no_arguments_fails);

    std::string error;
    auto args = parse({}, &error);
    ASSERT_TRUE(!args.has_value());
}

static void test_listen_port_zero_is_accepted() {
    TEST(listen_port_zero_is_accepted);

    // Not something C's supervisor would ever pass, but 0 is a
    // syntactically valid uint16_t -- rejecting it would be an
    // undocumented extra restriction this function has no basis for.
    // Whether port 0 makes operational sense is UdpReceiver::bind()'s
    // concern (where it means "kernel picks one"), not this parser's.
    auto args = parse({"--receiver-id", "0", "--listen-port", "0"});
    ASSERT_TRUE(args.has_value());
    ASSERT_EQ(args->listen_port, 0u);
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "=== Receiver Args Tests (Phase 8 wiring) ===\n\n";

    RUN(test_parses_both_flags);
    RUN(test_socket_defaults_when_not_given);
    RUN(test_socket_override_accepted);
    RUN(test_order_independent);
    RUN(test_matches_real_invocation_shape);
    RUN(test_missing_receiver_id_fails);
    RUN(test_missing_listen_port_fails);
    RUN(test_flag_with_no_value_fails);
    RUN(test_non_numeric_value_fails);
    RUN(test_trailing_garbage_in_number_fails);
    RUN(test_port_out_of_range_fails);
    RUN(test_negative_value_fails);
    RUN(test_unrecognized_argument_fails);
    RUN(test_no_arguments_fails);
    RUN(test_listen_port_zero_is_accepted);

    std::cout << "\n=== Results: " << g_pass << " run, "
              << g_fail << " failed ===\n";

    return g_fail == 0 ? 0 : 1;
}
