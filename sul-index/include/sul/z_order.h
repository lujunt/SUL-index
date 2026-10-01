#pragma once

#include "sul/types.h"

namespace sul {

class ZOrderEncoder {
public:
    explicit ZOrderEncoder(int32_t dim_count);

    // Encode as a 128-bit Z-order value (all 96 bits of a 6D value are preserved).
    __uint128_t encode(const int32_t* coords) const;

    // Encode directly to a big-endian byte array, the canonical ART key for every dimension.
    void encode_to_bytes(const int32_t* coords, uint8_t* out) const;

    // Convert a 128-bit Z-order value to a key_len-byte big-endian range-query boundary.
    static void to_bytes(__uint128_t z, uint8_t* out, int32_t key_len);

    int32_t dim_count()    const { return dim_count_; }
    int32_t bits_per_dim() const { return bits_per_dim_; }
    int32_t key_len()      const { return key_len_; }

private:
    int32_t dim_count_;
    int32_t bits_per_dim_; // Fixed at BITS_PER_DIM = 16.
    int32_t key_len_;      // Actual key size = bits_per_dim_ * dim_count_ / 8 bytes.
};

// Scale a [0, 1) coordinate to int32; its low BITS_PER_DIM bits enter the Z-order encoding.
int32_t scale_unit_double_to_int32(double x);

}
