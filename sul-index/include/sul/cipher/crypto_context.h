#pragma once

#include <ophelib/paillier_fast.h>

#include <cstdint>
#include <memory>

namespace sul::cipher {

// CryptoContext：封装 Paillier 密钥与"浮点⇄整数"缩放
// - 全局共享一个 PaillierFast 实例（构建/查询双方都引用）
// - SCALE 与 agreements/OSM.cpp 中常量一致（100000），用于将浮点 slope/intercept 转为整数
class CryptoContext {
public:
    static constexpr int32_t SCALE = 100000;

    explicit CryptoContext(int key_size = 1024);

    ophelib::PaillierFast& paillier() { return *paillier_; }
    const ophelib::PaillierFast& paillier() const { return *paillier_; }

    // 整数 ↔ 密文
    ophelib::Ciphertext encrypt_i64(int64_t v) const;
    ophelib::Ciphertext encrypt_int(const ophelib::Integer& v) const;
    ophelib::Integer    decrypt(const ophelib::Ciphertext& ct) const;

    // 浮点 ↔ 缩放整数（用于 slope / intercept 编码）
    ophelib::Integer scale_float(double v) const;
    double           unscale_float(const ophelib::Integer& v) const;

    // 缩放后整数加密（slope/intercept 专用）
    ophelib::Ciphertext encrypt_float(double v) const;

    // 全局加密 0/1 常量（OSM/SIC 协议需要）
    const ophelib::Ciphertext& enc_zero() const { return *enc_zero_; }
    const ophelib::Ciphertext& enc_one()  const { return *enc_one_;  }

private:
    std::unique_ptr<ophelib::PaillierFast> paillier_;
    std::unique_ptr<ophelib::Ciphertext>   enc_zero_;
    std::unique_ptr<ophelib::Ciphertext>   enc_one_;
};

} // namespace sul::cipher
