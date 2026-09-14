#pragma once

#include <cstdint>
#include <vector>

namespace sul {

// 支持的最大维度数（当前实现覆盖2~6维）
constexpr int32_t MAX_DIMS = 6;

// ART key的最大字节数：6维×2字节/维=12字节，留4字节余量
constexpr int32_t MAX_KEY_BYTES = 16;

// 每维固定16位精度：2^16=65536量化级别，2D下支持百万级数据
// key实际字节数 = BITS_PER_DIM × dim_count / 8
// 2D→4字节→4层ART，4D→8字节→8层，6D→12字节→12层（上界）
constexpr int32_t BITS_PER_DIM = 16;

struct DataPoint {
    int32_t dimensions[MAX_DIMS]; // 各维度整数坐标（由[0,1)浮点缩放而来）
    int32_t dim_count;
    int32_t orig_id;
    __uint128_t z_value;          // z曲线值（128位；BITS_PER_DIM=16 时 6 维=96 位完全保留）
    uint8_t key_bytes[MAX_KEY_BYTES]; // ART key大端序字节表示，实际有效长度=2×dim_count
};

enum ARTNodeType : uint8_t {
    AT_NODE4 = 0,
    AT_NODE16 = 1,
    AT_NODE48 = 2,
    AT_NODE256 = 3,
    AT_LEAF = 4
};

struct ARTNodeHeader {
    ARTNodeType type;
};

struct ARTLeafNode {
    ARTNodeHeader header;
    DataPoint* data_point;
    std::vector<DataPoint*> duplicates; // 同坐标的不同记录，范围查询全部返回
};

// 容量4，bitmap用uint8记录哪些槽位有效
struct ARTNode4 {
    ARTNodeHeader header;
    uint8_t keys[4];
    uint8_t bitmap;
    void* children[4];
};

// 容量16，bitmap用uint16记录有效槽位
struct ARTNode16 {
    ARTNodeHeader header;
    uint8_t keys[16];
    uint16_t bitmap;
    void* children[16];
};

// 容量48，bitmap用uint64记录有效槽位（48位足够）
struct ARTNode48 {
    ARTNodeHeader header;
    uint8_t keys[48];
    uint64_t bitmap;
    void* children[48];
};

// 容量256，bitmap用uint64[4]记录有效槽位（共256位）
struct ARTNode256 {
    ARTNodeHeader header;
    uint8_t keys[256];
    uint64_t bitmap[4];
    void* children[256];
};

struct GPLInnerNode {
    __uint128_t key;          // 段首 z 值（与 DataPoint::z_value 同类型）
    double slope;
    double intercept;
    int32_t child_start;
    int32_t child_count;
};

struct GPLLeafNode {
    __uint128_t key;          // 段首 z 值
    double slope;
    double intercept;
    std::vector<DataPoint*> data_slots; // 学习层槽位，动态大小 max(2×seg_len+2×ε, 8)
    std::vector<uint8_t> occupied;
    void* art_root;
    int32_t slot_count;
    int32_t filled_count;
    int32_t seg_start;
    int32_t seg_end;
};

struct IndexConfig {
    int32_t error_bound = 64;
    int32_t max_layers  = 16;
    int32_t dim_count   = 2;
    // ART key实际字节数：BITS_PER_DIM位/维 × dim_count维 / 8位/字节
    int32_t key_len() const { return BITS_PER_DIM * dim_count / 8; }
};

}
