#include "sul/sul_index.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace sul {

SULPlainIndex::SULPlainIndex(const IndexConfig& config)
    : config_(config), encoder_(config.dim_count) {}

SULPlainIndex::~SULPlainIndex() = default;

namespace {

int32_t clamp_int(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

}

void SULPlainIndex::bulk_load(std::vector<DataPoint> points) {
    all_points_ = std::move(points);

    for (auto& dp : all_points_) {
        dp.z_value = encoder_.encode(dp.dimensions);
        ZOrderEncoder::to_bytes(dp.z_value, dp.key_bytes);
    }

    std::sort(all_points_.begin(), all_points_.end(),
              [](const DataPoint& a, const DataPoint& b) {
                  return a.z_value < b.z_value;
              });

    std::vector<uint64_t> sorted_keys;
    std::vector<DataPoint*> sorted_ptrs;
    sorted_keys.reserve(all_points_.size());
    sorted_ptrs.reserve(all_points_.size());
    for (auto& dp : all_points_) {
        sorted_keys.push_back(dp.z_value);
        sorted_ptrs.push_back(&dp);
    }

    GPLBuilder builder(config_.error_bound);
    GPLBuildResult res = builder.build(sorted_keys, sorted_ptrs, config_.max_layers);

    inner_layers_ = std::move(res.inner_layers);
    leaf_nodes_ = std::move(res.leaf_nodes);

    art_trees_.clear();
    art_trees_.reserve(leaf_nodes_.size());
    for (size_t i = 0; i < leaf_nodes_.size(); ++i) {
        art_trees_.emplace_back(std::make_unique<ARTTree>());
    }

    for (size_t k = 0; k < res.conflict_set.size(); ++k) {
        int32_t leaf_idx = res.conflict_leaf_idx[k];
        art_trees_[leaf_idx]->insert(res.conflict_set[k]);
    }

    for (size_t i = 0; i < leaf_nodes_.size(); ++i) {
        leaf_nodes_[i].art_root = art_trees_[i]->root();
    }
}

int32_t SULPlainIndex::locate_leaf(uint64_t z_value) const {
    if (leaf_nodes_.empty()) return -1;

    if (inner_layers_.empty()) {
        int32_t lo = 0;
        int32_t hi = static_cast<int32_t>(leaf_nodes_.size()) - 1;
        int32_t ans = 0;
        while (lo <= hi) {
            int32_t mid = (lo + hi) >> 1;
            if (leaf_nodes_[mid].key <= z_value) {
                ans = mid;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        return ans;
    }

    int32_t cur_idx = 0;
    for (size_t layer = 0; layer < inner_layers_.size(); ++layer) {
        const GPLInnerNode& node = inner_layers_[layer][cur_idx];
        double predicted = node.slope * static_cast<double>(z_value) + node.intercept;
        int32_t rel = static_cast<int32_t>(std::floor(predicted));
        rel = clamp_int(rel, 0, node.child_count - 1);
        cur_idx = node.child_start + rel;

        if (layer + 1 < inner_layers_.size()) {
            int32_t next_size = static_cast<int32_t>(inner_layers_[layer + 1].size());
            cur_idx = clamp_int(cur_idx, 0, next_size - 1);
        } else {
            int32_t leaf_size = static_cast<int32_t>(leaf_nodes_.size());
            cur_idx = clamp_int(cur_idx, 0, leaf_size - 1);
        }
    }

    while (cur_idx > 0 && leaf_nodes_[cur_idx].key > z_value) --cur_idx;
    while (cur_idx + 1 < static_cast<int32_t>(leaf_nodes_.size())
           && leaf_nodes_[cur_idx + 1].key <= z_value) {
        ++cur_idx;
    }
    return cur_idx;
}

DataPoint* SULPlainIndex::search_leaf_for_zvalue(int32_t leaf_idx, uint64_t z_value) const {
    const GPLLeafNode& leaf = leaf_nodes_[leaf_idx];
    double predicted = leaf.slope * static_cast<double>(z_value) + leaf.intercept;
    int32_t pos = static_cast<int32_t>(std::floor(predicted));
    pos = clamp_int(pos, 0, leaf.slot_count - 1);

    int32_t lo = clamp_int(pos - config_.error_bound, 0, leaf.slot_count - 1);
    int32_t hi = clamp_int(pos + config_.error_bound, 0, leaf.slot_count - 1);

    for (int32_t p = lo; p <= hi; ++p) {
        if (leaf.occupied[p] && leaf.data_slots[p]
            && leaf.data_slots[p]->z_value == z_value) {
            return leaf.data_slots[p];
        }
    }
    return nullptr;
}

DataPoint* SULPlainIndex::point_query(const int32_t* coords) const {
    uint64_t z = encoder_.encode(coords);
    int32_t leaf_idx = locate_leaf(z);
    if (leaf_idx < 0) return nullptr;

    DataPoint* hit = search_leaf_for_zvalue(leaf_idx, z);
    if (hit) {
        bool match = true;
        for (int32_t d = 0; d < config_.dim_count; ++d) {
            if (hit->dimensions[d] != coords[d]) { match = false; break; }
        }
        if (match) return hit;
    }

    const ARTTree* art = art_trees_[leaf_idx].get();
    if (art && !art->empty()) {
        uint8_t kb[KEY_BYTES];
        ZOrderEncoder::to_bytes(z, kb);
        DataPoint* art_hit = art->search(kb);
        if (art_hit) {
            bool match = true;
            for (int32_t d = 0; d < config_.dim_count; ++d) {
                if (art_hit->dimensions[d] != coords[d]) { match = false; break; }
            }
            if (match) return art_hit;
        }
    }
    return nullptr;
}

std::vector<DataPoint*> SULPlainIndex::range_query(const int32_t* low,
                                                   const int32_t* high) const {
    std::vector<DataPoint*> result;
    if (leaf_nodes_.empty()) return result;

    uint64_t z_lo = encoder_.encode(low);
    uint64_t z_hi = encoder_.encode(high);
    if (z_lo > z_hi) std::swap(z_lo, z_hi);

    uint8_t kb_lo[KEY_BYTES];
    uint8_t kb_hi[KEY_BYTES];
    ZOrderEncoder::to_bytes(z_lo, kb_lo);
    ZOrderEncoder::to_bytes(z_hi, kb_hi);

    int32_t left = locate_leaf(z_lo);
    int32_t right = locate_leaf(z_hi);
    if (left < 0 || right < 0) return result;
    if (left > right) std::swap(left, right);

    std::vector<DataPoint*> candidates;

    auto collect_leaf_slots_range = [&](int32_t leaf_idx,
                                        uint64_t z_min, uint64_t z_max) {
        const GPLLeafNode& leaf = leaf_nodes_[leaf_idx];
        for (int32_t p = 0; p < leaf.slot_count; ++p) {
            if (!leaf.occupied[p]) continue;
            DataPoint* dp = leaf.data_slots[p];
            if (dp && dp->z_value >= z_min && dp->z_value <= z_max) {
                candidates.push_back(dp);
            }
        }
    };

    auto collect_leaf_slots_all = [&](int32_t leaf_idx) {
        const GPLLeafNode& leaf = leaf_nodes_[leaf_idx];
        for (int32_t p = 0; p < leaf.slot_count; ++p) {
            if (leaf.occupied[p] && leaf.data_slots[p]) {
                candidates.push_back(leaf.data_slots[p]);
            }
        }
    };

    if (left == right) {
        collect_leaf_slots_range(left, z_lo, z_hi);
        const ARTTree* art = art_trees_[left].get();
        if (art && !art->empty()) {
            auto v = art->range_search(kb_lo, kb_hi);
            for (auto* p : v) {
                if (p->z_value >= z_lo && p->z_value <= z_hi) candidates.push_back(p);
            }
        }
    } else {
        collect_leaf_slots_range(left, z_lo, UINT64_MAX);
        {
            const ARTTree* art = art_trees_[left].get();
            if (art && !art->empty()) {
                uint8_t kb_max[KEY_BYTES];
                ZOrderEncoder::to_bytes(UINT64_MAX, kb_max);
                auto v = art->range_search(kb_lo, kb_max);
                for (auto* p : v) {
                    if (p->z_value >= z_lo) candidates.push_back(p);
                }
            }
        }
        for (int32_t i = left + 1; i < right; ++i) {
            collect_leaf_slots_all(i);
            const ARTTree* art = art_trees_[i].get();
            if (art && !art->empty()) {
                auto v = art->collect_all();
                for (auto* p : v) candidates.push_back(p);
            }
        }
        collect_leaf_slots_range(right, 0, z_hi);
        {
            const ARTTree* art = art_trees_[right].get();
            if (art && !art->empty()) {
                uint8_t kb_min[KEY_BYTES];
                ZOrderEncoder::to_bytes(0, kb_min);
                auto v = art->range_search(kb_min, kb_hi);
                for (auto* p : v) {
                    if (p->z_value <= z_hi) candidates.push_back(p);
                }
            }
        }
    }

    for (DataPoint* dp : candidates) {
        bool in_rect = true;
        for (int32_t d = 0; d < config_.dim_count; ++d) {
            if (dp->dimensions[d] < low[d] || dp->dimensions[d] > high[d]) {
                in_rect = false;
                break;
            }
        }
        if (in_rect) result.push_back(dp);
    }
    return result;
}

size_t SULPlainIndex::learning_layer_filled() const {
    size_t total = 0;
    for (const auto& leaf : leaf_nodes_) total += leaf.filled_count;
    return total;
}

size_t SULPlainIndex::art_layer_points() const {
    size_t total = 0;
    for (const auto& a : art_trees_) total += a->leaf_count();
    return total;
}

size_t SULPlainIndex::art_total_inner_nodes() const {
    size_t total = 0;
    for (const auto& a : art_trees_) total += a->node_count();
    return total;
}

}
