#pragma once

#include "sul/types.h"

#include <cstddef>
#include <vector>

namespace sul {

class ARTTree {
public:
    ARTTree();
    ~ARTTree();

    ARTTree(const ARTTree&) = delete;
    ARTTree& operator=(const ARTTree&) = delete;

    void insert(DataPoint* dp);

    DataPoint* search(const uint8_t key_bytes[KEY_BYTES]) const;

    std::vector<DataPoint*> range_search(const uint8_t low[KEY_BYTES],
                                         const uint8_t high[KEY_BYTES]) const;

    std::vector<DataPoint*> collect_all() const;

    bool empty() const { return root_ == nullptr; }

    size_t node_count() const { return inner_count_; }
    size_t leaf_count() const { return leaf_count_; }

    void* root() const { return root_; }

private:
    void* root_;
    size_t inner_count_;
    size_t leaf_count_;

    void* insert_inner(void* node, const uint8_t key_bytes[KEY_BYTES],
                       DataPoint* dp, int32_t depth);
    void* expand_node4_to_node16(ARTNode4* n4);
    void* expand_node16_to_node48(ARTNode16* n16);
    void* expand_node48_to_node256(ARTNode48* n48);
    void destroy(void* node, int32_t depth);

    void collect_subtree(void* node, int32_t depth,
                         std::vector<DataPoint*>& out) const;

    void range_collect(void* node, int32_t depth,
                       const uint8_t low[KEY_BYTES], const uint8_t high[KEY_BYTES],
                       bool tight_low, bool tight_high,
                       std::vector<DataPoint*>& out) const;
};

}
