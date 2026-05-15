#pragma once

#include "sul/types.h"

#include <cstddef>
#include <vector>

namespace sul {

// ART（自适应基数树）：逐字节遍历key，深度=key_len（即2×dim_count）
// 节点扩容链：Node4 → Node16 → Node48 → Node256
class ARTTree {
public:
    // key_len：ART树的层数，等于实际key字节数（BITS_PER_DIM×dim_count/8）
    explicit ARTTree(int32_t key_len);
    ~ARTTree();

    ARTTree(const ARTTree&) = delete;
    ARTTree& operator=(const ARTTree&) = delete;

    void insert(DataPoint* dp);

    // 精确查找：按key_bytes逐层匹配，返回对应叶节点的数据点
    DataPoint* search(const uint8_t* key_bytes) const;

    // 范围查找：收集key_bytes在[low, high]（字典序）区间内的所有数据点
    std::vector<DataPoint*> range_search(const uint8_t* low,
                                         const uint8_t* high) const;

    std::vector<DataPoint*> collect_all() const;

    bool   empty()      const { return root_ == nullptr; }
    size_t node_count() const { return inner_count_; }
    size_t leaf_count() const { return leaf_count_; }
    void*  root()       const { return root_; }

    // 扩容事件计数器：用于观察插入时ART节点的扩容情况
    size_t expand_4_to_16()   const { return expand_4_to_16_; }
    size_t expand_16_to_48()  const { return expand_16_to_48_; }
    size_t expand_48_to_256() const { return expand_48_to_256_; }

private:
    void*   root_;
    size_t  inner_count_;
    size_t  leaf_count_;
    int32_t key_len_; // ART树层数（字节数）= 2×dim_count

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
