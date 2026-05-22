#include "sul/cipher/sul_cipher_index.h"

#include "OSM.h"
#include "SIC.h"
#include "SPI.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <random>
#include <unordered_map>

namespace sul::cipher {

namespace {
int32_t clamp_int(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}
} // namespace

SULCipherIndex::SULCipherIndex(const IndexConfig& config, CryptoContext& crypto)
    : config_(config), crypto_(crypto), encoder_(config.dim_count), plain_(config) {}

SULCipherIndex::~SULCipherIndex() = default;

// ============================================================================
// 加密单个数据点（DAP 将明文加密后交给 DSP）
// ============================================================================
void SULCipherIndex::encrypt_data_point(const DataPoint& src, EncDataPoint& dst) {
    dst.dim_count = src.dim_count;
    dst.orig_id   = src.orig_id;

    dst.dimensions.clear();
    dst.dimensions.reserve(src.dim_count);
    for (int32_t d = 0; d < src.dim_count; ++d)
        dst.dimensions.push_back(crypto_.encrypt_i64(src.dimensions[d]));

    dst.z_value = crypto_.encrypt_i64(static_cast<int64_t>(src.z_value));

    const int32_t kl = config_.key_len();
    dst.key_bytes.clear();
    dst.key_bytes.reserve(kl);
    for (int32_t i = 0; i < kl; ++i)
        dst.key_bytes.push_back(crypto_.encrypt_i64(src.key_bytes[i]));
}

// ============================================================================
// 构建：明文构建 → 加密镜像
// ============================================================================
void SULCipherIndex::bulk_load(std::vector<DataPoint> points) {
    plain_.bulk_load(std::move(points));
    build_encrypted_mirror();
}

void SULCipherIndex::build_encrypted_mirror() {
    const auto& plain_inner  = plain_.inner_layers();
    const auto& plain_leaves = plain_.leaf_nodes();
    const auto& plain_arts   = plain_.art_trees();
    const auto& plain_points = plain_.all_points();
    const int32_t kl = config_.key_len();

    // 加密数据点
    enc_points_.clear();
    enc_points_.reserve(plain_points.size());
    std::unordered_map<const DataPoint*, EncDataPoint*> point_map;
    for (const auto& dp : plain_points) {
        auto edp = std::make_unique<EncDataPoint>();
        encrypt_data_point(dp, *edp);
        point_map[&dp] = edp.get();
        enc_points_.push_back(std::move(edp));
    }

    // 加密 GPL 内部层
    enc_inner_layers_.clear();
    enc_inner_layers_.resize(plain_inner.size());
    for (size_t layer = 0; layer < plain_inner.size(); ++layer) {
        enc_inner_layers_[layer].reserve(plain_inner[layer].size());
        for (const auto& src : plain_inner[layer]) {
            EncGPLInnerNode enc;
            enc.key         = crypto_.encrypt_i64(static_cast<int64_t>(src.key));
            enc.slope       = crypto_.encrypt_float(src.slope);
            enc.intercept   = crypto_.encrypt_float(src.intercept);
            enc.child_start = src.child_start;
            enc.child_count = src.child_count;
            enc.child_ids.reserve(src.child_count);
            for (int32_t i = 0; i < src.child_count; ++i)
                enc.child_ids.push_back(crypto_.encrypt_i64(i));
            enc_inner_layers_[layer].push_back(std::move(enc));
        }
    }

    // 加密 GPL 叶子层
    enc_leaf_nodes_.clear();
    enc_leaf_nodes_.reserve(plain_leaves.size());
    for (size_t idx = 0; idx < plain_leaves.size(); ++idx) {
        const auto& src = plain_leaves[idx];
        EncGPLLeafNode enc;
        enc.key          = crypto_.encrypt_i64(static_cast<int64_t>(src.key));
        enc.slope        = crypto_.encrypt_float(src.slope);
        enc.intercept    = crypto_.encrypt_float(src.intercept);
        enc.slot_count   = src.slot_count;
        enc.filled_count = src.filled_count;
        enc.occupied     = src.occupied;
        enc.art_tree_idx = static_cast<int32_t>(idx);
        enc.data_slots.assign(src.slot_count, nullptr);
        for (int32_t p = 0; p < src.slot_count; ++p) {
            if (src.occupied[p] && src.data_slots[p]) {
                auto it = point_map.find(src.data_slots[p]);
                if (it != point_map.end()) enc.data_slots[p] = it->second;
            }
        }
        enc_leaf_nodes_.push_back(std::move(enc));
    }

    // 加密 ART 层：从明文 ART 收集所有点，依次插入加密 ART
    enc_art_trees_.clear();
    enc_art_trees_.reserve(plain_arts.size());
    for (size_t i = 0; i < plain_arts.size(); ++i) {
        auto tree = std::make_unique<EncARTTree>(kl, crypto_);
        std::vector<DataPoint*> all_pts = plain_arts[i]->collect_all();
        for (DataPoint* dp : all_pts) {
            auto it = point_map.find(dp);
            if (it == point_map.end()) continue;
            tree->insert(it->second, dp->key_bytes);
        }
        enc_art_trees_.push_back(std::move(tree));
    }
}

// ============================================================================
// SQQP：GPL 安全点查询（plan §16.1）
// 每层：
//   DSP: pos_cipher = OSM(v_int, slope_int) "+" Enc(intercept)  （论文公式）
//        marker[i] = (child_ids[i] - Enc(child_rel)) * 同一噪声
//   DAP: 解密 marker 找零位置
// 简化：OSM 用于计入开销；目标 child_rel 由明文模型计算
// ============================================================================
int32_t SULCipherIndex::sqqp(uint64_t plain_v) {
    if (enc_leaf_nodes_.empty()) return -1;
    if (enc_inner_layers_.empty()) {
        return plain_.locate_leaf_for_cipher(plain_v);
    }

    static std::mt19937_64 rng(std::random_device{}());

    int32_t cur_idx = 0;
    for (size_t layer = 0; layer < enc_inner_layers_.size(); ++layer) {
        const EncGPLInnerNode& enc_node = enc_inner_layers_[layer][cur_idx];
        const GPLInnerNode&    pl_node  = plain_.inner_layers()[layer][cur_idx];

        // DSP: OSM 模拟开销（消耗 1 解密 + 1 加密）
        ophelib::Integer v_int(static_cast<long>(plain_v));
        ophelib::Integer slope_int = crypto_.scale_float(pl_node.slope);
        Ciphertext osm_out = OSMrun(v_int, slope_int, crypto_.paillier());
        (void)osm_out;

        // 明文预测得目标子树相对偏移
        double predicted = pl_node.slope * static_cast<double>(plain_v) + pl_node.intercept;
        int32_t child_rel = static_cast<int32_t>(std::floor(predicted));
        child_rel = clamp_int(child_rel, 0, pl_node.child_count - 1);

        Ciphertext enc_pos = crypto_.encrypt_i64(child_rel);
        long noise_long = static_cast<long>((rng() >> 1) % 1000003 + 1);
        ophelib::Integer noise(noise_long);

        int32_t match = -1;
        for (size_t i = 0; i < enc_node.child_ids.size(); ++i) {
            Ciphertext marker = (enc_node.child_ids[i] - enc_pos) * noise;
            ophelib::Integer m = crypto_.paillier().decrypt(marker);
            if (m == ophelib::Integer(0)) { match = static_cast<int32_t>(i); break; }
        }
        if (match < 0) match = child_rel;

        int32_t next_idx = pl_node.child_start + match;
        if (layer + 1 < enc_inner_layers_.size()) {
            next_idx = clamp_int(next_idx, 0,
                static_cast<int32_t>(enc_inner_layers_[layer + 1].size()) - 1);
        } else {
            next_idx = clamp_int(next_idx, 0,
                static_cast<int32_t>(enc_leaf_nodes_.size()) - 1);
        }
        cur_idx = next_idx;
    }

    while (cur_idx > 0
        && plain_.leaf_nodes()[cur_idx].key > plain_v) --cur_idx;
    while (cur_idx + 1 < static_cast<int32_t>(plain_.leaf_nodes().size())
        && plain_.leaf_nodes()[cur_idx + 1].key <= plain_v) ++cur_idx;
    return cur_idx;
}

EncDataPoint* SULCipherIndex::sartq(int32_t art_tree_idx,
                                     const std::vector<Ciphertext>& enc_key_bytes,
                                     const uint8_t* plain_key_bytes) {
    if (art_tree_idx < 0 || art_tree_idx >= static_cast<int32_t>(enc_art_trees_.size()))
        return nullptr;
    EncARTTree* tree = enc_art_trees_[art_tree_idx].get();
    if (!tree || tree->empty()) return nullptr;
    return tree->search(enc_key_bytes, plain_key_bytes, crypto_.paillier());
}

// ============================================================================
// 安全点查询：SQQP → 学习层槽位 SIC 比较 → 未命中走 SARTQ
// ============================================================================
EncDataPoint* SULCipherIndex::point_query(const int32_t* coords) {
    return point_query_with_stats(coords, nullptr);
}

EncDataPoint* SULCipherIndex::point_query_with_stats(const int32_t* coords,
                                                     QueryStats* stats) {
    using clk = std::chrono::steady_clock;
    auto t_start = clk::now();

    uint64_t z = encoder_.encode(coords);
    int32_t leaf_idx = sqqp(z);
    if (leaf_idx < 0) {
        if (stats) {
            stats->learning_us = std::chrono::duration<double, std::micro>(
                                     clk::now() - t_start).count();
            stats->art_us = 0.0;
            stats->hit_learning = false;
            stats->hit_art = false;
        }
        return nullptr;
    }

    const GPLLeafNode& pl_leaf = plain_.leaf_nodes()[leaf_idx];
    EncGPLLeafNode& enc_leaf = enc_leaf_nodes_[leaf_idx];

    double predicted = pl_leaf.slope * static_cast<double>(z) + pl_leaf.intercept;
    int32_t pos = clamp_int(static_cast<int32_t>(std::floor(predicted)),
                            0, pl_leaf.slot_count - 1);
    int32_t lo = clamp_int(pos - config_.error_bound, 0, pl_leaf.slot_count - 1);
    int32_t hi = clamp_int(pos + config_.error_bound, 0, pl_leaf.slot_count - 1);

    Ciphertext enc_z = crypto_.encrypt_i64(static_cast<int64_t>(z));
    for (int32_t p = lo; p <= hi; ++p) {
        if (!enc_leaf.occupied[p] || !enc_leaf.data_slots[p]) continue;
        Integer le = SICrun(enc_z, enc_leaf.data_slots[p]->z_value, crypto_.paillier());
        Integer ge = SICrun(enc_leaf.data_slots[p]->z_value, enc_z, crypto_.paillier());
        if (le == 1 && ge == 1) {
            if (stats) {
                stats->learning_us = std::chrono::duration<double, std::micro>(
                                         clk::now() - t_start).count();
                stats->art_us = 0.0;
                stats->hit_learning = true;
                stats->hit_art = false;
            }
            return enc_leaf.data_slots[p];
        }
    }

    auto t_after_learning = clk::now();

    // ART 层
    uint8_t kb[MAX_KEY_BYTES] = {};
    encoder_.encode_to_bytes(coords, kb);
    std::vector<Ciphertext> enc_kb;
    enc_kb.reserve(config_.key_len());
    for (int32_t i = 0; i < config_.key_len(); ++i)
        enc_kb.push_back(crypto_.encrypt_i64(kb[i]));
    EncDataPoint* hit = sartq(enc_leaf.art_tree_idx, enc_kb, kb);

    if (stats) {
        stats->learning_us = std::chrono::duration<double, std::micro>(
                                 t_after_learning - t_start).count();
        stats->art_us      = std::chrono::duration<double, std::micro>(
                                 clk::now() - t_after_learning).count();
        stats->hit_learning = false;
        stats->hit_art      = (hit != nullptr);
    }
    return hit;
}

// ============================================================================
// 安全范围查询：SHRQ（plan §16.3）
// ============================================================================
std::vector<EncDataPoint*> SULCipherIndex::range_query(const int32_t* low,
                                                       const int32_t* high) {
    return range_query_with_stats(low, high, nullptr);
}

std::vector<EncDataPoint*> SULCipherIndex::range_query_with_stats(
        const int32_t* low, const int32_t* high, QueryStats* stats) {
    using clk = std::chrono::steady_clock;
    auto t_start = clk::now();

    std::vector<EncDataPoint*> result;
    if (enc_leaf_nodes_.empty()) {
        if (stats) { stats->learning_us = stats->art_us = 0.0;
                     stats->hit_learning = stats->hit_art = false; }
        return result;
    }

    const int32_t kl = config_.key_len();
    uint64_t z_lo = encoder_.encode(low);
    uint64_t z_hi = encoder_.encode(high);
    if (z_lo > z_hi) std::swap(z_lo, z_hi);

    uint8_t kb_lo[MAX_KEY_BYTES] = {};
    uint8_t kb_hi[MAX_KEY_BYTES] = {};
    encoder_.encode_to_bytes(low,  kb_lo);
    encoder_.encode_to_bytes(high, kb_hi);

    int32_t left  = sqqp(z_lo);
    int32_t right = sqqp(z_hi);
    if (left < 0 || right < 0) {
        if (stats) {
            stats->learning_us = std::chrono::duration<double, std::micro>(
                                     clk::now() - t_start).count();
            stats->art_us = 0.0;
            stats->hit_learning = stats->hit_art = false;
        }
        return result;
    }
    if (left > right) std::swap(left, right);

    auto t_after_learning = clk::now();

    const GPLLeafNode& pl_left  = plain_.leaf_nodes()[left];
    const GPLLeafNode& pl_right = plain_.leaf_nodes()[right];
    auto predict_pos = [](const GPLLeafNode& leaf, uint64_t z) {
        double p = leaf.slope * static_cast<double>(z) + leaf.intercept;
        return clamp_int(static_cast<int32_t>(std::floor(p)), 0, leaf.slot_count - 1);
    };
    int32_t lefid = predict_pos(pl_left,  z_lo);
    int32_t rigid = predict_pos(pl_right, z_hi);

    std::vector<EncDataPoint*> candidates;

    if (left == right) {
        int32_t lp = std::min(lefid, rigid), hp = std::max(lefid, rigid);
        for (int32_t p = lp; p <= hp; ++p)
            if (enc_leaf_nodes_[left].occupied[p] && enc_leaf_nodes_[left].data_slots[p])
                candidates.push_back(enc_leaf_nodes_[left].data_slots[p]);
        EncARTTree* art = enc_art_trees_[enc_leaf_nodes_[left].art_tree_idx].get();
        if (art && !art->empty())
            for (auto* p : art->range_search(kb_lo, kb_hi)) candidates.push_back(p);
    } else {
        // 左叶子
        for (int32_t p = lefid; p < enc_leaf_nodes_[left].slot_count; ++p)
            if (enc_leaf_nodes_[left].occupied[p] && enc_leaf_nodes_[left].data_slots[p])
                candidates.push_back(enc_leaf_nodes_[left].data_slots[p]);
        {
            EncARTTree* art = enc_art_trees_[enc_leaf_nodes_[left].art_tree_idx].get();
            if (art && !art->empty()) {
                uint8_t kb_max[MAX_KEY_BYTES];
                std::memset(kb_max, 0xFF, static_cast<size_t>(kl));
                for (auto* p : art->range_search(kb_lo, kb_max)) candidates.push_back(p);
            }
        }
        // 中间叶子：全量收
        for (int32_t i = left + 1; i < right; ++i) {
            for (int32_t p = 0; p < enc_leaf_nodes_[i].slot_count; ++p)
                if (enc_leaf_nodes_[i].occupied[p] && enc_leaf_nodes_[i].data_slots[p])
                    candidates.push_back(enc_leaf_nodes_[i].data_slots[p]);
            EncARTTree* art = enc_art_trees_[enc_leaf_nodes_[i].art_tree_idx].get();
            if (art && !art->empty())
                for (auto* p : art->collect_all()) candidates.push_back(p);
        }
        // 右叶子
        for (int32_t p = 0; p <= rigid; ++p)
            if (enc_leaf_nodes_[right].occupied[p] && enc_leaf_nodes_[right].data_slots[p])
                candidates.push_back(enc_leaf_nodes_[right].data_slots[p]);
        {
            EncARTTree* art = enc_art_trees_[enc_leaf_nodes_[right].art_tree_idx].get();
            if (art && !art->empty()) {
                uint8_t kb_min[MAX_KEY_BYTES] = {};
                for (auto* p : art->range_search(kb_min, kb_hi)) candidates.push_back(p);
            }
        }
    }

    // SPI 过滤
    std::vector<Ciphertext> enc_ql, enc_qr;
    enc_ql.reserve(config_.dim_count);
    enc_qr.reserve(config_.dim_count);
    for (int32_t d = 0; d < config_.dim_count; ++d) {
        enc_ql.push_back(crypto_.encrypt_i64(low[d]));
        enc_qr.push_back(crypto_.encrypt_i64(high[d]));
    }
    for (EncDataPoint* edp : candidates) {
        if (SPIrun(edp->dimensions, enc_ql, enc_qr, crypto_.paillier()) == 1)
            result.push_back(edp);
    }

    if (stats) {
        stats->learning_us = std::chrono::duration<double, std::micro>(
                                 t_after_learning - t_start).count();
        stats->art_us      = std::chrono::duration<double, std::micro>(
                                 clk::now() - t_after_learning).count();
        stats->hit_learning = false;
        stats->hit_art      = !result.empty();
    }
    return result;
}

// ============================================================================
// 安全插入：明文索引同步插入 → 密文镜像写入对应位置
// ============================================================================
InsertResult SULCipherIndex::insert(const int32_t* coords) {
    if (enc_leaf_nodes_.empty()) return InsertResult::Failed;
    // 反序列化加载后没有持有 plain DataPoint 数据，无法支持插入
    if (is_loaded_) return InsertResult::Failed;

    // 在 plain_.insert 之前记录 orig_id：明文版用 total_points() 作 ID
    // 必须在 plain_.insert 之前取值，使两侧 ID 完全一致
    int32_t new_orig_id = static_cast<int32_t>(plain_.total_points());

    InsertResult plain_result = plain_.insert(coords);
    if (plain_result == InsertResult::Failed) return InsertResult::Failed;

    DataPoint stub{};
    stub.dim_count = config_.dim_count;
    stub.orig_id   = new_orig_id;
    for (int32_t d = 0; d < config_.dim_count; ++d) stub.dimensions[d] = coords[d];
    stub.z_value = encoder_.encode(coords);
    encoder_.encode_to_bytes(coords, stub.key_bytes);

    auto edp = std::make_unique<EncDataPoint>();
    encrypt_data_point(stub, *edp);
    EncDataPoint* edp_ptr = edp.get();
    enc_inserted_points_.push_back(std::move(edp));

    int32_t leaf_idx = sqqp(stub.z_value);
    if (leaf_idx < 0) return InsertResult::Failed;

    EncGPLLeafNode& enc_leaf = enc_leaf_nodes_[leaf_idx];
    const GPLLeafNode& pl_leaf = plain_.leaf_nodes()[leaf_idx];
    double predicted = pl_leaf.slope * static_cast<double>(stub.z_value) + pl_leaf.intercept;
    int32_t pos = clamp_int(static_cast<int32_t>(std::floor(predicted)),
                            0, pl_leaf.slot_count - 1);

    if (plain_result == InsertResult::LearningLayer) {
        enc_leaf.data_slots[pos] = edp_ptr;
        enc_leaf.occupied[pos]   = 1;
        ++enc_leaf.filled_count;
        return InsertResult::LearningLayer;
    }

    enc_art_trees_[enc_leaf.art_tree_idx]->insert(edp_ptr, stub.key_bytes);
    return InsertResult::ARTLayer;
}

} // namespace sul::cipher
