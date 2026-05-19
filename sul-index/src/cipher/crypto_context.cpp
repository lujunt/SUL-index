#include "sul/cipher/crypto_context.h"

#include <cmath>

// agreements/OSM.cpp 和 agreements/SIC.cpp 使用的全局加密 0/1 密文
// 这两个 extern 变量由 CryptoContext 构造时填充
ophelib::Ciphertext global_enc_zero;
ophelib::Ciphertext global_enc_one;

namespace sul::cipher {

CryptoContext::CryptoContext(int key_size) {
    paillier_ = std::make_unique<ophelib::PaillierFast>(key_size);
    paillier_->generate_keys();

    enc_zero_ = std::make_unique<ophelib::Ciphertext>(paillier_->encrypt(ophelib::Integer(0)));
    enc_one_  = std::make_unique<ophelib::Ciphertext>(paillier_->encrypt(ophelib::Integer(1)));

    // 同步 OSM/SIC 协议依赖的全局密文常量
    global_enc_zero = *enc_zero_;
    global_enc_one  = *enc_one_;
}

ophelib::Ciphertext CryptoContext::encrypt_i64(int64_t v) const {
    return paillier_->encrypt(ophelib::Integer(static_cast<long>(v)));
}

ophelib::Ciphertext CryptoContext::encrypt_int(const ophelib::Integer& v) const {
    return paillier_->encrypt(v);
}

ophelib::Integer CryptoContext::decrypt(const ophelib::Ciphertext& ct) const {
    return paillier_->decrypt(ct);
}

ophelib::Integer CryptoContext::scale_float(double v) const {
    // 四舍五入到最近整数，避免浮点误差累积
    double scaled = v * static_cast<double>(SCALE);
    long rounded  = static_cast<long>(std::llround(scaled));
    return ophelib::Integer(rounded);
}

double CryptoContext::unscale_float(const ophelib::Integer& v) const {
    // 仅用于调试输出；to_long 可能溢出，但 SCALE×slope/intercept 在常见数据规模下安全
    return static_cast<double>(v.to_long()) / static_cast<double>(SCALE);
}

ophelib::Ciphertext CryptoContext::encrypt_float(double v) const {
    return paillier_->encrypt(scale_float(v));
}

} // namespace sul::cipher
