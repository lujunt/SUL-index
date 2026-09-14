#include "sul/art_tree.h"

#include <cstring>

namespace sul {

namespace {

ARTNode4* new_node4() {
    auto* n = new ARTNode4();
    n->header.type = AT_NODE4;
    std::memset(n->keys, 0, sizeof(n->keys));
    n->bitmap = 0;
    for (int32_t i = 0; i < 4; ++i) n->children[i] = nullptr;
    return n;
}

ARTNode16* new_node16() {
    auto* n = new ARTNode16();
    n->header.type = AT_NODE16;
    std::memset(n->keys, 0, sizeof(n->keys));
    n->bitmap = 0;
    for (int32_t i = 0; i < 16; ++i) n->children[i] = nullptr;
    return n;
}

ARTNode48* new_node48() {
    auto* n = new ARTNode48();
    n->header.type = AT_NODE48;
    std::memset(n->keys, 0, sizeof(n->keys));
    n->bitmap = 0;
    for (int32_t i = 0; i < 48; ++i) n->children[i] = nullptr;
    return n;
}

ARTNode256* new_node256() {
    auto* n = new ARTNode256();
    n->header.type = AT_NODE256;
    std::memset(n->keys, 0, sizeof(n->keys));
    for (int32_t i = 0; i < 4; ++i) n->bitmap[i] = 0;
    for (int32_t i = 0; i < 256; ++i) n->children[i] = nullptr;
    return n;
}

ARTLeafNode* new_leaf(DataPoint* dp) {
    auto* n = new ARTLeafNode();
    n->header.type = AT_LEAF;
    n->data_point = dp;
    return n;
}

ARTNodeType type_of(void* node) {
    return reinterpret_cast<ARTNodeHeader*>(node)->type;
}

// Node48的位图操作（48位用uint64_t存储）
bool n48_has(const ARTNode48* n, int32_t i) {
    return ((n->bitmap >> i) & 1ULL) != 0ULL;
}
void n48_set(ARTNode48* n, int32_t i) {
    n->bitmap |= (1ULL << i);
}

// Node256的位图操作（256位用uint64_t[4]存储，每个元素管理64位）
bool n256_has(const ARTNode256* n, int32_t i) {
    return ((n->bitmap[i >> 6] >> (i & 63)) & 1ULL) != 0ULL;
}
void n256_set(ARTNode256* n, int32_t i) {
    n->bitmap[i >> 6] |= (1ULL << (i & 63));
}

int32_t find_slot_node4(const ARTNode4* n, uint8_t byte_val) {
    for (int32_t i = 0; i < 4; ++i)
        if ((n->bitmap & (1u << i)) && n->keys[i] == byte_val) return i;
    return -1;
}
int32_t find_empty_node4(const ARTNode4* n) {
    for (int32_t i = 0; i < 4; ++i) if (!(n->bitmap & (1u << i))) return i;
    return -1;
}

int32_t find_slot_node16(const ARTNode16* n, uint8_t byte_val) {
    for (int32_t i = 0; i < 16; ++i)
        if ((n->bitmap & (1u << i)) && n->keys[i] == byte_val) return i;
    return -1;
}
int32_t find_empty_node16(const ARTNode16* n) {
    for (int32_t i = 0; i < 16; ++i) if (!(n->bitmap & (1u << i))) return i;
    return -1;
}

int32_t find_slot_node48(const ARTNode48* n, uint8_t byte_val) {
    for (int32_t i = 0; i < 48; ++i)
        if (n48_has(n, i) && n->keys[i] == byte_val) return i;
    return -1;
}
int32_t find_empty_node48(const ARTNode48* n) {
    for (int32_t i = 0; i < 48; ++i) if (!n48_has(n, i)) return i;
    return -1;
}

int32_t find_slot_node256(const ARTNode256* n, uint8_t byte_val) {
    for (int32_t i = 0; i < 256; ++i)
        if (n256_has(n, i) && n->keys[i] == byte_val) return i;
    return -1;
}
int32_t find_empty_node256(const ARTNode256* n) {
    for (int32_t i = 0; i < 256; ++i) if (!n256_has(n, i)) return i;
    return -1;
}

} // namespace

ARTTree::ARTTree(int32_t key_len)
    : root_(nullptr), inner_count_(0), leaf_count_(0), key_len_(key_len) {}

ARTTree::~ARTTree() {
    if (root_) destroy(root_, 0);
}

void ARTTree::destroy(void* node, int32_t depth) {
    if (!node) return;
    ARTNodeType t = type_of(node);
    if (t == AT_LEAF) {
        delete reinterpret_cast<ARTLeafNode*>(node);
        return;
    }
    if (t == AT_NODE4) {
        auto* n = reinterpret_cast<ARTNode4*>(node);
        for (int32_t i = 0; i < 4; ++i)
            if (n->bitmap & (1u << i)) destroy(n->children[i], depth + 1);
        delete n;
    } else if (t == AT_NODE16) {
        auto* n = reinterpret_cast<ARTNode16*>(node);
        for (int32_t i = 0; i < 16; ++i)
            if (n->bitmap & (1u << i)) destroy(n->children[i], depth + 1);
        delete n;
    } else if (t == AT_NODE48) {
        auto* n = reinterpret_cast<ARTNode48*>(node);
        for (int32_t i = 0; i < 48; ++i)
            if (n48_has(n, i)) destroy(n->children[i], depth + 1);
        delete n;
    } else {
        auto* n = reinterpret_cast<ARTNode256*>(node);
        for (int32_t i = 0; i < 256; ++i)
            if (n256_has(n, i)) destroy(n->children[i], depth + 1);
        delete n;
    }
}

void ARTTree::insert(DataPoint* dp) {
    if (!root_) {
        root_ = new_node4();
        ++inner_count_;
    }
    root_ = insert_inner(root_, dp->key_bytes, dp, 0);
}

void* ARTTree::insert_inner(void* node, const uint8_t* key_bytes,
                             DataPoint* dp, int32_t depth) {
    uint8_t byte_val = key_bytes[depth];
    ARTNodeType t = type_of(node);

    // 到达最后一层（depth == key_len_-1）则挂叶节点，否则向下递归
    auto attach_or_recurse = [&](void*& child_slot) {
        if (depth == key_len_ - 1) {
            if (child_slot && type_of(child_slot) == AT_LEAF) {
                reinterpret_cast<ARTLeafNode*>(child_slot)->duplicates.push_back(dp);
            } else {
                if (child_slot) destroy(child_slot, depth + 1);
                child_slot = new_leaf(dp);
                ++leaf_count_;
            }
        } else {
            if (child_slot) {
                child_slot = insert_inner(child_slot, key_bytes, dp, depth + 1);
            } else {
                auto* nc = new_node4();
                ++inner_count_;
                child_slot = insert_inner(nc, key_bytes, dp, depth + 1);
            }
        }
    };

    if (t == AT_NODE4) {
        auto* n = reinterpret_cast<ARTNode4*>(node);
        int32_t idx = find_slot_node4(n, byte_val);
        if (idx >= 0) { attach_or_recurse(n->children[idx]); return node; }
        int32_t empty = find_empty_node4(n);
        if (empty >= 0) {
            n->keys[empty] = byte_val;
            n->bitmap |= static_cast<uint8_t>(1u << empty);
            n->children[empty] = nullptr;
            attach_or_recurse(n->children[empty]);
            return node;
        }
        // Node4已满，扩容为Node16
        return insert_inner(expand_node4_to_node16(n), key_bytes, dp, depth);
    }

    if (t == AT_NODE16) {
        auto* n = reinterpret_cast<ARTNode16*>(node);
        int32_t idx = find_slot_node16(n, byte_val);
        if (idx >= 0) { attach_or_recurse(n->children[idx]); return node; }
        int32_t empty = find_empty_node16(n);
        if (empty >= 0) {
            n->keys[empty] = byte_val;
            n->bitmap |= static_cast<uint16_t>(1u << empty);
            n->children[empty] = nullptr;
            attach_or_recurse(n->children[empty]);
            return node;
        }
        // Node16已满，扩容为Node48
        return insert_inner(expand_node16_to_node48(n), key_bytes, dp, depth);
    }

    if (t == AT_NODE48) {
        auto* n = reinterpret_cast<ARTNode48*>(node);
        int32_t idx = find_slot_node48(n, byte_val);
        if (idx >= 0) { attach_or_recurse(n->children[idx]); return node; }
        int32_t empty = find_empty_node48(n);
        if (empty >= 0) {
            n->keys[empty] = byte_val;
            n48_set(n, empty);
            n->children[empty] = nullptr;
            attach_or_recurse(n->children[empty]);
            return node;
        }
        // Node48已满，扩容为Node256
        return insert_inner(expand_node48_to_node256(n), key_bytes, dp, depth);
    }

    // AT_NODE256：最大节点，不再扩容
    auto* n = reinterpret_cast<ARTNode256*>(node);
    int32_t idx = find_slot_node256(n, byte_val);
    if (idx >= 0) { attach_or_recurse(n->children[idx]); return node; }
    int32_t empty = find_empty_node256(n);
    if (empty >= 0) {
        n->keys[empty] = byte_val;
        n256_set(n, empty);
        n->children[empty] = nullptr;
        attach_or_recurse(n->children[empty]);
    }
    return node;
}

void* ARTTree::expand_node4_to_node16(ARTNode4* n4) {
    auto* n16 = new_node16();
    int32_t j = 0;
    for (int32_t i = 0; i < 4; ++i) {
        if (n4->bitmap & (1u << i)) {
            n16->keys[j] = n4->keys[i];
            n16->children[j] = n4->children[i];
            n16->bitmap |= static_cast<uint16_t>(1u << j);
            ++j;
        }
    }
    bool was_root = (root_ == n4);
    delete n4;
    if (was_root) root_ = n16;
    ++expand_4_to_16_;
    return n16;
}

void* ARTTree::expand_node16_to_node48(ARTNode16* n16) {
    auto* n48 = new_node48();
    int32_t j = 0;
    for (int32_t i = 0; i < 16; ++i) {
        if (n16->bitmap & (1u << i)) {
            n48->keys[j] = n16->keys[i];
            n48->children[j] = n16->children[i];
            n48_set(n48, j);
            ++j;
        }
    }
    bool was_root = (root_ == n16);
    delete n16;
    if (was_root) root_ = n48;
    ++expand_16_to_48_;
    return n48;
}

void* ARTTree::expand_node48_to_node256(ARTNode48* n48) {
    auto* n256 = new_node256();
    int32_t j = 0;
    for (int32_t i = 0; i < 48; ++i) {
        if (n48_has(n48, i)) {
            n256->keys[j] = n48->keys[i];
            n256->children[j] = n48->children[i];
            n256_set(n256, j);
            ++j;
        }
    }
    bool was_root = (root_ == n48);
    delete n48;
    if (was_root) root_ = n256;
    ++expand_48_to_256_;
    return n256;
}

DataPoint* ARTTree::search(const uint8_t* key_bytes) const {
    void* cur = root_;
    // 逐层按当前字节匹配，遍历key_len_层
    for (int32_t depth = 0; depth < key_len_ && cur; ++depth) {
        uint8_t byte_val = key_bytes[depth];
        ARTNodeType t = type_of(cur);
        if (t == AT_LEAF) return reinterpret_cast<ARTLeafNode*>(cur)->data_point;
        if (t == AT_NODE4) {
            auto* n = reinterpret_cast<const ARTNode4*>(cur);
            int32_t idx = find_slot_node4(n, byte_val);
            if (idx < 0) return nullptr;
            cur = n->children[idx];
        } else if (t == AT_NODE16) {
            auto* n = reinterpret_cast<const ARTNode16*>(cur);
            int32_t idx = find_slot_node16(n, byte_val);
            if (idx < 0) return nullptr;
            cur = n->children[idx];
        } else if (t == AT_NODE48) {
            auto* n = reinterpret_cast<const ARTNode48*>(cur);
            int32_t idx = find_slot_node48(n, byte_val);
            if (idx < 0) return nullptr;
            cur = n->children[idx];
        } else {
            auto* n = reinterpret_cast<const ARTNode256*>(cur);
            int32_t idx = find_slot_node256(n, byte_val);
            if (idx < 0) return nullptr;
            cur = n->children[idx];
        }
    }
    if (cur && type_of(cur) == AT_LEAF)
        return reinterpret_cast<ARTLeafNode*>(cur)->data_point;
    return nullptr;
}

void ARTTree::collect_subtree(void* node, int32_t depth,
                               std::vector<DataPoint*>& out) const {
    if (!node) return;
    ARTNodeType t = type_of(node);
    if (t == AT_LEAF) {
        auto* leaf = reinterpret_cast<ARTLeafNode*>(node);
        out.push_back(leaf->data_point);
        out.insert(out.end(), leaf->duplicates.begin(), leaf->duplicates.end());
        return;
    }
    if (t == AT_NODE4) {
        auto* n = reinterpret_cast<const ARTNode4*>(node);
        for (int32_t i = 0; i < 4; ++i)
            if (n->bitmap & (1u << i)) collect_subtree(n->children[i], depth + 1, out);
    } else if (t == AT_NODE16) {
        auto* n = reinterpret_cast<const ARTNode16*>(node);
        for (int32_t i = 0; i < 16; ++i)
            if (n->bitmap & (1u << i)) collect_subtree(n->children[i], depth + 1, out);
    } else if (t == AT_NODE48) {
        auto* n = reinterpret_cast<const ARTNode48*>(node);
        for (int32_t i = 0; i < 48; ++i)
            if (n48_has(n, i)) collect_subtree(n->children[i], depth + 1, out);
    } else {
        auto* n = reinterpret_cast<const ARTNode256*>(node);
        for (int32_t i = 0; i < 256; ++i)
            if (n256_has(n, i)) collect_subtree(n->children[i], depth + 1, out);
    }
}

std::vector<DataPoint*> ARTTree::collect_all() const {
    std::vector<DataPoint*> out;
    collect_subtree(root_, 0, out);
    return out;
}

// 范围收集：tight_low/tight_high表示当前层是否还在边界约束内
void ARTTree::range_collect(void* node, int32_t depth,
                             const uint8_t* low, const uint8_t* high,
                             bool tight_low, bool tight_high,
                             std::vector<DataPoint*>& out) const {
    if (!node) return;
    ARTNodeType t = type_of(node);
    if (t == AT_LEAF) {
        auto* leaf = reinterpret_cast<ARTLeafNode*>(node);
        out.push_back(leaf->data_point);
        out.insert(out.end(), leaf->duplicates.begin(), leaf->duplicates.end());
        return;
    }

    // 当前层的字节范围：若已不在边界约束内则全范围[0x00, 0xFF]
    uint8_t lb = tight_low  ? low[depth]  : 0x00;
    uint8_t hb = tight_high ? high[depth] : 0xFF;
    if (lb > hb) return;

    auto walk = [&](uint8_t k, void* child) {
        if (k < lb || k > hb) return;
        bool ntl = tight_low  && (k == low[depth]);
        bool nth = tight_high && (k == high[depth]);
        range_collect(child, depth + 1, low, high, ntl, nth, out);
    };

    if (t == AT_NODE4) {
        auto* n = reinterpret_cast<const ARTNode4*>(node);
        for (int32_t i = 0; i < 4; ++i)
            if (n->bitmap & (1u << i)) walk(n->keys[i], n->children[i]);
    } else if (t == AT_NODE16) {
        auto* n = reinterpret_cast<const ARTNode16*>(node);
        for (int32_t i = 0; i < 16; ++i)
            if (n->bitmap & (1u << i)) walk(n->keys[i], n->children[i]);
    } else if (t == AT_NODE48) {
        auto* n = reinterpret_cast<const ARTNode48*>(node);
        for (int32_t i = 0; i < 48; ++i)
            if (n48_has(n, i)) walk(n->keys[i], n->children[i]);
    } else {
        auto* n = reinterpret_cast<const ARTNode256*>(node);
        for (int32_t i = 0; i < 256; ++i)
            if (n256_has(n, i)) walk(n->keys[i], n->children[i]);
    }
}

std::vector<DataPoint*> ARTTree::range_search(const uint8_t* low,
                                               const uint8_t* high) const {
    std::vector<DataPoint*> out;
    range_collect(root_, 0, low, high, true, true, out);
    return out;
}

}
