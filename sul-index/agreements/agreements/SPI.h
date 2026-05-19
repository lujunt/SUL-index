#ifndef SPI_H
#define SPI_H

#include <ophelib/paillier_fast.h>
#include <vector>

using namespace ophelib;

// SPI 协议：Secure Point-In-range
// 判断密文数据点是否落在密文范围查询 Q = [ql, qr] 内
// 逐维度对 ql[d]<=coords[d]<=qr[d] 调用 SIC 比较，所有维度都满足时返回 1
//
// 入参：
//   enc_coords  : 数据点各维度的加密坐标，size = dim_count
//   enc_ql      : 范围下界各维度加密坐标
//   enc_qr      : 范围上界各维度加密坐标
//   paillier    : Paillier 加解密上下文
//
// 出参：Integer，1 表示点在范围内，0 表示不在
//
// 模拟说明：DSP 持有所有密文，调用 SICrun 时由 DAP 配合完成比较
Integer SPIrun(const std::vector<Ciphertext>& enc_coords,
               const std::vector<Ciphertext>& enc_ql,
               const std::vector<Ciphertext>& enc_qr,
               PaillierFast& paillier);

#endif // SPI_H
