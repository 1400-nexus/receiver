#pragma once
// =============================================================================
// receiver_args.hpp -- CLI argument parsing for the receiver binary.
//
// C's session_manager spawns each receiver like this
// (Session-Manager/src/session_manager/main.py:_build_receiver_specs()):
//
//   argv = [binary_path, "--receiver-id", str(index), "--listen-port", str(port)]
//
// A receiver learns its own identity and port from argv at spawn time --
// there is no config file or hardcoded constant on this side to read them
// from instead (docs/CROSS_TEAM_ANSWERS.md, Q8/Q14 addendum). This is the
// parser for that contract.
//
// `--socket` is NOT part of C's current spawn argv (checked -- only
// --receiver-id/--listen-port are passed) but is accepted here anyway,
// optional with a default: RECEIVER_CONTRACT.md §1 documents the UDS path
// as "the path in [paths].socket_path (/run/nexus/session-manager.sock in
// the container)" -- a container-specific value, not something confirmed
// for a native run. Rather than hardcode that one value with no way to
// override it if it's wrong outside a container, or guess at an env var
// name I haven't independently verified, this stays a real CLI flag --
// forward-compatible if C's spawn logic ever passes it, and always
// overridable for native runs/testing in the meantime.
//
// Author: Person B (C++ Receiver)
// =============================================================================

#include <cstdint>
#include <optional>
#include <string>

// RECEIVER_CONTRACT.md §1's documented container path -- see the note
// above on why this is a default, not the only option.
constexpr const char* DEFAULT_SESSION_MANAGER_SOCKET_PATH = "/run/nexus/session-manager.sock";

struct ReceiverArgs {
    uint32_t receiver_id;
    uint16_t listen_port;
    std::string socket_path = DEFAULT_SESSION_MANAGER_SOCKET_PATH;
};

// Parses "--receiver-id <uint32> --listen-port <uint16> [--socket <path>]"
// out of argv (the first two required, order-independent; argv[0] -- the
// binary's own path -- is skipped, matching the usual C convention). On
// failure, returns nullopt and writes a human-readable reason to
// *out_error (missing flag, missing value, non-numeric value, value out
// of range for its type) -- this function only parses; deciding what to
// do about a bad invocation (print usage, exit non-zero, ...) is main()'s
// job, not this one's.
std::optional<ReceiverArgs> parse_receiver_args(int argc, char** argv,
                                                 std::string* out_error);

// Usage string for a parse failure or --help.
extern const char* const RECEIVER_USAGE;
