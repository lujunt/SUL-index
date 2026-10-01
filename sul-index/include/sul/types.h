#pragma once

#include <cstdint>
#include <vector>

namespace sul {

// Maximum supported dimensions (the current implementation covers 2 to 6).
constexpr int32_t MAX_DIMS = 6;

// Maximum ART key size: 6 dimensions x 2 bytes = 12 bytes, plus 4 bytes of headroom.
constexpr int32_t MAX_KEY_BYTES = 16;

// Fixed 16-bit precision per dimension: 2^16 = 65,536 quantization levels.
// Actual key size = BITS_PER_DIM * dim_count / 8.
// Upper bounds: 2D -> 4 bytes/layers, 4D -> 8, and 6D -> 12.
constexpr int32_t BITS_PER_DIM = 16;

struct DataPoint {
    int32_t dimensions[MAX_DIMS]; // Integer coordinates scaled from [0, 1) values.
    int32_t dim_count;
    int32_t orig_id;
    __uint128_t z_value;          // 128-bit Z-order value; 6D occupies 96 bits at 16 bits/dimension.
    uint8_t key_bytes[MAX_KEY_BYTES]; // Big-endian ART key; active length is 2 * dim_count.
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
    std::vector<DataPoint*> duplicates; // Distinct records at the same coordinates; range queries return all.
};

// Capacity 4; a uint8 bitmap marks valid slots.
struct ARTNode4 {
    ARTNodeHeader header;
    uint8_t keys[4];
    uint8_t bitmap;
    void* children[4];
};

// Capacity 16; a uint16 bitmap marks valid slots.
struct ARTNode16 {
    ARTNodeHeader header;
    uint8_t keys[16];
    uint16_t bitmap;
    void* children[16];
};

// Capacity 48; a uint64 bitmap marks valid slots.
struct ARTNode48 {
    ARTNodeHeader header;
    uint8_t keys[48];
    uint64_t bitmap;
    void* children[48];
};

// Capacity 256; four uint64 values provide the 256-bit bitmap.
struct ARTNode256 {
    ARTNodeHeader header;
    uint8_t keys[256];
    uint64_t bitmap[4];
    void* children[256];
};

struct GPLInnerNode {
    __uint128_t key;          // First Z-order value in the segment.
    double slope;
    double intercept;
    int32_t child_start;
    int32_t child_count;
};

struct GPLLeafNode {
    __uint128_t key;          // First Z-order value in the segment.
    double slope;
    double intercept;
    std::vector<DataPoint*> data_slots; // Learning-layer slots sized max(2*seg_len + 2*epsilon, 8).
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
    // Actual ART key size: BITS_PER_DIM * dim_count / 8 bytes.
    int32_t key_len() const { return BITS_PER_DIM * dim_count / 8; }
};

}
