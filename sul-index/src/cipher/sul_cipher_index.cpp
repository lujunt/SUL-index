#include "sul/cipher/sul_cipher_index.h"

#include "OSM.h"
#include "SIC.h"
#include "SPI.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <thread>
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
// Encrypt one data point on the DAP before sending it to the DSP.
// ============================================================================
void SULCipherIndex::encrypt_data_point(const DataPoint& src, EncDataPoint& dst) {
    dst.dim_count = src.dim_count;
    dst.orig_id   = src.orig_id;

    dst.dimensions.clear();
    dst.dimensions.reserve(src.dim_count);
    for (int32_t d = 0; d < src.dim_count; ++d)
        dst.dimensions.push_back(crypto_.encrypt_i64(src.dimensions[d]));

    dst.z_value = crypto_.encrypt_u128(src.z_value);

    const int32_t kl = config_.key_len();
    dst.key_bytes.clear();
    dst.key_bytes.reserve(kl);
    for (int32_t i = 0; i < kl; ++i)
        dst.key_bytes.push_back(crypto_.encrypt_i64(src.key_bytes[i]));
}

// ============================================================================
// Build the plaintext index, then its encrypted mirror.
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

    // Encrypt data points.
    enc_points_.clear();
    enc_points_.reserve(plain_points.size());
    std::unordered_map<const DataPoint*, EncDataPoint*> point_map;
    for (const auto& dp : plain_points) {
        auto edp = std::make_unique<EncDataPoint>();
        encrypt_data_point(dp, *edp);
        point_map[&dp] = edp.get();
        enc_points_.push_back(std::move(edp));
    }

    // Encrypt GPL inner layers.
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

    // Encrypt GPL leaves.
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

    // Encrypt the ART layer by collecting plaintext ART points and inserting each one.
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

    // Build encrypted coordinate bboxes over GPL slots and ART points for each leaf.
    // Empty leaves use zero placeholders. Also initialize DAP-side plaintext bboxes for updates.
    const int32_t D = config_.dim_count;
    plain_bbox_lo_.assign(enc_leaf_nodes_.size(), std::vector<int32_t>(D, 0));
    plain_bbox_hi_.assign(enc_leaf_nodes_.size(), std::vector<int32_t>(D, 0));
    for (size_t idx = 0; idx < enc_leaf_nodes_.size(); ++idx) {
        EncGPLLeafNode& enc = enc_leaf_nodes_[idx];
        const GPLLeafNode& src = plain_leaves[idx];
        int32_t lo[MAX_DIMS], hi[MAX_DIMS];
        for (int32_t d = 0; d < D; ++d) {
            lo[d] = std::numeric_limits<int32_t>::max();
            hi[d] = std::numeric_limits<int32_t>::min();
        }
        bool any = false;
        auto update = [&](const int32_t* coord) {
            for (int32_t d = 0; d < D; ++d) {
                if (coord[d] < lo[d]) lo[d] = coord[d];
                if (coord[d] > hi[d]) hi[d] = coord[d];
            }
            any = true;
        };
        for (int32_t p = 0; p < src.slot_count; ++p) {
            if (src.occupied[p] && src.data_slots[p])
                update(src.data_slots[p]->dimensions);
        }
        if (enc.art_tree_idx >= 0
            && enc.art_tree_idx < static_cast<int32_t>(plain_arts.size())) {
            for (DataPoint* dp : plain_arts[enc.art_tree_idx]->collect_all())
                update(dp->dimensions);
        }
        enc.coord_lo_enc.clear();
        enc.coord_hi_enc.clear();
        enc.coord_lo_enc.reserve(static_cast<size_t>(D));
        enc.coord_hi_enc.reserve(static_cast<size_t>(D));
        for (int32_t d = 0; d < D; ++d) {
            int32_t l = any ? lo[d] : 0;
            int32_t h = any ? hi[d] : 0;
            enc.coord_lo_enc.push_back(crypto_.encrypt_i64(l));
            enc.coord_hi_enc.push_back(crypto_.encrypt_i64(h));
            plain_bbox_lo_[idx][d] = l;
            plain_bbox_hi_[idx][d] = h;
        }
    }
}

