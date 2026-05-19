#include "sul/cipher/sul_cipher_index.h"

#include <random>

namespace sul::cipher {

namespace {

EncARTNode4* new_enc_node4(CryptoContext& ctx) {
    auto* n = new EncARTNode4();
    n->header.type = AT_NODE4;
    n->bitmap = 0;
    n->keys.assign(4,        ctx.enc_zero());
    n->child_ids.assign(4,   ctx.enc_zero());
    n->plain_keys.assign(4, 0);
    for (int32_t i = 0; i < 4; ++i) n->children[i] = nullptr;
    return n;
}
EncARTNode16* new_enc_node16(CryptoContext& ctx) {
    auto* n = new EncARTNode16();
    n->header.type = AT_NODE16;
    n->bitmap = 0;
    n->keys.assign(16,      ctx.enc_zero());
    n->child_ids.assign(16, ctx.enc_zero());
    n->plain_keys.assign(16, 0);
    for (int32_t i = 0; i < 16; ++i) n->children[i] = nullptr;
    return n;
}
EncARTNode48* new_enc_node48(CryptoContext& ctx) {
    auto* n = new EncARTNode48();
    n->header.type = AT_NODE48;
    n->bitmap = 0;
    n->keys.assign(48,      ctx.enc_zero());
    n->child_ids.assign(48, ctx.enc_zero());
    n->plain_keys.assign(48, 0);
    for (int32_t i = 0; i < 48; ++i) n->children[i] = nullptr;
    return n;
}
EncARTNode256* new_enc_node256(CryptoContext& ctx) {
    auto* n = new EncARTNode256();
    n->header.type = AT_NODE256;
    for (int32_t i = 0; i < 4; ++i) n->bitmap[i] = 0;
    n->keys.assign(256,      ctx.enc_zero());
    n->child_ids.assign(256, ctx.enc_zero());
    n->plain_keys.assign(256, 0);
    for (int32_t i = 0; i < 256; ++i) n->children[i] = nullptr;
    return n;
}
EncARTLeaf* new_enc_leaf(EncDataPoint* edp) {
    auto* n = new EncARTLeaf();
    n->header.type = AT_LEAF;
    n->data_point  = edp;
    return n;
}

ARTNodeType type_of(void* node) {
    return reinterpret_cast<EncARTNodeHeader*>(node)->type;
}

bool n48_has(const EncARTNode48* n, int32_t i)  { return ((n->bitmap >> i) & 1ULL) != 0ULL; }
void n48_set(EncARTNode48* n, int32_t i)        { n->bitmap |= (1ULL << i); }
bool n256_has(const EncARTNode256* n, int32_t i){ return ((n->bitmap[i>>6] >> (i&63)) & 1ULL) != 0ULL; }
void n256_set(EncARTNode256* n, int32_t i)      { n->bitmap[i>>6] |= (1ULL << (i&63)); }

} // namespace

EncARTTree::EncARTTree(int32_t key_len, CryptoContext& crypto)
    : root_(nullptr), inner_count_(0), leaf_count_(0),
      key_len_(key_len), crypto_(crypto) {}

EncARTTree::~EncARTTree() {
    if (root_) destroy(root_, 0);
}

void EncARTTree::destroy(void* node, int32_t depth) {
    if (!node) return;
    ARTNodeType t = type_of(node);
    if (t == AT_LEAF) { delete reinterpret_cast<EncARTLeaf*>(node); return; }
    if (t == AT_NODE4) {
        auto* n = reinterpret_cast<EncARTNode4*>(node);
        for (int32_t i = 0; i < 4; ++i) if (n->bitmap & (1u<<i)) destroy(n->children[i], depth+1);
        delete n;
    } else if (t == AT_NODE16) {
        auto* n = reinterpret_cast<EncARTNode16*>(node);
        for (int32_t i = 0; i < 16; ++i) if (n->bitmap & (1u<<i)) destroy(n->children[i], depth+1);
        delete n;
    } else if (t == AT_NODE48) {
        auto* n = reinterpret_cast<EncARTNode48*>(node);
        for (int32_t i = 0; i < 48; ++i) if (n48_has(n, i)) destroy(n->children[i], depth+1);
        delete n;
    } else {
        auto* n = reinterpret_cast<EncARTNode256*>(node);
        for (int32_t i = 0; i < 256; ++i) if (n256_has(n, i)) destroy(n->children[i], depth+1);
        delete n;
    }
}

void EncARTTree::insert(EncDataPoint* edp, const uint8_t* plain_key_bytes) {
    if (!root_) {
        root_ = new_enc_node4(crypto_);
        ++inner_count_;
    }
    root_ = insert_inner(root_, edp, plain_key_bytes, 0);
}

void* EncARTTree::insert_inner(void* node, EncDataPoint* edp,
                                const uint8_t* plain_key_bytes, int32_t depth) {
    uint8_t byte_val = plain_key_bytes[depth];
    ARTNodeType t = type_of(node);

    auto attach_or_recurse = [&](void*& child_slot) {
        if (depth == key_len_ - 1) {
            if (child_slot && type_of(child_slot) == AT_LEAF) {
                reinterpret_cast<EncARTLeaf*>(child_slot)->data_point = edp;
            } else {
                if (child_slot) destroy(child_slot, depth + 1);
                child_slot = new_enc_leaf(edp);
                ++leaf_count_;
            }
        } else {
            if (child_slot) {
                child_slot = insert_inner(child_slot, edp, plain_key_bytes, depth + 1);
            } else {
                auto* nc = new_enc_node4(crypto_);
                ++inner_count_;
                child_slot = insert_inner(nc, edp, plain_key_bytes, depth + 1);
            }
        }
    };

    if (t == AT_NODE4) {
        auto* n = reinterpret_cast<EncARTNode4*>(node);
        for (int32_t i = 0; i < 4; ++i)
            if ((n->bitmap & (1u<<i)) && n->plain_keys[i] == byte_val) {
                attach_or_recurse(n->children[i]); return node;
            }
        for (int32_t i = 0; i < 4; ++i)
            if (!(n->bitmap & (1u<<i))) {
                n->plain_keys[i] = byte_val;
                n->keys[i]       = crypto_.encrypt_i64(byte_val);
                n->child_ids[i]  = crypto_.encrypt_i64(i);
                n->bitmap |= static_cast<uint8_t>(1u << i);
                n->children[i] = nullptr;
                attach_or_recurse(n->children[i]);
                return node;
            }
        return insert_inner(expand_node4_to_node16(n), edp, plain_key_bytes, depth);
    }

    if (t == AT_NODE16) {
        auto* n = reinterpret_cast<EncARTNode16*>(node);
        for (int32_t i = 0; i < 16; ++i)
            if ((n->bitmap & (1u<<i)) && n->plain_keys[i] == byte_val) {
                attach_or_recurse(n->children[i]); return node;
            }
        for (int32_t i = 0; i < 16; ++i)
            if (!(n->bitmap & (1u<<i))) {
                n->plain_keys[i] = byte_val;
                n->keys[i]       = crypto_.encrypt_i64(byte_val);
                n->child_ids[i]  = crypto_.encrypt_i64(i);
                n->bitmap |= static_cast<uint16_t>(1u << i);
                n->children[i] = nullptr;
                attach_or_recurse(n->children[i]);
                return node;
            }
        return insert_inner(expand_node16_to_node48(n), edp, plain_key_bytes, depth);
    }

    if (t == AT_NODE48) {
        auto* n = reinterpret_cast<EncARTNode48*>(node);
        for (int32_t i = 0; i < 48; ++i)
            if (n48_has(n, i) && n->plain_keys[i] == byte_val) {
                attach_or_recurse(n->children[i]); return node;
            }
        for (int32_t i = 0; i < 48; ++i)
            if (!n48_has(n, i)) {
                n->plain_keys[i] = byte_val;
                n->keys[i]       = crypto_.encrypt_i64(byte_val);
                n->child_ids[i]  = crypto_.encrypt_i64(i);
                n48_set(n, i);
                n->children[i] = nullptr;
                attach_or_recurse(n->children[i]);
                return node;
            }
        return insert_inner(expand_node48_to_node256(n), edp, plain_key_bytes, depth);
    }

    auto* n = reinterpret_cast<EncARTNode256*>(node);
    for (int32_t i = 0; i < 256; ++i)
        if (n256_has(n, i) && n->plain_keys[i] == byte_val) {
            attach_or_recurse(n->children[i]); return node;
        }
    for (int32_t i = 0; i < 256; ++i)
        if (!n256_has(n, i)) {
            n->plain_keys[i] = byte_val;
            n->keys[i]       = crypto_.encrypt_i64(byte_val);
            n->child_ids[i]  = crypto_.encrypt_i64(i);
            n256_set(n, i);
            n->children[i] = nullptr;
            attach_or_recurse(n->children[i]);
            return node;
        }
    return node;
}

void* EncARTTree::expand_node4_to_node16(EncARTNode4* n4) {
    auto* n16 = new_enc_node16(crypto_);
    int32_t j = 0;
    for (int32_t i = 0; i < 4; ++i) {
        if (n4->bitmap & (1u << i)) {
            n16->plain_keys[j] = n4->plain_keys[i];
            n16->keys[j]       = n4->keys[i];
            n16->child_ids[j]  = crypto_.encrypt_i64(j);
            n16->children[j]   = n4->children[i];
            n16->bitmap |= static_cast<uint16_t>(1u << j);
            ++j;
        }
    }
    bool was_root = (root_ == n4);
    delete n4;
    if (was_root) root_ = n16;
    return n16;
}

void* EncARTTree::expand_node16_to_node48(EncARTNode16* n16) {
    auto* n48 = new_enc_node48(crypto_);
    int32_t j = 0;
    for (int32_t i = 0; i < 16; ++i) {
        if (n16->bitmap & (1u << i)) {
            n48->plain_keys[j] = n16->plain_keys[i];
            n48->keys[j]       = n16->keys[i];
            n48->child_ids[j]  = crypto_.encrypt_i64(j);
            n48->children[j]   = n16->children[i];
            n48_set(n48, j);
            ++j;
        }
    }
    bool was_root = (root_ == n16);
    delete n16;
    if (was_root) root_ = n48;
    return n48;
}

void* EncARTTree::expand_node48_to_node256(EncARTNode48* n48) {
    auto* n256 = new_enc_node256(crypto_);
    int32_t j = 0;
    for (int32_t i = 0; i < 48; ++i) {
        if (n48_has(n48, i)) {
            n256->plain_keys[j] = n48->plain_keys[i];
            n256->keys[j]       = n48->keys[i];
            n256->child_ids[j]  = crypto_.encrypt_i64(j);
            n256->children[j]   = n48->children[i];
            n256_set(n256, j);
            ++j;
        }
    }
    bool was_root = (root_ == n48);
    delete n48;
    if (was_root) root_ = n256;
    return n256;
}

// ============================================================================
// SARTQ：安全 ART 点查询（plan §16.2）
// 每层流程：
//   DSP：标记向量 marker[i] = (enc_keys[i] - enc_k) * 同一随机噪声
//        子树 ID 也加同一噪声（密文加噪不影响"零位置识别"）
//   DAP：解密标记向量，零位置即目标子节点；若全部非零则返回 null
// 简化：DAP 直接由 paillier 解密
// ============================================================================
EncDataPoint* EncARTTree::search(const std::vector<Ciphertext>& enc_key_bytes,
                                  const uint8_t* /*plain_key_bytes*/,
                                  ophelib::PaillierFast& paillier) {
    void* cur = root_;
    if (!cur) return nullptr;

    static std::mt19937_64 rng(std::random_device{}());

    for (int32_t depth = 0; depth < key_len_ && cur; ++depth) {
        ARTNodeType t = type_of(cur);
        if (t == AT_LEAF) return reinterpret_cast<EncARTLeaf*>(cur)->data_point;

        const Ciphertext& enc_k = enc_key_bytes[depth];

        std::vector<int32_t>           active_slots;
        std::vector<const Ciphertext*> active_keys;
        void** active_children = nullptr;

        if (t == AT_NODE4) {
            auto* n = reinterpret_cast<EncARTNode4*>(cur);
            for (int32_t i = 0; i < 4; ++i)
                if (n->bitmap & (1u<<i)) { active_slots.push_back(i); active_keys.push_back(&n->keys[i]); }
            active_children = n->children;
        } else if (t == AT_NODE16) {
            auto* n = reinterpret_cast<EncARTNode16*>(cur);
            for (int32_t i = 0; i < 16; ++i)
                if (n->bitmap & (1u<<i)) { active_slots.push_back(i); active_keys.push_back(&n->keys[i]); }
            active_children = n->children;
        } else if (t == AT_NODE48) {
            auto* n = reinterpret_cast<EncARTNode48*>(cur);
            for (int32_t i = 0; i < 48; ++i)
                if (n48_has(n, i)) { active_slots.push_back(i); active_keys.push_back(&n->keys[i]); }
            active_children = n->children;
        } else {
            auto* n = reinterpret_cast<EncARTNode256*>(cur);
            for (int32_t i = 0; i < 256; ++i)
                if (n256_has(n, i)) { active_slots.push_back(i); active_keys.push_back(&n->keys[i]); }
            active_children = n->children;
        }

        if (active_slots.empty()) return nullptr;

        // DSP: 标记向量 + 同一随机噪声
        long noise_long = static_cast<long>((rng() >> 1) % 1000003 + 1);
        ophelib::Integer noise(noise_long);

        // DAP: 解密 marker，找到 0 位置
        int32_t match_idx = -1;
        for (size_t i = 0; i < active_keys.size(); ++i) {
            Ciphertext marker = (*active_keys[i] - enc_k) * noise;
            ophelib::Integer m = paillier.decrypt(marker);
            if (m == ophelib::Integer(0)) { match_idx = static_cast<int32_t>(i); break; }
        }

        if (match_idx < 0) return nullptr;
        cur = active_children[active_slots[match_idx]];
    }

    if (cur && type_of(cur) == AT_LEAF)
        return reinterpret_cast<EncARTLeaf*>(cur)->data_point;
    return nullptr;
}

void EncARTTree::collect_subtree(void* node, int32_t depth,
                                  std::vector<EncDataPoint*>& out) const {
    if (!node) return;
    ARTNodeType t = type_of(node);
    if (t == AT_LEAF) { out.push_back(reinterpret_cast<EncARTLeaf*>(node)->data_point); return; }
    if (t == AT_NODE4) {
        auto* n = reinterpret_cast<const EncARTNode4*>(node);
        for (int32_t i = 0; i < 4; ++i)   if (n->bitmap & (1u<<i)) collect_subtree(n->children[i], depth+1, out);
    } else if (t == AT_NODE16) {
        auto* n = reinterpret_cast<const EncARTNode16*>(node);
        for (int32_t i = 0; i < 16; ++i)  if (n->bitmap & (1u<<i)) collect_subtree(n->children[i], depth+1, out);
    } else if (t == AT_NODE48) {
        auto* n = reinterpret_cast<const EncARTNode48*>(node);
        for (int32_t i = 0; i < 48; ++i)  if (n48_has(n, i)) collect_subtree(n->children[i], depth+1, out);
    } else {
        auto* n = reinterpret_cast<const EncARTNode256*>(node);
        for (int32_t i = 0; i < 256; ++i) if (n256_has(n, i)) collect_subtree(n->children[i], depth+1, out);
    }
}

std::vector<EncDataPoint*> EncARTTree::collect_all() const {
    std::vector<EncDataPoint*> out;
    collect_subtree(root_, 0, out);
    return out;
}

// 范围收集（明文层面遍历 plain_keys，调用方负责 SPI 维度过滤）
void EncARTTree::range_collect(void* node, int32_t depth,
                                const uint8_t* low, const uint8_t* high,
                                bool tight_low, bool tight_high,
                                std::vector<EncDataPoint*>& out) const {
    if (!node) return;
    ARTNodeType t = type_of(node);
    if (t == AT_LEAF) { out.push_back(reinterpret_cast<EncARTLeaf*>(node)->data_point); return; }

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
        auto* n = reinterpret_cast<const EncARTNode4*>(node);
        for (int32_t i = 0; i < 4; ++i)
            if (n->bitmap & (1u<<i)) walk(n->plain_keys[i], n->children[i]);
    } else if (t == AT_NODE16) {
        auto* n = reinterpret_cast<const EncARTNode16*>(node);
        for (int32_t i = 0; i < 16; ++i)
            if (n->bitmap & (1u<<i)) walk(n->plain_keys[i], n->children[i]);
    } else if (t == AT_NODE48) {
        auto* n = reinterpret_cast<const EncARTNode48*>(node);
        for (int32_t i = 0; i < 48; ++i)
            if (n48_has(n, i)) walk(n->plain_keys[i], n->children[i]);
    } else {
        auto* n = reinterpret_cast<const EncARTNode256*>(node);
        for (int32_t i = 0; i < 256; ++i)
            if (n256_has(n, i)) walk(n->plain_keys[i], n->children[i]);
    }
}

std::vector<EncDataPoint*> EncARTTree::range_search(const uint8_t* low,
                                                     const uint8_t* high) const {
    std::vector<EncDataPoint*> out;
    range_collect(root_, 0, low, high, true, true, out);
    return out;
}

} // namespace sul::cipher
