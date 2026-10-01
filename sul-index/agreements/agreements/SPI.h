#ifndef SPI_H
#define SPI_H

#include <ophelib/paillier_fast.h>
#include <vector>

using namespace ophelib;

// SPI protocol: determine whether an encrypted point lies in encrypted range Q = [ql, qr].
// SIC checks ql[d] <= coords[d] <= qr[d] in every dimension; return one only if all pass.
//
// Inputs:
//   enc_coords: encrypted point coordinates, size dim_count
//   enc_ql: encrypted lower bounds
//   enc_qr: encrypted upper bounds
//   paillier: Paillier cryptographic context
//
// Output: Integer one for an in-range point, otherwise zero.
//
// The DSP holds ciphertexts and the DAP cooperates during SICrun comparisons.
Integer SPIrun(const std::vector<Ciphertext>& enc_coords,
               const std::vector<Ciphertext>& enc_ql,
               const std::vector<Ciphertext>& enc_qr,
               PaillierFast& paillier);

#endif // SPI_H