// ============================================================================
// SQQP secure GPL point query. At each level the DSP computes the encrypted predicted
// position and blinded child markers; the DAP decrypts markers to find the zero position.
// OSM accounts for protocol cost while the plaintext model determines child_rel.
// ============================================================================
int32_t SULCipherIndex::sqqp(__uint128_t plain_v) {
    if (enc_leaf_nodes_.empty()) return -1;
    if (enc_inner_layers_.empty()) {
        return plain_.locate_leaf_for_cipher(plain_v);
    }

    static std::mt19937_64 rng(std::random_device{}());

    int32_t cur_idx = 0;
    for (size_t layer = 0; layer < enc_inner_layers_.size(); ++layer) {
        const EncGPLInnerNode& enc_node = enc_inner_layers_[layer][cur_idx];
        const GPLInnerNode&    pl_node  = plain_.inner_layers()[layer][cur_idx];

        // DSP: OSM simulation consumes one decryption and one encryption.
        ophelib::Integer v_int = CryptoContext::u128_to_integer(plain_v);
        ophelib::Integer slope_int = crypto_.scale_float(pl_node.slope);
        Ciphertext osm_out = OSMrun(v_int, slope_int, crypto_.paillier());
        (void)osm_out;

        // The plaintext model predicts the target subtree's relative offset.
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
// Secure point query: SQQP, SIC over learning slots, then SARTQ on a miss.
// ============================================================================
EncDataPoint* SULCipherIndex::point_query(const int32_t* coords) {
    return point_query_with_stats(coords, nullptr);
}

EncDataPoint* SULCipherIndex::point_query_with_stats(const int32_t* coords,
                                                     QueryStats* stats) {
    using clk = std::chrono::steady_clock;
    auto t_start = clk::now();

    __uint128_t z = encoder_.encode(coords);
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

    Ciphertext enc_z = crypto_.encrypt_u128(z);
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

    // ART layer.
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
// SHRQ secure range query.
// ============================================================================
std::vector<EncDataPoint*> SULCipherIndex::range_query(const int32_t* low,
                                                       const int32_t* high) {
    return range_query_with_stats(low, high, nullptr);
}

std::vector<EncDataPoint*> SULCipherIndex::range_query_with_stats(
        const int32_t* low, const int32_t* high, QueryStats* stats) {
    return range_query_impl(low, high, stats, true);
}

size_t SULCipherIndex::count_range_candidates(const int32_t* low, const int32_t* high,
                                            QueryStats* stats) {
    QueryStats local{};
    if (!stats) stats = &local;
    range_query_impl(low, high, stats, false);
    return stats->candidates_total;
}

std::vector<EncDataPoint*> SULCipherIndex::range_query_impl(
        const int32_t* low, const int32_t* high, QueryStats* stats, bool filter_candidates) {
    using clk = std::chrono::steady_clock;
    auto t_start = clk::now();

    std::vector<EncDataPoint*> result;
    if (enc_leaf_nodes_.empty()) {
        if (stats) {
            stats->learning_us = stats->art_us = 0.0;
            stats->collect_us = stats->spi_setup_us = stats->spi_filter_us = 0.0;
            stats->candidates_total = stats->candidates_kept = 0;
            stats->middle_leaves_total = stats->middle_leaves_pruned = 0;
            stats->hit_learning = stats->hit_art = false;
        }
        return result;
    }

    const int32_t kl = config_.key_len();
    __uint128_t z_lo = encoder_.encode(low);
    __uint128_t z_hi = encoder_.encode(high);
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
            stats->collect_us = stats->spi_setup_us = stats->spi_filter_us = 0.0;
            stats->candidates_total = stats->candidates_kept = 0;
            stats->middle_leaves_total = stats->middle_leaves_pruned = 0;
            stats->hit_learning = stats->hit_art = false;
        }
        return result;
    }
    if (left > right) std::swap(left, right);

    auto t_after_learning = clk::now();

    // Encrypt query bounds once and reuse them for bbox SIC checks and SPI.
    std::vector<Ciphertext> enc_ql, enc_qr;
    enc_ql.reserve(config_.dim_count);
    enc_qr.reserve(config_.dim_count);
    for (int32_t d = 0; d < config_.dim_count; ++d) {
        enc_ql.push_back(crypto_.encrypt_i64(low[d]));
        enc_qr.push_back(crypto_.encrypt_i64(high[d]));
    }
    auto t_after_spi_setup = clk::now();

    std::vector<EncDataPoint*> candidates;
    size_t middle_total  = 0;
    size_t middle_pruned = 0;

    // Layer 1 OUTSIDE pruning: SIC checks whether the bbox is disjoint from [low, high]
    // in any dimension. The two comparisons within a dimension run in parallel, while
    // dimensions remain sequential for early exit. Insertions keep encrypted bboxes sound.
    auto leaf_outside = [&](const EncGPLLeafNode& leaf) -> bool {
        for (int32_t d = 0; d < config_.dim_count; ++d) {
            Integer cmp_low_in;
            Integer cmp_hi_in;
            std::thread t_hi([&]() {
                cmp_hi_in = SICrun(leaf.coord_lo_enc[d], enc_qr[d], crypto_.paillier());
            });
            cmp_low_in = SICrun(enc_ql[d], leaf.coord_hi_enc[d], crypto_.paillier());
            t_hi.join();
            if (cmp_low_in != 1 || cmp_hi_in != 1) return true;
        }
        return false;
    };

    if (left == right) {
        // Collect all valid slots without Z-order filtering, matching the plaintext path.
        // Z-curve discontinuities make such filtering incomplete; SPI provides exact filtering.
        for (int32_t p = 0; p < enc_leaf_nodes_[left].slot_count; ++p)
            if (enc_leaf_nodes_[left].occupied[p] && enc_leaf_nodes_[left].data_slots[p])
                candidates.push_back(enc_leaf_nodes_[left].data_slots[p]);
        EncARTTree* art = enc_art_trees_[enc_leaf_nodes_[left].art_tree_idx].get();
        if (art && !art->empty())
            for (auto* p : art->range_search(kb_lo, kb_hi)) candidates.push_back(p);
    } else {
        // Scan every slot in the left leaf to avoid Z-curve false negatives.
        for (int32_t p = 0; p < enc_leaf_nodes_[left].slot_count; ++p)
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
        // Skip OUTSIDE middle leaves; otherwise collect all points.
        for (int32_t i = left + 1; i < right; ++i) {
            ++middle_total;
            const EncGPLLeafNode& mleaf = enc_leaf_nodes_[i];
            if (leaf_outside(mleaf)) {
                ++middle_pruned;
                continue;
            }
            for (int32_t p = 0; p < mleaf.slot_count; ++p)
                if (mleaf.occupied[p] && mleaf.data_slots[p])
                    candidates.push_back(mleaf.data_slots[p]);
            EncARTTree* art = enc_art_trees_[mleaf.art_tree_idx].get();
            if (art && !art->empty())
                for (auto* p : art->collect_all()) candidates.push_back(p);
        }
        // Scan every slot in the right leaf to avoid Z-curve false negatives.
        for (int32_t p = 0; p < enc_leaf_nodes_[right].slot_count; ++p)
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

    auto t_after_collect = clk::now();

    // SPI filtering reuses query bounds encrypted before bbox checks.
    if (filter_candidates) {
        for (EncDataPoint* edp : candidates) {
            if (SPIrun(edp->dimensions, enc_ql, enc_qr, crypto_.paillier()) == 1)
                result.push_back(edp);
        }
    }
    auto t_end = clk::now();

    if (stats) {
        stats->learning_us = std::chrono::duration<double, std::micro>(
                                 t_after_learning - t_start).count();
        stats->spi_setup_us  = std::chrono::duration<double, std::micro>(
                                 t_after_spi_setup - t_after_learning).count();
        stats->collect_us  = std::chrono::duration<double, std::micro>(
                                 t_after_collect - t_after_spi_setup).count();
        stats->spi_filter_us = filter_candidates ? std::chrono::duration<double, std::micro>(
                                 t_end - t_after_collect).count() : 0.0;
        stats->art_us = stats->collect_us + stats->spi_setup_us
                      + stats->spi_filter_us;
        stats->candidates_total = candidates.size();
        stats->candidates_kept  = result.size();
        stats->middle_leaves_total  = middle_total;
        stats->middle_leaves_pruned = middle_pruned;
        stats->hit_learning = false;
        stats->hit_art      = !result.empty();
    }
    return result;
}

// ============================================================================
// Secure insertion: update the plaintext index and corresponding encrypted mirror position.
// ============================================================================
InsertResult SULCipherIndex::insert(const int32_t* coords) {
    if (enc_leaf_nodes_.empty()) return InsertResult::Failed;
    // Deserialized indexes have no plaintext DataPoints and cannot support insertion.
    if (is_loaded_) return InsertResult::Failed;

    // Capture orig_id before plain_.insert, which uses total_points(), so both sides match.
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

    // Re-encrypt any leaf bbox boundary expanded by the new point, preserving sound
    // middle-leaf pruning with at most dim*2 Paillier encryptions per insertion.
    if (static_cast<size_t>(leaf_idx) < plain_bbox_lo_.size()) {
        auto& bb_lo = plain_bbox_lo_[leaf_idx];
        auto& bb_hi = plain_bbox_hi_[leaf_idx];
        for (int32_t d = 0; d < config_.dim_count; ++d) {
            if (coords[d] < bb_lo[d]) {
                bb_lo[d] = coords[d];
                enc_leaf.coord_lo_enc[d] = crypto_.encrypt_i64(coords[d]);
            }
            if (coords[d] > bb_hi[d]) {
                bb_hi[d] = coords[d];
                enc_leaf.coord_hi_enc[d] = crypto_.encrypt_i64(coords[d]);
            }
        }
    }
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
