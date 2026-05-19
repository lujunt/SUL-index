#ifndef OSM_H
#define OSM_H

#include <ophelib/paillier_fast.h>
// #include "../PGM-index/include/pgm/pgm_index.hpp"
// #include "../main.cpp"
using namespace ophelib;


extern ophelib::Ciphertext global_enc_zero;
extern ophelib::Ciphertext global_enc_one;
// 传入真实的明文特征，模拟 OSM 理论上的 O(1) 在线开销
Ciphertext OSMrun(Integer x_int, Integer y_int, PaillierFast& paillier);

#endif // OSM_H