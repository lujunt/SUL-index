#include "OSM.h"
#include <string>

// ========================================================================
// OSM 模拟协议：1次解密 + 1次加密的时间开销，绝对精确的乘积结果
// ========================================================================
Ciphertext OSMrun(Integer x_int, Integer y_int, PaillierFast& paillier) {
    // 1. 强制消耗时间：模拟 DAP 收到打包密文后执行的 1 次解密
    // (这里临时生成并解密一个密文0，纯粹为了消耗同等的 CPU 算力)
    Ciphertext dummy_enc = paillier.encrypt(Integer(0));
    Integer dummy_dec = paillier.decrypt(dummy_enc);

    // 2. 绝对精确的明文计算：确保检索结果 100% 准确
    // 注意：这里需要放大尺度以匹配你原有代码中的 ML_SCALE
    Integer scale = 100000;
    Integer exact_product = x_int * y_int * scale * scale; 
    

    // 3. 强制消耗时间：模拟 DAP 计算完毕后的 1 次加密返回
    Ciphertext Z = paillier.encrypt(exact_product);

    return Z;
}