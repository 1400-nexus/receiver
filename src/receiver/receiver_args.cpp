#include "receiver/receiver_args.hpp"

#include <charconv>
#include <cstring>

const char* const RECEIVER_USAGE =
    "usage: receiver --receiver-id <uint32> --listen-port <uint16> [--socket <path>]";

namespace {

// std::from_chars, not std::stoul()/atoi(): no exceptions (this codebase
// otherwise avoids them -- ShmManager/UdpReceiver both report failure via
// return value), no locale-dependent parsing, and it reports exactly how
// much of the string it consumed -- "9100garbage" is correctly rejected
// as invalid instead of silently parsed as 9100. Also correctly rejects
// an out-of-range value (e.g. a port > 65535 into a uint16_t) via
// std::errc::result_out_of_range, with no risk of silent truncation.
template <typename T>
bool parse_uint(const char* s, T* out) {
    if (s == nullptr || *s == '\0') return false;
    const char* end = s + std::strlen(s);
    const auto [ptr, ec] = std::from_chars(s, end, *out);
    return ec == std::errc() && ptr == end;
}

} // namespace

std::optional<ReceiverArgs> parse_receiver_args(int argc, char** argv,
                                                 std::string* out_error) {
    auto fail = [&](const std::string& msg) -> std::optional<ReceiverArgs> {
        if (out_error) *out_error = msg;
        return std::nullopt;
    };

    std::optional<uint32_t> receiver_id;
    std::optional<uint16_t> listen_port;
    std::string socket_path = DEFAULT_SESSION_MANAGER_SOCKET_PATH;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--receiver-id") {
            if (i + 1 >= argc) return fail("--receiver-id requires a value");
            uint32_t v = 0;
            if (!parse_uint(argv[++i], &v)) {
                return fail(std::string("--receiver-id: not a valid uint32: ") + argv[i]);
            }
            receiver_id = v;
        } else if (arg == "--listen-port") {
            if (i + 1 >= argc) return fail("--listen-port requires a value");
            uint16_t v = 0;
            if (!parse_uint(argv[++i], &v)) {
                return fail(std::string("--listen-port: not a valid uint16 (0-65535): ") + argv[i]);
            }
            listen_port = v;
        } else if (arg == "--socket") {
            if (i + 1 >= argc) return fail("--socket requires a value");
            socket_path = argv[++i];
            if (socket_path.empty()) return fail("--socket: path must not be empty");
        } else {
            return fail("unrecognized argument: " + arg);
        }
    }

    if (!receiver_id.has_value()) return fail("missing required --receiver-id");
    if (!listen_port.has_value()) return fail("missing required --listen-port");

    ReceiverArgs args;
    args.receiver_id = *receiver_id;
    args.listen_port = *listen_port;
    args.socket_path = socket_path;
    return args;
}
