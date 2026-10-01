#ifndef OSM_H
#define OSM_H

#include <ophelib/paillier_fast.h>
// #include "../PGM-index/include/pgm/pgm_index.hpp"
// #include "../main.cpp"
using namespace ophelib;


extern ophelib::Ciphertext global_enc_zero;
extern ophelib::Ciphertext global_enc_one;
// Use the actual plaintext feature to simulate OSM's theoretical O(1) online cost.
Ciphertext OSMrun(Integer x_int, Integer y_int, PaillierFast& paillier);

#endif // OSM_H
