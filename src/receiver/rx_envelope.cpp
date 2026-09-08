#include "receiver/rx_envelope.hpp"
#include "receiver/proto_hash.hpp"

std::string build_receiver_hello(uint32_t receiver_id, uint32_t pid,
                                  uint16_t listen_port) {
    nexus::rx::RxEnvelope env;
    auto* hello = env.mutable_receiver_hello();
    hello->set_receiver_id(receiver_id);
    hello->set_pid(pid);
    hello->set_listen_port(listen_port);
    // Raw 32 bytes, NOT hex-encoded -- ReceiverHello.proto_hash is a
    // `bytes` field, and session_manager's real compute_proto_hash()
    // returns hasher.digest() (raw), not .hexdigest() (docs/
    // ANSWERS_FROM_C.md §5, cross-checked directly against
    // ipc/handshake.py rather than trusted from A's prose description
    // alone, which said "lowercase hex" -- the real code is authoritative
    // here, per this whole project's own standard).
    hello->set_proto_hash(NEXUS_PROTO_HASH, NEXUS_PROTO_HASH_SIZE);

    std::string out;
    env.SerializeToString(&out);
    return out;
}

std::string build_heartbeat(uint32_t process_id, uint64_t timestamp_unix_ms) {
    nexus::rx::RxEnvelope env;
    auto* hb = env.mutable_heartbeat();
    hb->set_process_id(process_id);
    hb->set_timestamp_unix_ms(timestamp_unix_ms);

    std::string out;
    env.SerializeToString(&out);
    return out;
}

std::string build_manifest_seen(uint32_t receiver_id,
                                 const nexus::common::Manifest& manifest) {
    nexus::rx::RxEnvelope env;
    auto* ms = env.mutable_manifest_seen();
    ms->set_receiver_id(receiver_id);
    *ms->mutable_manifest() = manifest;

    std::string out;
    env.SerializeToString(&out);
    return out;
}

std::string build_receiver_stats(const nexus::rx::ReceiverStats& stats) {
    nexus::rx::RxEnvelope env;
    *env.mutable_receiver_stats() = stats;

    std::string out;
    env.SerializeToString(&out);
    return out;
}

std::string build_block_decoded(const std::string& session_id, uint32_t receiver_id,
                                 const std::vector<uint32_t>& block_ids) {
    nexus::rx::RxEnvelope env;
    auto* bd = env.mutable_block_decoded();
    bd->set_session_id(session_id);
    bd->set_receiver_id(receiver_id);
    for (uint32_t id : block_ids) bd->add_block_ids(id);

    std::string out;
    env.SerializeToString(&out);
    return out;
}

bool parse_incoming(const std::string& raw, nexus::rx::RxEnvelope* out_envelope,
                     RxEnvelopeCase* out_case) {
    if (!out_envelope->ParseFromString(raw)) {
        return false;
    }

    switch (out_envelope->msg_case()) {
        case nexus::rx::RxEnvelope::kSessionOpen:
            *out_case = RxEnvelopeCase::SessionOpen;
            break;
        case nexus::rx::RxEnvelope::kPurgeSession:
            *out_case = RxEnvelopeCase::PurgeSession;
            break;
        case nexus::rx::RxEnvelope::kConfig:
            *out_case = RxEnvelopeCase::Config;
            break;
        case nexus::rx::RxEnvelope::kHeartbeat:
            *out_case = RxEnvelopeCase::Heartbeat;
            break;
        case nexus::rx::RxEnvelope::kReceiverHello:
        case nexus::rx::RxEnvelope::kManifestSeen:
        case nexus::rx::RxEnvelope::kBlockDecoded:
        case nexus::rx::RxEnvelope::kReceiverStats:
            // Receiver->manager-only cases. A well-formed manager never
            // sends these back to a receiver -- reported distinctly
            // rather than silently folded into Unset, so a caller can
            // tell "nothing was set" apart from "something was set, but
            // it's the wrong direction."
            *out_case = RxEnvelopeCase::OtherReceiverOriginated;
            break;
        case nexus::rx::RxEnvelope::MSG_NOT_SET:
        default:
            *out_case = RxEnvelopeCase::Unset;
            break;
    }

    return true;
}
