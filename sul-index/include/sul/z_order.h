#pragma once

#include "sul/types.h"

namespace sul {

class ZOrderEncoder {
public:
    explicit ZOrderEncoder(int32_t dim_count);

    // 编码为uint64_t（适用于≤4维；6维时高位截断，仅供排序近似用）
    uint64_t encode(const int32_t* coords) const;

    // 直接编码到字节数组（适用于所有维度，大端序，ART key的正确来源）
    void encode_to_bytes(const int32_t* coords, uint8_t* out) const;

    // 将uint64_t z值转为key_len字节的大端序数组（用于范围查询边界转换）
    static void to_bytes(uint64_t z, uint8_t* out, int32_t key_len);

    int32_t dim_count()    const { return dim_count_; }
    int32_t bits_per_dim() const { return bits_per_dim_; }
    int32_t key_len()      const { return key_len_; }

private:
    int32_t dim_count_;
    int32_t bits_per_dim_; // 固定为BITS_PER_DIM=16
    int32_t key_len_;      // 实际key字节数 = bits_per_dim_ × dim_count_ / 8
};

// 将[0,1)浮点坐标缩放为int32整数（取低BITS_PER_DIM位参与z曲线编码）
int32_t scale_unit_double_to_int32(double x);

}
