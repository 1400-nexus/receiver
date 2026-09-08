#include "receiver/rs_codec.hpp"

#include <isa-l/erasure_code.h>

#include <cstring>

RsCodec::RsCodec(uint32_t k, uint32_t n, uint32_t symbol_bytes)
    : k_(k), n_(n), symbol_bytes_(symbol_bytes), encode_matrix_(uint64_t(n) * k) {
    // Cauchy, not Vandermonde -- see the header comment for why this
    // isn't a style preference. Builds the full n*k matrix: rows [0,k)
    // are the identity block (systematic -- symbol i, i<k, IS data symbol
    // i, untouched), rows [k,n) are the Cauchy-generated parity
    // coefficients.
    gf_gen_cauchy1_matrix(encode_matrix_.data(), static_cast<int>(n_),
                           static_cast<int>(k_));
}

bool RsCodec::decode(const uint8_t* const* symbols, const bool* present,
                      uint8_t* out_data) const {
    // Fast path: nothing to reconstruct. A block that only lost parity
    // symbols needs no linear algebra at all -- matches A's
    // fake_receiver.py's own optimization (docs/ANSWERS_FROM_A.md §9).
    bool all_data_present = true;
    for (uint32_t i = 0; i < k_; ++i) {
        if (!present[i]) { all_data_present = false; break; }
    }
    if (all_data_present) {
        for (uint32_t i = 0; i < k_; ++i) {
            std::memcpy(out_data + uint64_t(i) * symbol_bytes_, symbols[i], symbol_bytes_);
        }
        return true;
    }

    // Collect present symbol indices (data or parity, doesn't matter --
    // that's the whole point of an erasure code) and take the first k of
    // them. Any k linearly-independent rows of the encode matrix work;
    // taking them in index order is simple, deterministic, and correct.
    std::vector<uint32_t> chosen;
    chosen.reserve(k_);
    for (uint32_t i = 0; i < n_ && chosen.size() < k_; ++i) {
        if (present[i]) chosen.push_back(i);
    }
    if (chosen.size() < k_) {
        return false; // insufficient symbols -- mathematically impossible, not just imperfect
    }

    // Build the k*k submatrix from the chosen rows of the full encode
    // matrix, then invert it. This inverted matrix, applied to exactly
    // these k (encoded) symbols, recovers all k original data symbols --
    // the standard erasure-decode construction: the first k rows of the
    // real encode matrix are the identity, so encode_matrix^-1 applied to
    // any k linearly-independent encoded outputs recovers the original
    // data by the same linear relationship that produced them.
    std::vector<uint8_t> sub_matrix(uint64_t(k_) * k_);
    for (uint32_t r = 0; r < k_; ++r) {
        std::memcpy(&sub_matrix[uint64_t(r) * k_],
                    &encode_matrix_[uint64_t(chosen[r]) * k_], k_);
    }

    std::vector<uint8_t> decode_matrix(uint64_t(k_) * k_);
    if (gf_invert_matrix(sub_matrix.data(), decode_matrix.data(),
                          static_cast<int>(k_)) != 0) {
        // Singular submatrix. Shouldn't happen with a valid Cauchy matrix
        // and k distinct rows -- every k*k submatrix of a Cauchy matrix is
        // invertible by construction, which is the entire reason Cauchy
        // was chosen over Vandermonde. Treated as decode failure rather
        // than undefined behavior if it ever somehow does.
        return false;
    }

    std::vector<uint8_t> tables(uint64_t(k_) * k_ * 32);
    ec_init_tables(static_cast<int>(k_), static_cast<int>(k_),
                   decode_matrix.data(), tables.data());

    std::vector<uint8_t*> src(k_);
    for (uint32_t r = 0; r < k_; ++r) {
        // ec_encode_data's signature wants non-const source pointers even
        // though it never writes through them -- a long-standing ISA-L
        // API wart, not a sign this function mutates its input.
        src[r] = const_cast<uint8_t*>(symbols[chosen[r]]);
    }

    std::vector<uint8_t*> dst(k_);
    for (uint32_t i = 0; i < k_; ++i) {
        dst[i] = out_data + uint64_t(i) * symbol_bytes_;
    }

    ec_encode_data(static_cast<int>(symbol_bytes_), static_cast<int>(k_),
                   static_cast<int>(k_), tables.data(), src.data(), dst.data());

    return true;
}

void RsCodec::encode_for_testing(const uint8_t* const* data_symbols,
                                  uint8_t* out_parity) const {
    const uint32_t p = n_ - k_; // parity symbol count

    // Tables built from the parity rows only (rows [k,n) of the full
    // matrix) -- skips the identity block on top, matching A's own
    // `matrix_.data() + k*k` (docs/ANSWERS_FROM_A.md §8).
    std::vector<uint8_t> tables(uint64_t(k_) * p * 32);
    ec_init_tables(static_cast<int>(k_), static_cast<int>(p),
                   const_cast<uint8_t*>(&encode_matrix_[uint64_t(k_) * k_]),
                   tables.data());

    std::vector<uint8_t*> src(k_);
    for (uint32_t i = 0; i < k_; ++i) {
        src[i] = const_cast<uint8_t*>(data_symbols[i]);
    }

    std::vector<uint8_t*> dst(p);
    for (uint32_t i = 0; i < p; ++i) {
        dst[i] = out_parity + uint64_t(i) * symbol_bytes_;
    }

    ec_encode_data(static_cast<int>(symbol_bytes_), static_cast<int>(k_),
                   static_cast<int>(p), tables.data(), src.data(), dst.data());
}
