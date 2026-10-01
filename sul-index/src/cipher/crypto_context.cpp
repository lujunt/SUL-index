#include "sul/cipher/crypto_context.h"

#include <cmath>
#include <cstdio>

// Global encrypted zero and one used by OSM.cpp and SIC.cpp; CryptoContext initializes them.
ophelib::Ciphertext global_enc_zero;
ophelib::Ciphertext global_enc_one;

namespace sul::cipher {

CryptoContext::CryptoContext(int key_size) {
    paillier_ = std::make_unique<ophelib::PaillierFast>(key_size);
    paillier_->generate_keys();

    enc_zero_ = std::make_unique<ophelib::Ciphertext>(paillier_->encrypt(ophelib::Integer(0)));
    enc_one_  = std::make_unique<ophelib::Ciphertext>(paillier_->encrypt(ophelib::Integer(1)));

    // Synchronize the global ciphertext constants required by OSM and SIC.
    global_enc_zero = *enc_zero_;
    global_enc_one  = *enc_one_;
}

CryptoContext::CryptoContext(int /*key_size*/, const ophelib::KeyPair& kp) {
    // PaillierFast(KeyPair) parses public/private keys and performs precomputation.
    paillier_ = std::make_unique<ophelib::PaillierFast>(kp);

    enc_zero_ = std::make_unique<ophelib::Ciphertext>(paillier_->encrypt(ophelib::Integer(0)));
    enc_one_  = std::make_unique<ophelib::Ciphertext>(paillier_->encrypt(ophelib::Integer(1)));

    global_enc_zero = *enc_zero_;
    global_enc_one  = *enc_one_;
}

ophelib::Ciphertext CryptoContext::encrypt_i64(int64_t v) const {
    return paillier_->encrypt(ophelib::Integer(static_cast<long>(v)));
}

ophelib::Integer CryptoContext::u128_to_integer(__uint128_t v) {
    // A 64-bit word occupies at most 16 hexadecimal digits.
    if (v == 0) return ophelib::Integer(0);
    uint64_t hi = static_cast<uint64_t>(v >> 64);
    uint64_t lo = static_cast<uint64_t>(v);
    char buf[40];
    if (hi == 0) {
        std::snprintf(buf, sizeof(buf), "%lx", static_cast<unsigned long>(lo));
    } else {
        std::snprintf(buf, sizeof(buf), "%lx%016lx",
                      static_cast<unsigned long>(hi),
                      static_cast<unsigned long>(lo));
    }
    return ophelib::Integer(buf, 16);
}

ophelib::Ciphertext CryptoContext::encrypt_u128(__uint128_t v) const {
    return paillier_->encrypt(u128_to_integer(v));
}

ophelib::Ciphertext CryptoContext::encrypt_int(const ophelib::Integer& v) const {
    return paillier_->encrypt(v);
}

ophelib::Integer CryptoContext::decrypt(const ophelib::Ciphertext& ct) const {
    return paillier_->decrypt(ct);
}

ophelib::Integer CryptoContext::scale_float(double v) const {
    // Round to the nearest integer to avoid accumulated floating-point error.
    double scaled = v * static_cast<double>(SCALE);
    long rounded  = static_cast<long>(std::llround(scaled));
    return ophelib::Integer(rounded);
}

double CryptoContext::unscale_float(const ophelib::Integer& v) const {
    // Debug output only. to_long may overflow, but scaled model parameters fit typical datasets.
    return static_cast<double>(v.to_long()) / static_cast<double>(SCALE);
}

ophelib::Ciphertext CryptoContext::encrypt_float(double v) const {
    return paillier_->encrypt(scale_float(v));
}

} // namespace sul::cipher
