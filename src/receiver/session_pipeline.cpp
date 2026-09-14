#include "receiver/session_pipeline.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

SessionPipeline::SessionPipeline(ShmManager& shm) : shm_(shm) {}

bool SessionPipeline::handle_session_open(const nexus::rx::SessionOpen& msg, uint64_t file_size) {
    if (sessions_.count(msg.session_id())) {
        return true; // idempotent -- see the header comment
    }

    // msg.k()/msg.n()/msg.block_bytes() are per-session, taken exactly as
    // given -- never hardcoded (docs/ANSWERS_FROM_A.md §10). block_bytes()
    // is the SYMBOL size on the wire (docs/ANSWERS_FROM_C.md §0), which is
    // exactly what RsCodec/BlockWriter both expect as their symbol_bytes
    // parameter -- no k* multiplication here, that trap is exactly what
    // burned this project once already.
    auto session_idx = shm_.open_session(msg.session_id().c_str(),
                                          msg.k(), msg.n(), msg.block_bytes(),
                                          msg.total_blocks(), file_size,
                                          msg.dest_path().c_str());
    if (!session_idx.has_value()) return false;

    SessionContext ctx;
    ctx.session_idx  = *session_idx;
    ctx.k            = msg.k();
    ctx.n            = msg.n();
    ctx.symbol_bytes = msg.block_bytes();
    ctx.codec  = std::make_unique<RsCodec>(ctx.k, ctx.n, ctx.symbol_bytes);
    ctx.writer = std::make_unique<BlockWriter>();

    if (!ctx.writer->open(msg.dest_path().c_str(), file_size, ctx.k, ctx.symbol_bytes)) {
        return false;
    }

    sessions_.emplace(msg.session_id(), std::move(ctx));
    return true;
}

void SessionPipeline::purge_session(const std::string& session_id) {
    sessions_.erase(session_id);
}

DataPacketOutcome SessionPipeline::handle_data_packet(const std::string& session_id,
                                                        uint32_t block_id, uint32_t symbol_id,
                                                        const uint8_t* payload, size_t payload_len,
                                                        uint32_t* out_decoded_block_id) {
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return DataPacketOutcome::UnknownSession;
    }
    SessionContext& ctx = it->second;

    if (symbol_id >= ctx.n || payload_len != ctx.symbol_bytes) {
        return DataPacketOutcome::InvalidBlockOrSymbol;
    }

    auto block_opt = shm_.block(ctx.session_idx, block_id);
    if (!block_opt.has_value()) {
        return DataPacketOutcome::InvalidBlockOrSymbol; // block_id >= this session's total_blocks
    }
    BlockView block = *block_opt;

    // Check first, before allocating a slot for nothing -- is_present()
    // is a cheap acquire-load, cheaper than an alloc_slot() CAS we'd just
    // have to undo. This is only an optimization, not a correctness
    // guard -- register_symbol() itself is still the real, race-safe
    // arbiter (see below): two receivers can both pass this check for
    // the same symbol_id an instant apart, and that's fine.
    if (block.is_present(symbol_id)) {
        return DataPacketOutcome::Duplicate;
    }

    const uint32_t slot = shm_.alloc_slot();
    if (slot == SLOT_IDX_FREE) {
        return DataPacketOutcome::ArenaExhausted;
    }

    SlotView sv = shm_.slot(slot);
    std::memcpy(sv.data(), payload, payload_len);

    const auto result = block.register_symbol(symbol_id, slot);
    if (result == SymbolRegisterResult::Duplicate) {
        // Lost a race against another registration between our
        // is_present() check and here -- this call's slot was never
        // actually used, so free it (register_symbol()'s own documented
        // contract, shm_manager.hpp).
        shm_.free_slot(slot);
        return DataPacketOutcome::Duplicate;
    }
    if (result == SymbolRegisterResult::InvalidSymbolId) {
        // Can't actually happen given the symbol_id >= ctx.n check above
        // (ctx.n <= MAX_N always), but handled rather than assumed.
        shm_.free_slot(slot);
        return DataPacketOutcome::InvalidBlockOrSymbol;
    }

    // NewSymbol. Check the decode threshold -- only the registration that
    // actually crosses seen()>=k attempts the claim, matching the brief's
    // own "after registering a NEW symbol, check seen>=k" (§38).
    if (block.seen() >= ctx.k && block.try_claim_decode()) {
        if (decode_and_write_block(ctx, block_id, block)) {
            if (out_decoded_block_id) *out_decoded_block_id = block_id;
            return DataPacketOutcome::BlockDecoded;
        }
        // Won the claim but decode/write genuinely failed -- nothing sane
        // to do here but report this registration succeeded (the symbol
        // IS registered) without a completed block. The block stays
        // DECODE_CLAIMED permanently, which is the honest state -- "stuck",
        // not silently "fine." Real recovery (retry? alert operator?) is
        // future work, deliberately not guessed at in this pass.
    }

    return DataPacketOutcome::RegisteredOnly;
}

bool SessionPipeline::decode_and_write_block(SessionContext& ctx, uint32_t block_id,
                                              BlockView& block) {
    // Gather symbol pointers + presence for RsCodec::decode(). n is
    // small (<= MAX_N == 255), so fixed-size stack arrays -- no heap
    // allocation on this path, matching the no-per-packet-allocation
    // instinct from Phase 7 (this runs once per completed BLOCK, not per
    // packet, but the habit still costs nothing to keep).
    const uint8_t* symbols[MAX_N] = {};
    bool present[MAX_N] = {};
    for (uint32_t s = 0; s < ctx.n; ++s) {
        if (block.is_present(s)) {
            present[s] = true;
            symbols[s] = static_cast<const uint8_t*>(shm_.slot(block.slot_idx(s)).data());
        }
    }

    std::vector<uint8_t> decoded(uint64_t(ctx.k) * ctx.symbol_bytes);
    if (!ctx.codec->decode(symbols, present, decoded.data())) {
        return false; // shouldn't happen -- we only get here once seen()>=k
    }

    if (!ctx.writer->write_block(block_id, decoded.data())) return false;
    // Durable BEFORE this block is ever reported complete --
    // RECEIVER_CONTRACT.md §5 property 5, see BlockWriter::flush_block()'s
    // own comment for why this specific ordering is load-bearing.
    if (!ctx.writer->flush_block(block_id)) return false;
    if (!block.mark_decode_complete()) return false; // shouldn't happen -- we hold the claim

    // Release every slot this block was holding -- the bytes are durably
    // on disk now, nothing in the arena needs to keep them.
    for (uint32_t s = 0; s < ctx.n; ++s) {
        if (present[s]) shm_.free_slot(block.slot_idx(s));
    }

    return true;
}
