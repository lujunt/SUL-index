// SUL-cipher-index 序列化 / 反序列化
//
// 文件格式（二进制，小端）：
//   Magic        : "SCIDX002"  (8B)   v2: Integer 改为 raw big-endian 字节
//   Version      : u32 = 6（读取兼容 v5；v6 增加 ART 同键记录列表）
//   KeySize      : u32  Paillier 密钥位数
//   SCALE        : i32  浮点缩放常量（应等于 CryptoContext::SCALE）
//   IndexConfig  : error_bound(i32) max_layers(i32) dim_count(i32)
//
//   KeyPair      : key_size_bits(u64) a_bits(u64)
//                  Integer pub.n / pub.g / priv.p / priv.q / priv.a
//   每个 Integer 以 u32 len + 'len' 字节 big-endian 无符号大数保存
//   （len == 0 表示数值 0；GMP 不写前导零，所以 len 通常略小于 ceil(2K/8)）
//
//   Plain Skeleton:
//     n_layers(u32)
//       per layer: n_nodes(u32);
//         per node: key(u64) slope(f64) intercept(f64) child_start(i32) child_count(i32)
//     n_leaves(u32)
//       per leaf: key(u64) slope(f64) intercept(f64)
//                 slot_count(i32) filled(i32) seg_start(i32) seg_end(i32)
//                 occupied(u8 × slot_count)
//
//   Encrypted DataPoint 池（pool_id 顺序：enc_points_ 拼接 enc_inserted_points_）：
//     n_enc_points(u32)
//       per point: dim_count(i32) orig_id(i32)
//                  dim_count 个 Ciphertext
//                  1 个 Ciphertext (z_value)
//                  key_len 个 Ciphertext (key_bytes)
//
//   Encrypted GPL Inner Layers（与 plain 一一对应）:
//     n_layers(u32)
//       per layer: n_nodes(u32)
//         per node: key(ct) slope(ct) intercept(ct) child_count(i32) child_ids[child_count](ct)
//
//   Encrypted GPL Leaf Nodes:
//     n_leaves(u32)
//       per leaf: key(ct) slope(ct) intercept(ct) art_tree_idx(i32)
//                 data_slot_pool_ids: i32 × slot_count （-1 表示空槽）
//
//   Encrypted ART Trees:
//     n_arts(u32)
//       per tree: has_root(u8)
//         if has_root: pre-order walk
//           type(u8)
//           AT_LEAF: pool_id(i32), duplicate_count(u64), duplicate_pool_ids(i32[])
//           AT_NODE*: 4/16/48/256 个槽
//             present(u8); 若 present: plain_key(u8) key(ct) child_id(ct) 然后递归子节点
//
//   每个 Ciphertext 以 u32 len + len 字节 big-endian raw 大数保存（v2）

#include "sul/cipher/sul_cipher_index.h"
#include "sul/cipher/cipher_types.h"

#include <ophelib/paillier_base.h>
#include <ophelib/paillier_fast.h>
#include <ophelib/integer.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace sul::cipher {

