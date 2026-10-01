#include "SPI.h"
#include "SIC.h"

#include <ophelib/paillier_fast.h>
#include <thread>

using namespace ophelib;

// SPI implementation using SIC per dimension. cmp_low and cmp_high run in parallel within
// a dimension; dimensions remain sequential to preserve early exit on the first failure.
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
