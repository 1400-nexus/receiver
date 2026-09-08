#pragma once
// =============================================================================
// RsCodec — Phase 10 (AGENT_IMPLEMENTATION.md §27): Reed-Solomon decode via
// Intel ISA-L.
//
// The matrix/algorithm choices here are not this file's invention -- they
// were confirmed against Person A's real sender code, not guessed or taken
// from A's_guide.txt alone (docs/ANSWERS_FROM_A.md §8-9):
//
//   - Cauchy matrix (gf_gen_cauchy1_matrix), NOT Vandermonde. Vandermonde
//     does not guarantee every k*k submatrix is invertible once k+p is
//     large, and at k=200,p=55 that matters -- the failure mode is decode
//     working for some loss patterns and silently failing for others.
//   - Systematic form: symbols 0..k-1 are the raw source data, unmodified;
//     symbols k..n-1 are parity, in ec_encode_data's own output order.
//   - k/n/symbol_bytes are PER-SESSION values carried in the Manifest --
//     never hardcoded (A's answer, §10). This class takes them as
//     constructor parameters for exactly that reason.
//
// This class only decodes. The receiver never encodes real data -- that's
// the sender's job -- but `encode()` exists anyway, clearly marked
// test-only, because a Reed-Solomon decoder is untestable without a way to
// produce valid encoded fixtures, and duplicating the same matrix
// construction in a second, test-only class would be exactly the kind of
// "two implementations of the same thing" this whole project has spent
// effort avoiding elsewhere (CRC16, proto_hash).
//
// Author: Person B (C++ Receiver)
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <vector>

class RsCodec {
public:
    // k = data symbols, n = total symbols (data + parity), symbol_bytes =
    // size of each symbol in bytes. Builds the n*k Cauchy encode matrix
    // once; every decode() call reuses it (only the k*k submatrix
    // selection + inversion happens per call, since that depends on which
    // symbols are actually present for a given block).
    RsCodec(uint32_t k, uint32_t n, uint32_t symbol_bytes);

    uint32_t k() const { return k_; }
    uint32_t n() const { return n_; }
    uint32_t symbol_bytes() const { return symbol_bytes_; }

    // Reconstruct the k data symbols (0..k-1) from whatever symbols are
    // present. `symbols[i]` for i in [0,n) must point at symbol i's bytes
    // (symbol_bytes() long) when present[i] is true; symbols[i] itself is
    // never read when present[i] is false, so a null/dangling pointer
    // there is fine.
    //
    // Needs at least k of the n symbols present, in ANY combination of
    // data/parity -- that's the whole point of an erasure code. On
    // success, `out_data` (caller-owned, k()*symbol_bytes() bytes) holds
    // the k reconstructed data symbols in order, and this returns true.
    //
    // Returns false only when fewer than k symbols are present --
    // decode is mathematically impossible, not just imperfect, and no
    // partial/best-effort output is written to out_data in that case.
    //
    // Fast path: if all k data symbols (0..k-1) are already present, this
    // is a memcpy, no linear algebra at all -- per A's own fake_receiver,
    // a block that lost only parity symbols needs no decode.
    bool decode(const uint8_t* const* symbols, const bool* present,
                uint8_t* out_data) const;

    // TEST-ONLY. The real receiver never encodes -- A's sender does, and
    // A's real encoder is what actually has to match this decoder, not
    // the other way around. This exists purely so this class's own tests
    // (and any future diagnostic self-check) can produce valid n-symbol
    // encoded blocks from k data symbols without a second, divergent
    // matrix implementation. `out_parity` (caller-owned, (n()-k())*
    // symbol_bytes() bytes) receives the n-k parity symbols; the k data
    // symbols are the caller's own input, unmodified (systematic form).
    void encode_for_testing(const uint8_t* const* data_symbols,
                             uint8_t* out_parity) const;

private:
    uint32_t k_, n_, symbol_bytes_;
    std::vector<uint8_t> encode_matrix_; // n_ * k_ bytes: gf_gen_cauchy1_matrix's output
};