namespace {

// ============================================================================
// 二进制 IO 助手
// ============================================================================
template <typename T>
void write_pod(std::ostream& os, const T& v) {
    static_assert(std::is_trivially_copyable_v<T>, "write_pod 仅支持 POD");
    os.write(reinterpret_cast<const char*>(&v), sizeof(T));
}

template <typename T>
T read_pod(std::istream& is) {
    static_assert(std::is_trivially_copyable_v<T>, "read_pod 仅支持 POD");
    T v{};
    is.read(reinterpret_cast<char*>(&v), sizeof(T));
    if (!is) throw std::runtime_error("scidx: 读取 POD 失败");
    return v;
}

// Integer 落盘格式：u32 len + len 字节 big-endian 原始无符号大数
// （len == 0 表示数值 0；GMP 不写前导零，所以 len 通常 < ceil(2K/8)）
// 仅用于 Paillier 密文（恒非负，∈[0, n²)）与公私钥分量，故无符号位需求。
void write_integer(std::ostream& os, const ophelib::Integer& v) {
    const size_t bits = mpz_sizeinbase(v.get_mpz_t(), 2);
    const size_t cap  = (bits + 7) / 8;
    std::vector<uint8_t> buf(cap);
    size_t count = 0;
    if (cap > 0) {
        mpz_export(buf.data(), &count,
                   1 /*order: MSB first*/,
                   1 /*size: 1 byte/word*/,
                   0 /*endian: native (irrelevant for 1-byte words)*/,
                   0 /*nails: 0*/,
                   v.get_mpz_t());
    }
    // value=0 时 mpz_export 返回 count=0 且不写字节
    uint32_t n = static_cast<uint32_t>(count);
    write_pod(os, n);
    if (n > 0) os.write(reinterpret_cast<const char*>(buf.data()), n);
}

ophelib::Integer read_integer(std::istream& is) {
    uint32_t n = read_pod<uint32_t>(is);
    if (n == 0) return ophelib::Integer(0);
    std::vector<uint8_t> buf(n);
    is.read(reinterpret_cast<char*>(buf.data()), n);
    if (!is) throw std::runtime_error("scidx: 读取 Integer 原始字节失败");
    ophelib::Integer v;
    mpz_import(v.get_mpz_t(), n,
               1 /*order: MSB first*/,
               1 /*size: 1 byte/word*/,
               0 /*endian: native*/,
               0 /*nails: 0*/,
               buf.data());
    return v;
}

void write_ct(std::ostream& os, const ophelib::Ciphertext& ct) {
    write_integer(os, ct.data);
}

ophelib::Ciphertext read_ct(std::istream& is,
                            const std::shared_ptr<ophelib::Integer>& n2_shared,
                            const std::shared_ptr<ophelib::FastMod>& fast_mod) {
    ophelib::Integer x = read_integer(is);
    return ophelib::Ciphertext(x, n2_shared, fast_mod);
}

// ART 节点辅助（与 enc_art_tree.cpp 等价）
ARTNodeType type_of(void* node) {
    return reinterpret_cast<EncARTNodeHeader*>(node)->type;
}
bool n48_has(const EncARTNode48* n, int32_t i)  { return ((n->bitmap >> i) & 1ULL) != 0ULL; }
void n48_set(EncARTNode48* n, int32_t i)        { n->bitmap |= (1ULL << i); }
bool n256_has(const EncARTNode256* n, int32_t i){ return ((n->bitmap[i>>6] >> (i&63)) & 1ULL) != 0ULL; }
void n256_set(EncARTNode256* n, int32_t i)      { n->bitmap[i>>6] |= (1ULL << (i&63)); }

// 序列化一棵 ART 子树（前序遍历）
void save_art_subtree(std::ostream& os, void* node,
                      const std::unordered_map<EncDataPoint*, int32_t>& pool_idx) {
    ARTNodeType t = type_of(node);
    write_pod<uint8_t>(os, static_cast<uint8_t>(t));

    if (t == AT_LEAF) {
        auto* n = reinterpret_cast<EncARTLeaf*>(node);
        auto it = pool_idx.find(n->data_point);
        int32_t pid = (it == pool_idx.end()) ? -1 : it->second;
        write_pod<int32_t>(os, pid);
        write_pod<uint64_t>(os, n->duplicates.size());
        for (auto* duplicate : n->duplicates) write_pod<int32_t>(os, pool_idx.at(duplicate));
        return;
    }

    auto write_slot = [&](bool present, uint8_t pk,
                          const ophelib::Ciphertext& key,
                          const ophelib::Ciphertext& cid,
                          void* child) {
        write_pod<uint8_t>(os, present ? 1u : 0u);
        if (!present) return;
        write_pod<uint8_t>(os, pk);
        write_ct(os, key);
        write_ct(os, cid);
        save_art_subtree(os, child, pool_idx);
    };

    if (t == AT_NODE4) {
        auto* n = reinterpret_cast<EncARTNode4*>(node);
        for (int32_t i = 0; i < 4; ++i)
            write_slot((n->bitmap & (1u<<i)) != 0,
                       n->plain_keys[i], n->keys[i], n->child_ids[i], n->children[i]);
    } else if (t == AT_NODE16) {
        auto* n = reinterpret_cast<EncARTNode16*>(node);
        for (int32_t i = 0; i < 16; ++i)
            write_slot((n->bitmap & (1u<<i)) != 0,
                       n->plain_keys[i], n->keys[i], n->child_ids[i], n->children[i]);
    } else if (t == AT_NODE48) {
        auto* n = reinterpret_cast<EncARTNode48*>(node);
        for (int32_t i = 0; i < 48; ++i)
            write_slot(n48_has(n, i),
                       n->plain_keys[i], n->keys[i], n->child_ids[i], n->children[i]);
    } else { // AT_NODE256
        auto* n = reinterpret_cast<EncARTNode256*>(node);
        for (int32_t i = 0; i < 256; ++i)
            write_slot(n256_has(n, i),
                       n->plain_keys[i], n->keys[i], n->child_ids[i], n->children[i]);
    }
}

// 反序列化节点构造
EncARTNode4* new_n4(const ophelib::Ciphertext& enc_zero) {
    auto* n = new EncARTNode4();
    n->header.type = AT_NODE4;
    n->bitmap = 0;
    n->keys.assign(4, enc_zero);
    n->child_ids.assign(4, enc_zero);
    n->plain_keys.assign(4, 0);
    for (int32_t i = 0; i < 4; ++i) n->children[i] = nullptr;
    return n;
}
EncARTNode16* new_n16(const ophelib::Ciphertext& enc_zero) {
    auto* n = new EncARTNode16();
    n->header.type = AT_NODE16;
    n->bitmap = 0;
    n->keys.assign(16, enc_zero);
    n->child_ids.assign(16, enc_zero);
    n->plain_keys.assign(16, 0);
    for (int32_t i = 0; i < 16; ++i) n->children[i] = nullptr;
    return n;
}
EncARTNode48* new_n48(const ophelib::Ciphertext& enc_zero) {
    auto* n = new EncARTNode48();
    n->header.type = AT_NODE48;
    n->bitmap = 0;
    n->keys.assign(48, enc_zero);
    n->child_ids.assign(48, enc_zero);
    n->plain_keys.assign(48, 0);
    for (int32_t i = 0; i < 48; ++i) n->children[i] = nullptr;
    return n;
}
EncARTNode256* new_n256(const ophelib::Ciphertext& enc_zero) {
    auto* n = new EncARTNode256();
    n->header.type = AT_NODE256;
    for (int32_t i = 0; i < 4; ++i) n->bitmap[i] = 0;
    n->keys.assign(256, enc_zero);
    n->child_ids.assign(256, enc_zero);
    n->plain_keys.assign(256, 0);
    for (int32_t i = 0; i < 256; ++i) n->children[i] = nullptr;
    return n;
}

struct ArtLoadContext {
    const std::vector<EncDataPoint*>& pool;
    const ophelib::Ciphertext& enc_zero;
    const std::shared_ptr<ophelib::Integer>& n2_shared;
    const std::shared_ptr<ophelib::FastMod>& fast_mod;
    uint32_t version;
    size_t inner_count = 0;
    size_t leaf_count  = 0;
};

void* load_art_subtree(std::istream& is, ArtLoadContext& ctx);

// 公共槽位 IO，bitmap 写入由调用方根据节点族手动完成
template <typename N>
void load_slot_payload(std::istream& is, N* n, int32_t i, ArtLoadContext& ctx) {
    n->plain_keys[i] = read_pod<uint8_t>(is);
    n->keys[i]       = read_ct(is, ctx.n2_shared, ctx.fast_mod);
    n->child_ids[i]  = read_ct(is, ctx.n2_shared, ctx.fast_mod);
    n->children[i]   = load_art_subtree(is, ctx);
}

template <typename N>
void load_slots_small_bitmap(std::istream& is, N* n, int32_t cap, ArtLoadContext& ctx) {
    for (int32_t i = 0; i < cap; ++i) {
        uint8_t present = read_pod<uint8_t>(is);
        if (!present) continue;
        load_slot_payload(is, n, i, ctx);
        n->bitmap |= static_cast<decltype(n->bitmap)>(1u << i);
    }
}

void load_slots_n48(std::istream& is, EncARTNode48* n, ArtLoadContext& ctx) {
    for (int32_t i = 0; i < 48; ++i) {
        uint8_t present = read_pod<uint8_t>(is);
        if (!present) continue;
        load_slot_payload(is, n, i, ctx);
        n48_set(n, i);
    }
}

void load_slots_n256(std::istream& is, EncARTNode256* n, ArtLoadContext& ctx) {
    for (int32_t i = 0; i < 256; ++i) {
        uint8_t present = read_pod<uint8_t>(is);
        if (!present) continue;
        load_slot_payload(is, n, i, ctx);
        n256_set(n, i);
    }
}

void* load_art_subtree(std::istream& is, ArtLoadContext& ctx) {
    uint8_t t_raw = read_pod<uint8_t>(is);
    auto t = static_cast<ARTNodeType>(t_raw);

    if (t == AT_LEAF) {
        int32_t pid = read_pod<int32_t>(is);
        auto* leaf = new EncARTLeaf();
        leaf->header.type = AT_LEAF;
        leaf->data_point  = (pid >= 0 && pid < static_cast<int32_t>(ctx.pool.size()))
                          ? ctx.pool[pid] : nullptr;
        if (ctx.version >= 6) {
            const auto count = read_pod<uint64_t>(is);
            if (count > ctx.pool.size()) { delete leaf; throw std::runtime_error("scidx: invalid duplicate count"); }
            for (uint64_t i = 0; i < count; ++i) {
                const auto duplicate_id = read_pod<int32_t>(is);
                if (duplicate_id < 0 || static_cast<size_t>(duplicate_id) >= ctx.pool.size()) {
                    delete leaf;
                    throw std::runtime_error("scidx: invalid duplicate pool id");
                }
                leaf->duplicates.push_back(ctx.pool[duplicate_id]);
            }
        }
        ++ctx.leaf_count;
        return leaf;
    }

    ++ctx.inner_count;
    if (t == AT_NODE4) {
        auto* n = new_n4(ctx.enc_zero);
        load_slots_small_bitmap(is, n, 4, ctx);
        return n;
    }
    if (t == AT_NODE16) {
        auto* n = new_n16(ctx.enc_zero);
        load_slots_small_bitmap(is, n, 16, ctx);
        return n;
    }
    if (t == AT_NODE48) {
        auto* n = new_n48(ctx.enc_zero);
        load_slots_n48(is, n, ctx);
        return n;
    }
    auto* n = new_n256(ctx.enc_zero);
    load_slots_n256(is, n, ctx);
    return n;
}

// v1 (SCIDX001) 使用 hex 字符串保存 Integer；
// v2 (SCIDX002) 改为 raw big-endian 字节（约腰斩文件体积）。
// v3 (SCIDX003) 每叶子新增 plaintext coord bbox（2*dim_count*4 字节）。
// v4 (SCIDX004) bbox 改为 Paillier 密文；用 SIC 在密文域做 OUTSIDE 判定。
// v5 (SCIDX005) z_value 升级到 128 位：plain GPL 节点 key 由 8 字节 → 16 字节
//               （hi64+lo64）。修复 d≥5 时 z 截断导致的候选爆炸。不兼容 v4。
constexpr char kMagic[8] = {'S','C','I','D','X','0','0','5'};
constexpr uint32_t kVersion = 6; // v6: ART 重复键记录列表；仍可读取 v5

} // namespace

