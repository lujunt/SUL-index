#pragma once

#include "sul/types.h"

namespace sul {

class ZOrderEncoder {
public:
    explicit ZOrderEncoder(int32_t dim_count);

    uint64_t encode(const int32_t* coords) const;

    static void to_bytes(uint64_t z, uint8_t out[KEY_BYTES]);

    int32_t dim_count() const { return dim_count_; }
    int32_t bits_per_dim() const { return bits_per_dim_; }

private:
    int32_t dim_count_;
    int32_t bits_per_dim_;
};

int32_t scale_unit_double_to_int32(double x);

}
