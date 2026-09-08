#pragma once
// =============================================================================
// rx_envelope — Phase 12: build/parse `rx.proto RxEnvelope` messages for
// the receiver<->session_manager UDS control plane.
//
// This is the UDS-side equivalent of frame_dispatcher.hpp (Phase 8), and
// draws the same boundary: pure wire-shape translation, no business
// logic. Building a message here doesn't decide WHEN to send it or what
// values to put in it beyond what's passed in; parsing here doesn't
// decide what to DO with an inbound SessionOpen/PurgeSession/Config —
// that's Phase 9+ territory.
//
// Message shapes, field names, and the "no framing beyond one serialized
// envelope per SOCK_SEQPACKET message" contract are all confirmed against
// session_manager's real code, not assumed — see
// docs/ANSWERS_FROM_C.md §5/§15 and ipc/{codec,message_types,constants}.py.
//
// Author: Person B (C++ Receiver)
// =============================================================================

#include "rx.pb.h"

#include <cstdint>
#include <string>
#include <vector>

// --- Outbound (receiver -> manager) -----------------------------------------
//
// Each of these builds the message, wraps it in an RxEnvelope, and
// serializes it — ready for UdsChannel::send(). Field VALUES are taken as
// parameters rather than assembled internally (no reading global state,
// no picking a timestamp) — keeping this layer a pure translator.

// `receiver_id`/`pid`/`listen_port` are exactly what
// docs/CROSS_TEAM_ANSWERS.md's Q8/Q14 confirmed a receiver reports about
// itself (the manager does not assign the port). `proto_hash` is NOT a
// parameter — it's the build-time constant from the generated
// proto_hash.hpp (Phase 12's own CMake step), included directly here,
// since it's the same for every ReceiverHello this binary will ever send
// and there's nothing a caller could meaningfully override it with.
std::string build_receiver_hello(uint32_t receiver_id, uint32_t pid,
                                  uint16_t listen_port);

std::string build_heartbeat(uint32_t process_id, uint64_t timestamp_unix_ms);

std::string build_manifest_seen(uint32_t receiver_id,
                                 const nexus::common::Manifest& manifest);

std::string build_receiver_stats(const nexus::rx::ReceiverStats& stats);

// `block_ids` is `repeated uint32` on the wire — confirmed batched, not
// one message per block (docs/ANSWERS_FROM_C.md §15,
// aggregator.py's handle_block_decoded loops over multiple ids from one
// message).
std::string build_block_decoded(const std::string& session_id, uint32_t receiver_id,
                                 const std::vector<uint32_t>& block_ids);

// --- Inbound (manager -> receiver) ------------------------------------------

// Which RxEnvelope case an inbound message was. Deliberately does not
// list ReceiverHello/ManifestSeen/ReceiverStats/BlockDecoded as expected
// values — those are receiver->manager only; parse_incoming() still
// reports them via OtherReceiverOriginated if one somehow arrives, rather
// than silently misreporting it as Unset.
enum class RxEnvelopeCase : uint32_t {
    Unset,                     // MSG_NOT_SET -- malformed/empty envelope
    SessionOpen,
    PurgeSession,
    Config,
    Heartbeat,
    OtherReceiverOriginated,   // a receiver->manager-only case arrived here somehow
};

// Parses raw bytes (from UdsChannel::receive()) as an RxEnvelope. On
// success (parse succeeds, regardless of which case), `out_case` says
// which case it was and `out_envelope` holds the parsed message — read
// the matching field (out_envelope->session_open(), etc.) once out_case
// says which one is valid. Returns false only if the bytes don't parse
// as a valid RxEnvelope at all (`out_case`/`out_envelope` are untouched
// in that case).
bool parse_incoming(const std::string& raw, nexus::rx::RxEnvelope* out_envelope,
                     RxEnvelopeCase* out_case);
