#pragma once

#include "sul/types.h"

#include <ophelib/paillier_fast.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace sul::cipher {

using ophelib::Ciphertext;

// ============================================================================
// 加密数据点：所有字段（坐标、z 值、key 字节）均为 Paillier 密文
// ============================================================================
struct EncDataPoint {
    std::vector<Ciphertext> dimensions;   // 加密坐标，size = dim_count
    Ciphertext               z_value;      // 加密 z 曲线值
    std::vector<Ciphertext> key_bytes;    // 加密 key 字节，size = key_len
    int32_t                  dim_count = 0;
    int32_t                  orig_id   = -1; // 仅供日志/调试，不参与协议
};

// ============================================================================
// 加密 GPL 内部节点：slope/intercept/key/child_ids 均加密；
// child_start/child_count 为 DSP 内部明文结构索引（不暴露给查询协议）
// ============================================================================
struct EncGPLInnerNode {
    Ciphertext key;
    Ciphertext slope;           // 加密的 SCALE×slope 整数化值
    Ciphertext intercept;       // 加密的 SCALE×intercept 整数化值
    int32_t    child_start = 0;
    int32_t    child_count = 0;
    std::vector<Ciphertext> child_ids; // 加密子节点 ID（0..child_count-1）
};

// ============================================================================
// 加密 GPL 叶子节点
// 注：bitmap (occupied) 维持明文，便于 DSP 快速判断槽位有效性（论文设定一致）
// ============================================================================
struct EncGPLLeafNode {
    Ciphertext key;
    Ciphertext slope;
    Ciphertext intercept;
    std::vector<EncDataPoint*> data_slots; // size = slot_count
    std::vector<uint8_t>       occupied;
    int32_t slot_count    = 0;
    int32_t filled_count  = 0;
    int32_t art_tree_idx  = -1;
    // ciphertext coord bbox：含本叶子所有 GPL 槽位 + ART 子树点
    // 用于范围查询中间叶子 OUTSIDE 剪枝（通过 SIC 完成比较，不向 DSP 泄露 coord 范围）
    // 大小 = config_.dim_count
    std::vector<Ciphertext> coord_lo_enc;
    std::vector<Ciphertext> coord_hi_enc;
};

// ============================================================================
// 加密 ART 节点：keys[] 与 child_ids[] 均加密
// 用与明文版一致的 ARTNodeType 区分容量等级
// ============================================================================
struct EncARTNodeHeader { ARTNodeType type; };

struct EncARTLeaf {
    EncARTNodeHeader header;
    EncDataPoint*    data_point;
    std::vector<EncDataPoint*> duplicates;
};

// 注：plain_keys 是 DAP 私有视图（与 keys[] 一一对应），
//    DSP 在论文设定下不持有；本实现将两侧合并为单进程，故直接作为字段保存
struct EncARTNode4 {
    EncARTNodeHeader header;
    std::vector<Ciphertext> keys;
    std::vector<uint8_t>    plain_keys; // size = 4, DAP 视图
    uint8_t  bitmap;
    void*    children[4];
    std::vector<Ciphertext> child_ids;
};

struct EncARTNode16 {
    EncARTNodeHeader header;
    std::vector<Ciphertext> keys;
    std::vector<uint8_t>    plain_keys; // size = 16
    uint16_t bitmap;
    void*    children[16];
    std::vector<Ciphertext> child_ids;
};

struct EncARTNode48 {
    EncARTNodeHeader header;
    std::vector<Ciphertext> keys;
    std::vector<uint8_t>    plain_keys; // size = 48
    uint64_t bitmap;
    void*    children[48];
    std::vector<Ciphertext> child_ids;
};

struct EncARTNode256 {
    EncARTNodeHeader header;
    std::vector<Ciphertext> keys;
    std::vector<uint8_t>    plain_keys; // size = 256
    uint64_t bitmap[4];
    void*    children[256];
    std::vector<Ciphertext> child_ids;
};

} // namespace sul::cipher