// ============================================================================
// SULCipherIndex::save_to_file
// ============================================================================
void SULCipherIndex::save_to_file(const std::string& path) const {
    std::ofstream os(path, std::ios::binary);
    if (!os) throw std::runtime_error("scidx: 无法创建文件 " + path);

    os.write(kMagic, sizeof(kMagic));
    write_pod<uint32_t>(os, kVersion);

    const auto& paillier = crypto_.paillier();
    const auto& pub  = paillier.get_pub();
    const auto& priv = paillier.get_priv();
    write_pod<uint32_t>(os, static_cast<uint32_t>(pub.key_size_bits));
    write_pod<int32_t>(os, CryptoContext::SCALE);

    write_pod<int32_t>(os, config_.error_bound);
    write_pod<int32_t>(os, config_.max_layers);
    write_pod<int32_t>(os, config_.dim_count);

    write_pod<uint64_t>(os, static_cast<uint64_t>(priv.key_size_bits));
    write_pod<uint64_t>(os, static_cast<uint64_t>(priv.a_bits));
    write_integer(os, pub.n);
    write_integer(os, pub.g);
    write_integer(os, priv.p);
    write_integer(os, priv.q);
    write_integer(os, priv.a);

    // Plain skeleton
    const auto& plain_inner = plain_.inner_layers();
    write_pod<uint32_t>(os, static_cast<uint32_t>(plain_inner.size()));
    for (const auto& layer : plain_inner) {
        write_pod<uint32_t>(os, static_cast<uint32_t>(layer.size()));
        for (const auto& node : layer) {
            // v5: 128 位 key = hi64 + lo64
            write_pod<uint64_t>(os, static_cast<uint64_t>(node.key >> 64));
            write_pod<uint64_t>(os, static_cast<uint64_t>(node.key));
            write_pod<double>(os, node.slope);
            write_pod<double>(os, node.intercept);
            write_pod<int32_t>(os, node.child_start);
            write_pod<int32_t>(os, node.child_count);
        }
    }
    const auto& plain_leaves = plain_.leaf_nodes();
    write_pod<uint32_t>(os, static_cast<uint32_t>(plain_leaves.size()));
    for (const auto& leaf : plain_leaves) {
        // v5: 128 位 key
        write_pod<uint64_t>(os, static_cast<uint64_t>(leaf.key >> 64));
        write_pod<uint64_t>(os, static_cast<uint64_t>(leaf.key));
        write_pod<double>(os, leaf.slope);
        write_pod<double>(os, leaf.intercept);
        write_pod<int32_t>(os, leaf.slot_count);
        write_pod<int32_t>(os, leaf.filled_count);
        write_pod<int32_t>(os, leaf.seg_start);
        write_pod<int32_t>(os, leaf.seg_end);
        for (int32_t p = 0; p < leaf.slot_count; ++p)
            write_pod<uint8_t>(os, leaf.occupied[p]);
    }

    // EncDataPoint pool
    const uint32_t n_initial  = static_cast<uint32_t>(enc_points_.size());
    const uint32_t n_inserted = static_cast<uint32_t>(enc_inserted_points_.size());
    write_pod<uint32_t>(os, n_initial + n_inserted);

    std::unordered_map<EncDataPoint*, int32_t> pool_idx;
    pool_idx.reserve(static_cast<size_t>(n_initial + n_inserted));

    auto write_enc_point = [&](EncDataPoint* edp, int32_t pid) {
        pool_idx[edp] = pid;
        write_pod<int32_t>(os, edp->dim_count);
        write_pod<int32_t>(os, edp->orig_id);
        for (const auto& c : edp->dimensions) write_ct(os, c);
        write_ct(os, edp->z_value);
        for (const auto& c : edp->key_bytes) write_ct(os, c);
    };
    int32_t pid = 0;
    for (const auto& up : enc_points_)          write_enc_point(up.get(), pid++);
    for (const auto& up : enc_inserted_points_) write_enc_point(up.get(), pid++);

    // Encrypted GPL inner layers
    write_pod<uint32_t>(os, static_cast<uint32_t>(enc_inner_layers_.size()));
    for (const auto& layer : enc_inner_layers_) {
        write_pod<uint32_t>(os, static_cast<uint32_t>(layer.size()));
        for (const auto& node : layer) {
            write_ct(os, node.key);
            write_ct(os, node.slope);
            write_ct(os, node.intercept);
            write_pod<int32_t>(os, node.child_count);
            for (const auto& c : node.child_ids) write_ct(os, c);
        }
    }

    // Encrypted GPL leaf nodes
    write_pod<uint32_t>(os, static_cast<uint32_t>(enc_leaf_nodes_.size()));
    for (const auto& leaf : enc_leaf_nodes_) {
        write_ct(os, leaf.key);
        write_ct(os, leaf.slope);
        write_ct(os, leaf.intercept);
        write_pod<int32_t>(os, leaf.art_tree_idx);
        // v4: ciphertext coord bbox (含 GPL slots + ART 点)
        for (int32_t d = 0; d < config_.dim_count; ++d)
            write_ct(os, leaf.coord_lo_enc[d]);
        for (int32_t d = 0; d < config_.dim_count; ++d)
            write_ct(os, leaf.coord_hi_enc[d]);
        for (int32_t p = 0; p < leaf.slot_count; ++p) {
            EncDataPoint* edp = leaf.data_slots[p];
            int32_t s_pid = -1;
            if (edp) {
                auto it = pool_idx.find(edp);
                if (it != pool_idx.end()) s_pid = it->second;
            }
            write_pod<int32_t>(os, s_pid);
        }
    }

    // Encrypted ART trees
    write_pod<uint32_t>(os, static_cast<uint32_t>(enc_art_trees_.size()));
    for (const auto& tree : enc_art_trees_) {
        uint8_t has = (tree && tree->root()) ? 1u : 0u;
        write_pod<uint8_t>(os, has);
        if (has) save_art_subtree(os, tree->root(), pool_idx);
    }

    os.flush();
    if (!os) throw std::runtime_error("scidx: 写入失败 " + path);
}

