#pragma once

#include <cstdint>
#include <vector>

namespace sul {

constexpr int32_t MAX_DIMS = 4;
constexpr int32_t KEY_BYTES = 8;

struct DataPoint {
    int32_t dimensions[MAX_DIMS];
    int32_t dim_count;
    int32_t orig_id;
    uint64_t z_value;
    uint8_t key_bytes[KEY_BYTES];
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
};

struct ARTNode4 {
    ARTNodeHeader header;
    uint8_t keys[4];
    uint8_t bitmap;
    void* children[4];
};

struct ARTNode16 {
    ARTNodeHeader header;
    uint8_t keys[16];
    uint16_t bitmap;
    void* children[16];
};

struct ARTNode48 {
    ARTNodeHeader header;
    uint8_t keys[48];
    uint64_t bitmap;
    void* children[48];
};

struct ARTNode256 {
    ARTNodeHeader header;
    uint8_t keys[256];
    uint64_t bitmap[4];
    void* children[256];
};

struct GPLInnerNode {
    uint64_t key;
    double slope;
    double intercept;
    int32_t child_start;
    int32_t child_count;
};

struct GPLLeafNode {
    uint64_t key;
    double slope;
    double intercept;
    std::vector<DataPoint*> data_slots;
    std::vector<uint8_t> occupied;
    void* art_root;
    int32_t slot_count;
    int32_t filled_count;
    int32_t seg_start;
    int32_t seg_end;
};

struct IndexConfig {
    int32_t error_bound = 64;
    int32_t max_layers = 16;
    int32_t dim_count = 2;
};

}
