#pragma once

#include "sul/types.h"

#include <ophelib/paillier_fast.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace sul::cipher {

using ophelib::Ciphertext;

// ============================================================================
// Encrypted data point: coordinates, Z-order value, and key bytes are Paillier ciphertexts.
// ============================================================================
struct EncDataPoint {
    std::vector<Ciphertext> dimensions;   // Encrypted coordinates; size = dim_count.
    Ciphertext               z_value;      // Encrypted Z-order value.
    std::vector<Ciphertext> key_bytes;    // Encrypted key bytes; size = key_len.
    int32_t                  dim_count = 0;
    int32_t                  orig_id   = -1; // Logging/debugging only; not part of the protocol.
};

// ============================================================================
// Encrypted GPL inner node. slope, intercept, key, and child_ids are encrypted;
// child_start and child_count are DSP-private structural indexes.
// ============================================================================
struct EncGPLInnerNode {
    Ciphertext key;
    Ciphertext slope;           // Encrypted integer SCALE * slope.
    Ciphertext intercept;       // Encrypted integer SCALE * intercept.
    int32_t    child_start = 0;
    int32_t    child_count = 0;
    std::vector<Ciphertext> child_ids; // Encrypted child IDs from 0 to child_count - 1.
};

// ============================================================================
// Encrypted GPL leaf. The occupied bitmap remains plaintext so the DSP can test slots
// efficiently, matching the paper's model.
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
    // Encrypted coordinate bounding box over GPL slots and ART points in this leaf.
    // SIC uses it to prune OUTSIDE middle leaves without exposing ranges to the DSP.
    // Size = config_.dim_count.
    std::vector<Ciphertext> coord_lo_enc;
    std::vector<Ciphertext> coord_hi_enc;
};

// ============================================================================
// Encrypted ART node. keys and child_ids are encrypted; ARTNodeType identifies capacity.
// ============================================================================
struct EncARTNodeHeader { ARTNodeType type; };

struct EncARTLeaf {
    EncARTNodeHeader header;
    EncDataPoint*    data_point;
    std::vector<EncDataPoint*> duplicates;
};

// plain_keys is the DAP-private view corresponding to keys. A DSP would not hold it;
// this single-process implementation stores it directly because both roles are combined.
struct EncARTNode4 {
    EncARTNodeHeader header;
    std::vector<Ciphertext> keys;
    std::vector<uint8_t>    plain_keys; // Size = 4; DAP view.
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