// ============================================================================
// SULCipherIndex::load_from_file
// ============================================================================
std::pair<std::unique_ptr<CryptoContext>, std::unique_ptr<SULCipherIndex>>
SULCipherIndex::load_from_file(const std::string& path) {
    std::ifstream is(path, std::ios::binary);
    if (!is) throw std::runtime_error("scidx: 无法打开文件 " + path);

    char magic[8] = {};
    is.read(magic, sizeof(magic));
    if (std::memcmp(magic, kMagic, sizeof(magic)) != 0)
        throw std::runtime_error("scidx: magic 不匹配 " + path);
    uint32_t version = read_pod<uint32_t>(is);
    if (version != 5 && version != kVersion) throw std::runtime_error("scidx: 不支持的版本");
    uint32_t key_size = read_pod<uint32_t>(is);
    int32_t  scale_on_disk = read_pod<int32_t>(is);
    if (scale_on_disk != CryptoContext::SCALE)
        throw std::runtime_error("scidx: SCALE 不匹配");

    IndexConfig cfg;
    cfg.error_bound = read_pod<int32_t>(is);
    cfg.max_layers  = read_pod<int32_t>(is);
    cfg.dim_count   = read_pod<int32_t>(is);

    uint64_t kb = read_pod<uint64_t>(is);
    uint64_t ab = read_pod<uint64_t>(is);
    ophelib::Integer n_int = read_integer(is);
    ophelib::Integer g_int = read_integer(is);
    ophelib::Integer p_int = read_integer(is);
    ophelib::Integer q_int = read_integer(is);
    ophelib::Integer a_int = read_integer(is);

    ophelib::PublicKey  pub(static_cast<size_t>(kb), n_int, g_int);
    ophelib::PrivateKey priv(static_cast<size_t>(kb), static_cast<size_t>(ab),
                              p_int, q_int, a_int);
    ophelib::KeyPair kp(pub, priv);

    auto crypto_owner = std::make_unique<CryptoContext>(static_cast<int>(key_size), kp);
    CryptoContext& crypto = *crypto_owner;
    auto n2_shared = crypto.paillier().get_n2();
    auto fast_mod  = crypto.paillier().get_fast_mod();
    const auto& enc_zero_const = crypto.enc_zero();

    auto idx_owner = std::make_unique<SULCipherIndex>(cfg, crypto);
    SULCipherIndex& idx = *idx_owner;

    // Plain skeleton
    uint32_t n_layers = read_pod<uint32_t>(is);
    std::vector<std::vector<GPLInnerNode>> inner_layers(n_layers);
    for (uint32_t L = 0; L < n_layers; ++L) {
        uint32_t n_nodes = read_pod<uint32_t>(is);
        inner_layers[L].resize(n_nodes);
        for (uint32_t i = 0; i < n_nodes; ++i) {
            GPLInnerNode& node = inner_layers[L][i];
            // v5: 128 位 key = hi64 + lo64
            uint64_t hi = read_pod<uint64_t>(is);
            uint64_t lo = read_pod<uint64_t>(is);
            node.key         = (static_cast<__uint128_t>(hi) << 64) | lo;
            node.slope       = read_pod<double>(is);
            node.intercept   = read_pod<double>(is);
            node.child_start = read_pod<int32_t>(is);
            node.child_count = read_pod<int32_t>(is);
        }
    }
    uint32_t n_leaves = read_pod<uint32_t>(is);
    std::vector<GPLLeafNode> leaves(n_leaves);
    for (uint32_t i = 0; i < n_leaves; ++i) {
        GPLLeafNode& leaf = leaves[i];
        // v5: 128 位 key
        uint64_t hi = read_pod<uint64_t>(is);
        uint64_t lo = read_pod<uint64_t>(is);
        leaf.key          = (static_cast<__uint128_t>(hi) << 64) | lo;
        leaf.slope        = read_pod<double>(is);
        leaf.intercept    = read_pod<double>(is);
        leaf.slot_count   = read_pod<int32_t>(is);
        leaf.filled_count = read_pod<int32_t>(is);
        leaf.seg_start    = read_pod<int32_t>(is);
        leaf.seg_end      = read_pod<int32_t>(is);
        leaf.occupied.resize(leaf.slot_count);
        for (int32_t p = 0; p < leaf.slot_count; ++p)
            leaf.occupied[p] = read_pod<uint8_t>(is);
        leaf.data_slots.assign(leaf.slot_count, nullptr);
        leaf.art_root = nullptr;
    }
    idx.plain_.restore_skeleton(std::move(inner_layers), std::move(leaves));

    // Encrypted DataPoint pool
    uint32_t n_enc_points = read_pod<uint32_t>(is);
    std::vector<EncDataPoint*> pool(n_enc_points, nullptr);
    idx.enc_points_.clear();
    idx.enc_points_.reserve(n_enc_points);
    const int32_t kl = cfg.key_len();
    for (uint32_t i = 0; i < n_enc_points; ++i) {
        auto edp = std::make_unique<EncDataPoint>();
        edp->dim_count = read_pod<int32_t>(is);
        edp->orig_id   = read_pod<int32_t>(is);
        edp->dimensions.reserve(edp->dim_count);
        for (int32_t d = 0; d < edp->dim_count; ++d)
            edp->dimensions.push_back(read_ct(is, n2_shared, fast_mod));
        edp->z_value = read_ct(is, n2_shared, fast_mod);
        edp->key_bytes.reserve(kl);
        for (int32_t k = 0; k < kl; ++k)
            edp->key_bytes.push_back(read_ct(is, n2_shared, fast_mod));
        pool[i] = edp.get();
        idx.enc_points_.push_back(std::move(edp));
    }

    // Encrypted GPL inner layers
    uint32_t n_enc_layers = read_pod<uint32_t>(is);
    idx.enc_inner_layers_.clear();
    idx.enc_inner_layers_.resize(n_enc_layers);
    for (uint32_t L = 0; L < n_enc_layers; ++L) {
        uint32_t n_nodes = read_pod<uint32_t>(is);
        idx.enc_inner_layers_[L].reserve(n_nodes);
        for (uint32_t i = 0; i < n_nodes; ++i) {
            EncGPLInnerNode node;
            node.key       = read_ct(is, n2_shared, fast_mod);
            node.slope     = read_ct(is, n2_shared, fast_mod);
            node.intercept = read_ct(is, n2_shared, fast_mod);
            node.child_count = read_pod<int32_t>(is);
            node.child_start =
                (L < idx.plain_.inner_layers().size()
                 && i < idx.plain_.inner_layers()[L].size())
                ? idx.plain_.inner_layers()[L][i].child_start : 0;
            node.child_ids.reserve(node.child_count);
            for (int32_t c = 0; c < node.child_count; ++c)
                node.child_ids.push_back(read_ct(is, n2_shared, fast_mod));
            idx.enc_inner_layers_[L].push_back(std::move(node));
        }
    }

    // Encrypted GPL leaf nodes
    uint32_t n_enc_leaves = read_pod<uint32_t>(is);
    idx.enc_leaf_nodes_.clear();
    idx.enc_leaf_nodes_.reserve(n_enc_leaves);
    for (uint32_t i = 0; i < n_enc_leaves; ++i) {
        EncGPLLeafNode leaf;
        leaf.key       = read_ct(is, n2_shared, fast_mod);
        leaf.slope     = read_ct(is, n2_shared, fast_mod);
        leaf.intercept = read_ct(is, n2_shared, fast_mod);
        leaf.art_tree_idx = read_pod<int32_t>(is);
        // v4: ciphertext coord bbox
        leaf.coord_lo_enc.clear();
        leaf.coord_hi_enc.clear();
        leaf.coord_lo_enc.reserve(static_cast<size_t>(cfg.dim_count));
        leaf.coord_hi_enc.reserve(static_cast<size_t>(cfg.dim_count));
        for (int32_t d = 0; d < cfg.dim_count; ++d)
            leaf.coord_lo_enc.push_back(read_ct(is, n2_shared, fast_mod));
        for (int32_t d = 0; d < cfg.dim_count; ++d)
            leaf.coord_hi_enc.push_back(read_ct(is, n2_shared, fast_mod));
        const auto& pl_leaf = idx.plain_.leaf_nodes()[i];
        leaf.slot_count   = pl_leaf.slot_count;
        leaf.filled_count = pl_leaf.filled_count;
        leaf.occupied     = pl_leaf.occupied;
        leaf.data_slots.assign(leaf.slot_count, nullptr);
        for (int32_t p = 0; p < leaf.slot_count; ++p) {
            int32_t s_pid = read_pod<int32_t>(is);
            if (s_pid >= 0 && s_pid < static_cast<int32_t>(pool.size()))
                leaf.data_slots[p] = pool[s_pid];
        }
        idx.enc_leaf_nodes_.push_back(std::move(leaf));
    }

    // Encrypted ART trees
    uint32_t n_arts = read_pod<uint32_t>(is);
    idx.enc_art_trees_.clear();
    idx.enc_art_trees_.reserve(n_arts);
    for (uint32_t i = 0; i < n_arts; ++i) {
        auto tree = std::make_unique<EncARTTree>(kl, crypto);
        uint8_t has = read_pod<uint8_t>(is);
        if (has) {
            ArtLoadContext ctx{pool, enc_zero_const, n2_shared, fast_mod, version, 0, 0};
            void* root = load_art_subtree(is, ctx);
            tree->set_root(root, ctx.inner_count, ctx.leaf_count);
        }
        idx.enc_art_trees_.push_back(std::move(tree));
    }

    idx.is_loaded_ = true;
    return std::make_pair(std::move(crypto_owner), std::move(idx_owner));
}

} // namespace sul::cipher
