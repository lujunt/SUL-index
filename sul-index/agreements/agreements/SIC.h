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
// 随机函数 F,随机生成0或1

extern ophelib::Ciphertext global_enc_zero;
extern ophelib::Ciphertext global_enc_one;
int F();

// 线程DAP
void SICDAPFunction(Ciphertext&, Ciphertext, PaillierFast& );

// 线程DSP
void SICDSPFunction(Ciphertext , Ciphertext , Ciphertext&, PaillierFast&);

// SICrun 函数
//string SICrun(string , string, Paillier&);
Integer SICrun(Ciphertext, Ciphertext, PaillierFast&);

Integer SICrun_1(Ciphertext, Ciphertext, PaillierFast&);
#endif // SIC_H