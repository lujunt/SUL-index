#ifndef SIC_H
#define SIC_H

#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <random>
#include <gmp.h>
#include <gmpxx.h>
#include <string.h>
#include <ophelib/paillier_fast.h>
// #include "../PGM-index/include/pgm/pgm_index.hpp"
// #include "../main.cpp"
// #include "SM.h"
using namespace ophelib;
using namespace std;
// Random function F producing zero or one.

extern ophelib::Ciphertext global_enc_zero;
extern ophelib::Ciphertext global_enc_one;
int F();

// DAP worker.
void SICDAPFunction(Ciphertext&, Ciphertext, PaillierFast& );

// DSP worker.
void SICDSPFunction(Ciphertext , Ciphertext , Ciphertext&, PaillierFast&);

// SIC protocol entry point.
//string SICrun(string , string, Paillier&);
Integer SICrun(Ciphertext, Ciphertext, PaillierFast&);

Integer SICrun_1(Ciphertext, Ciphertext, PaillierFast&);
#endif // SIC_H
