#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <random>
#include <string.h>
#include "SIC.h"
#include <gmp.h>
#include <string>
#include <gmpxx.h>
// #include "SM.h"
using namespace ophelib;
using namespace std;

// Random function F producing zero or one.
int F() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 1);
    return dis(gen);
}

// DAP function accepting Z and returning R.
void SICDAPFunction(Ciphertext& R, Ciphertext Z, PaillierFast& paillier) {//z->string
    Integer H, res(1), tmp(0), denc_Z;
    Ciphertext enc0, enc1;
    PublicKey pub = paillier.get_pub();
    H = pub.n / 2;
    enc0 = global_enc_zero;
    enc1 = global_enc_one;
    denc_Z = paillier.decrypt(Z);
    if(denc_Z < 0) denc_Z += pub.n;
    size_t a,b;
    a =  mpz_sizeinbase(denc_Z.get_mpz_t(), 2);
    b = mpz_sizeinbase(pub.n.get_mpz_t(), 2);
    // cout<<endl<<"a: "<<a<<" b: "<<b<<endl;
    if(a > b / 2) R = enc1;
    else R = enc0;
    // if(denc_Z < 0) R = enc1;
    // else R = enc0;
}

// DSP function.
void SICDSPFunction(Ciphertext X, Ciphertext Y, Ciphertext& final_result, PaillierFast& paillier) {
    // X *= 2;
    // Y *= 2;
    //(n+a)*2%n = 2n+2a; 
    PublicKey pub = paillier.get_pub();
    Integer tmp(2), res, one(1);
    Ciphertext enc_2, enc_1, Z, R;
    // enc_1 = paillier.encrypt(one);
    // enc_2 = paillier.encrypt(tmp);
    enc_1 = global_enc_one;

    X = X * tmp;
    Y = Y * tmp;
    Y = Y + enc_1;
    int f = F();//no
    // f = 0;
    if(f == 1)
    {
        Z = X - Y;
    }
    else 
    {
        Z = Y - X;
    }
    // The original thread was joined immediately, so call SICDAPFunction synchronously.
    SICDAPFunction(R, Z, paillier);
    if (f == 1) {
        final_result = R;
    } else {
        final_result = global_enc_one - R;
    }
}


Integer SICrun(Ciphertext X, Ciphertext Y, PaillierFast& paillier) {
    // Likewise, call SICDSPFunction synchronously; spawn/join overhead is material for SIC.
    Ciphertext final_result;
    SICDSPFunction(X, Y, final_result, paillier);
    return paillier.decrypt(final_result);
}

Integer SICrun_1(Ciphertext X, Ciphertext Y, PaillierFast& paillier)
{
    PublicKey pub = paillier.get_pub();
    Integer x, y, n(pub.n);
    x = paillier.decrypt(X);
    y = paillier.decrypt(Y);
    if(x > n / 2) x -= n;
    if(y > n / 2) y -= n;
    if(x <= y) return 1;
    else return 0;
}

// int main()
// {
//     PaillierFast paillier(1024);
//     paillier.generate_keys();
//     Integer a(1), b(-1), c(2), d(-2), e(0);
//     Ciphertext A, B, C, D, E;
//     A = paillier.encrypt(a);
//     B = paillier.encrypt(b);
//     C = paillier.encrypt(c);
//     D = paillier.encrypt(d);
//     E = paillier.encrypt(e);
//     cout<<"0: "<<SICrun(A, B, paillier)<<endl;
//     cout<<"1: "<<SICrun(B, A, paillier)<<endl;
//     cout<<"1: "<<SICrun(A, C, paillier)<<endl;
//     cout<<"1: "<<SICrun(D, B, paillier)<<endl;
//     cout<<"0: "<<SICrun(A, E, paillier)<<endl;
//     cout<<"1: "<<SICrun(B, E, paillier)<<endl;
//     cout<<"1: "<<SICrun(E, C, paillier)<<endl;
//     cout<<"0: "<<SICrun(E, D, paillier)<<endl;
// }
