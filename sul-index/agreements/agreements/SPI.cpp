#include "SPI.h"
#include "SIC.h"

#include <ophelib/paillier_fast.h>
#include <thread>

using namespace ophelib;

// SPI 协议实现：逐维度 SIC 比较
// 对每个维度 d：
//   1) cmp_low  = SIC(ql[d], coords[d])  应返回 1（ql<=coords）
//   2) cmp_high = SIC(coords[d], qr[d])  应返回 1（coords<=qr）
// 优化：单维内 cmp_low / cmp_high 并行执行（额外线程跑 cmp_high，主线程跑 cmp_low）
// 跨维度保持顺序——保留维度间早返回（dim 0 失败时不进入 dim 1）
// 任一维度不满足则点不在范围内
Integer SPIrun(const std::vector<Ciphertext>& enc_coords,
               const std::vector<Ciphertext>& enc_ql,
               const std::vector<Ciphertext>& enc_qr,
               PaillierFast& paillier) {
    const size_t dim = enc_coords.size();
    if (dim == 0 || enc_ql.size() != dim || enc_qr.size() != dim) {
        return Integer(0);
    }

    for (size_t d = 0; d < dim; ++d) {
        Integer cmp_low;
        Integer cmp_high;
        std::thread t_high([&]() {
            cmp_high = SICrun(enc_coords[d], enc_qr[d], paillier);
        });
        cmp_low = SICrun(enc_ql[d], enc_coords[d], paillier);
        t_high.join();
        if (cmp_low != 1 || cmp_high != 1) {
            return Integer(0);
        }
    }
    return Integer(1);
}
