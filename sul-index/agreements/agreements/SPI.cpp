#include "SPI.h"
#include "SIC.h"

#include <ophelib/paillier_fast.h>

using namespace ophelib;

// SPI 协议实现：逐维度 SIC 比较
// 对每个维度 d：
//   1) cmp_low  = SIC(ql[d], coords[d])  应返回 1（ql<=coords）
//   2) cmp_high = SIC(coords[d], qr[d])  应返回 1（coords<=qr）
// 任一维度不满足则点不在范围内
Integer SPIrun(const std::vector<Ciphertext>& enc_coords,
               const std::vector<Ciphertext>& enc_ql,
               const std::vector<Ciphertext>& enc_qr,
               PaillierFast& paillier) {
    // DSP/DAP: SICrun 内部已包含双方交互，此处仅串联
    const size_t dim = enc_coords.size();
    if (dim == 0 || enc_ql.size() != dim || enc_qr.size() != dim) {
        return Integer(0);
    }

    for (size_t d = 0; d < dim; ++d) {
        // 第 d 维下界检查：ql[d] <= coords[d]
        Integer cmp_low = SICrun(enc_ql[d], enc_coords[d], paillier);
        if (cmp_low != 1) {
            return Integer(0);
        }
        // 第 d 维上界检查：coords[d] <= qr[d]
        Integer cmp_high = SICrun(enc_coords[d], enc_qr[d], paillier);
        if (cmp_high != 1) {
            return Integer(0);
        }
    }
    return Integer(1);
}
