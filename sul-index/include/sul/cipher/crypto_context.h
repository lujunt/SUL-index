#pragma once

#include <ophelib/paillier_fast.h>

#include <cstdint>
#include <memory>

namespace sul::cipher {

// CryptoContext owns Paillier keys and floating-point/integer scaling.
// A single PaillierFast instance is shared by builders and queries. SCALE matches
// agreements/OSM.cpp (100000) and converts slope/intercept values to integers.
class CryptoContext {
public:
    static constexpr int32_t SCALE = 100000;

    explicit CryptoContext(int key_size = 1024);

    // Construct from an existing Paillier key pair during deserialization.
    CryptoContext(int key_size, const ophelib::KeyPair& kp);

    ophelib::PaillierFast& paillier() { return *paillier_; }
    const ophelib::PaillierFast& paillier() const { return *paillier_; }

    // Integer/ciphertext conversion.
    ophelib::Ciphertext encrypt_i64(int64_t v) const;
    ophelib::Ciphertext encrypt_u128(__uint128_t v) const;  // Unsigned 128-bit Z-order value.
    ophelib::Ciphertext encrypt_int(const ophelib::Integer& v) const;
    ophelib::Integer    decrypt(const ophelib::Ciphertext& ct) const;

    // Convert __uint128_t to ophelib::Integer through a hexadecimal string.
    static ophelib::Integer u128_to_integer(__uint128_t v);

    // Floating-point/scaled-integer conversion for slope and intercept encoding.
    ophelib::Integer scale_float(double v) const;
    double           unscale_float(const ophelib::Integer& v) const;

    // Encrypt a scaled slope or intercept.
    ophelib::Ciphertext encrypt_float(double v) const;

    // Global encrypted zero/one constants required by OSM and SIC.
    const ophelib::Ciphertext& enc_zero() const { return *enc_zero_; }
    const ophelib::Ciphertext& enc_one()  const { return *enc_one_;  }

private:
    std::unique_ptr<ophelib::PaillierFast> paillier_;
    std::unique_ptr<ophelib::Ciphertext>   enc_zero_;
    std::unique_ptr<ophelib::Ciphertext>   enc_one_;
};

} // namespace sul::cipher
