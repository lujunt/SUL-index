#include "sul/z_order.h"

#include <algorithm>
#include <cmath>

namespace sul {

ZOrderEncoder::ZOrderEncoder(int32_t dim_count) : dim_count_(dim_count) {
    if (dim_count_ < 1) dim_count_ = 1;
    if (dim_count_ > MAX_DIMS) dim_count_ = MAX_DIMS;
    bits_per_dim_ = 64 / dim_count_;
    if (bits_per_dim_ > 32) bits_per_dim_ = 32;
}

uint64_t ZOrderEncoder::encode(const int32_t* coords) const {
    uint64_t result = 0;
    for (int32_t b = 0; b < bits_per_dim_; ++b) {
        for (int32_t d = 0; d < dim_count_; ++d) {
            uint64_t bit = (static_cast<uint32_t>(coords[d]) >> b) & 1ULL;
            result |= bit << (b * dim_count_ + d);
        }
    }
    return result;
}

void ZOrderEncoder::to_bytes(uint64_t z, uint8_t out[KEY_BYTES]) {
    for (int32_t i = 0; i < KEY_BYTES; ++i) {
        int32_t shift = (KEY_BYTES - 1 - i) * 8;
        out[i] = static_cast<uint8_t>((z >> shift) & 0xFFULL);
    }
}

int32_t scale_unit_double_to_int32(double x) {
    if (x < 0.0) x = 0.0;
    if (x >= 1.0) x = 1.0 - 1e-12;
    double scaled = x * static_cast<double>(INT32_MAX);
    return static_cast<int32_t>(scaled);
}

}
