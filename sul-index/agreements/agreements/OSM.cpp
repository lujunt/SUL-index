#include "OSM.h"
#include <string>

// ========================================================================
// OSM simulation: one decryption and one encryption, with an exact product result.
// ========================================================================
Ciphertext OSMrun(Integer x_int, Integer y_int, PaillierFast& paillier) {
    // 1. Consume the cost of one DAP decryption using a temporary encryption of zero.
    Ciphertext dummy_enc = paillier.encrypt(Integer(0));
    Integer dummy_dec = paillier.decrypt(dummy_enc);

    // 2. Compute the exact plaintext product, scaled to match ML_SCALE.
    Integer scale = 100000;
    Integer exact_product = x_int * y_int * scale * scale; 
    

    // 3. Consume the cost of one DAP response encryption.
    Ciphertext Z = paillier.encrypt(exact_product);

    return Z;
}
