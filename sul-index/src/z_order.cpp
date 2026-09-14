#include "sul/z_order.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sul {

ZOrderEncoder::ZOrderEncoder(int32_t dim_count) : dim_count_(dim_count) {
    if (dim_count_ < 1) dim_count_ = 1;
    if (dim_count_ > MAX_DIMS) dim_count_ = MAX_DIMS;
    bits_per_dim_ = BITS_PER_DIM;                        // 固定16位/维
    key_len_ = bits_per_dim_ * dim_count_ / 8;           // 2D→4字节，4D→8字节，6D→12字节
}

// 将各维度低 bits_per_dim_ 位交错编码为 128 位（6 维 = 96 位完整保留）
__uint128_t ZOrderEncoder::encode(const int32_t* coords) const {
    __uint128_t result = 0;
    for (int32_t b = 0; b < bits_per_dim_; ++b) {
        for (int32_t d = 0; d < dim_count_; ++d) {
            __uint128_t bit = (static_cast<uint32_t>(coords[d]) >> b) & 1u;
            result |= bit << (b * dim_count_ + d);
        }
    }
    return result;
}

// 将各维度低16位交错编码后直接写入大端序字节数组
// 位位置p（0=LSB）映射到字节下标 (total_bits-1-p)/8，位内偏移 p%8
void ZOrderEncoder::encode_to_bytes(const int32_t* coords, uint8_t* out) const {
    int32_t total_bits = bits_per_dim_ * dim_count_;
    std::memset(out, 0, static_cast<size_t>(key_len_));
    for (int32_t b = 0; b < bits_per_dim_; ++b) {
        for (int32_t d = 0; d < dim_count_; ++d) {
            int32_t p          = b * dim_count_ + d;         // z值中的全局位位置
            int32_t byte_idx   = (total_bits - 1 - p) / 8;  // 大端序字节下标
            int32_t bit_offset = p % 8;
            uint8_t bit = (static_cast<uint32_t>(coords[d]) >> b) & 1u;
            out[byte_idx] |= static_cast<uint8_t>(bit << bit_offset);
        }
    }
}

// 将 128 位 z 值转为 key_len 字节大端序数组（字节序与 encode_to_bytes 一致）
void ZOrderEncoder::to_bytes(__uint128_t z, uint8_t* out, int32_t key_len) {
    for (int32_t i = 0; i < key_len; ++i) {
        int32_t shift = (key_len - 1 - i) * 8;
        out[i] = (shift < 128) ? static_cast<uint8_t>((z >> shift) & 0xFFu) : 0u;
    }
}

// 将[0,1)浮点值缩放到[0, 2^BITS_PER_DIM)整数范围
// 使坐标值与z曲线编码位宽一致，避免高位信息丢失导致范围查询错位
int32_t scale_unit_double_to_int32(double x) {
    if (x < 0.0) x = 0.0;
    if (x >= 1.0) x = 1.0 - 1e-12;
    return static_cast<int32_t>(x * static_cast<double>(1u << BITS_PER_DIM));
}

}
