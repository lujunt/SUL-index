#pragma once

#include "sul/types.h"

#include <cstddef>
#include <vector>

namespace sul {

// Adaptive Radix Tree: traverse keys byte by byte to depth key_len (2 * dim_count).
// Node growth sequence: Node4 -> Node16 -> Node48 -> Node256.
class ARTTree {
public:
    // key_len is the ART depth and the actual key size in bytes.
    explicit ARTTree(int32_t key_len);
    ~ARTTree();

    ARTTree(const ARTTree&) = delete;
    ARTTree& operator=(const ARTTree&) = delete;

    void insert(DataPoint* dp);

    // Exact lookup: match key_bytes at every level and return the leaf data point.
    DataPoint* search(const uint8_t* key_bytes) const;

    // Range lookup: collect points whose key_bytes are in lexicographic [low, high].
    std::vector<DataPoint*> range_search(const uint8_t* low,
                                         const uint8_t* high) const;

    std::vector<DataPoint*> collect_all() const;

    bool   empty()      const { return root_ == nullptr; }
    size_t node_count() const { return inner_count_; }
    size_t leaf_count() const { return leaf_count_; }
    void*  root()       const { return root_; }

    // Counts ART node-growth events during insertion.
    size_t expand_4_to_16()   const { return expand_4_to_16_; }
    size_t expand_16_to_48()  const { return expand_16_to_48_; }
    size_t expand_48_to_256() const { return expand_48_to_256_; }

private:
    void*   root_;
    size_t  inner_count_;
    size_t  leaf_count_;
    int32_t key_len_; // ART depth in bytes = 2 * dim_count.

    size_t expand_4_to_16_   = 0;
    size_t expand_16_to_48_  = 0;
    size_t expand_48_to_256_ = 0;

    void* insert_inner(void* node, const uint8_t* key_bytes,
                       DataPoint* dp, int32_t depth);
    void* expand_node4_to_node16(ARTNode4* n4);
    void* expand_node16_to_node48(ARTNode16* n16);
    void* expand_node48_to_node256(ARTNode48* n48);
    void  destroy(void* node, int32_t depth);

    void collect_subtree(void* node, int32_t depth,
                         std::vector<DataPoint*>& out) const;

    void range_collect(void* node, int32_t depth,
                       const uint8_t* low, const uint8_t* high,
                       bool tight_low, bool tight_high,
                       std::vector<DataPoint*>& out) const;
};

}
