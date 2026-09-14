#include "sul/sul_index.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

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

} // namespace

void SULPlainIndex::bulk_load(std::vector<DataPoint> points) {
    all_points_ = std::move(points);

    // 为每个数据点计算z曲线值和ART key字节数组
    for (auto& dp : all_points_) {
        dp.z_value = encoder_.encode(dp.dimensions);           // 用于GPL层排序
        encoder_.encode_to_bytes(dp.dimensions, dp.key_bytes); // 用于ART层索引
    }

    // 按z值升序排列，保证GPL学习层的线性模型有意义
    std::sort(all_points_.begin(), all_points_.end(),
              [](const DataPoint& a, const DataPoint& b) {
                  return a.z_value < b.z_value;
              });

    std::vector<__uint128_t> sorted_keys;
    std::vector<DataPoint*> sorted_ptrs;
    sorted_keys.reserve(all_points_.size());
    sorted_ptrs.reserve(all_points_.size());
    for (auto& dp : all_points_) {
        sorted_keys.push_back(dp.z_value);
        sorted_ptrs.push_back(&dp);
    }

    // 构建多层GPL学习层
    GPLBuilder builder(config_.error_bound);
    GPLBuildResult res = builder.build(sorted_keys, sorted_ptrs, config_.max_layers);

    inner_layers_ = std::move(res.inner_layers);
    leaf_nodes_   = std::move(res.leaf_nodes);

    // 每个叶子节点挂载一棵ART树（无冲突点时为空树），key_len由维度决定
    const int32_t kl = config_.key_len();
    art_trees_.clear();
    art_trees_.reserve(leaf_nodes_.size());
    for (size_t i = 0; i < leaf_nodes_.size(); ++i) {
        art_trees_.emplace_back(std::make_unique<ARTTree>(kl));
    }

    // 将GPL层的冲突点插入对应叶子的ART树
    for (size_t k = 0; k < res.conflict_set.size(); ++k) {
        int32_t leaf_idx = res.conflict_leaf_idx[k];
        art_trees_[leaf_idx]->insert(res.conflict_set[k]);
    }

    // 回写ART根节点指针到叶子节点（供快速判断是否有冲突点）
    for (size_t i = 0; i < leaf_nodes_.size(); ++i) {
        leaf_nodes_[i].art_root = art_trees_[i]->root();
    }
}

// 用GPL多层模型定位z值所在的叶子节点下标
int32_t SULPlainIndex::locate_leaf(__uint128_t z_value) const {
    if (leaf_nodes_.empty()) return -1;

    if (inner_layers_.empty()) {
        // 无内层时直接对叶子数组二分查找
        int32_t lo = 0, hi = static_cast<int32_t>(leaf_nodes_.size()) - 1, ans = 0;
        while (lo <= hi) {
            int32_t mid = (lo + hi) >> 1;
            if (leaf_nodes_[mid].key <= z_value) { ans = mid; lo = mid + 1; }
            else hi = mid - 1;
        }
        return ans;
    }

    // 逐层用线性模型预测子节点位置
    int32_t cur_idx = 0;
    for (size_t layer = 0; layer < inner_layers_.size(); ++layer) {
        const GPLInnerNode& node = inner_layers_[layer][cur_idx];
        double predicted = node.slope * static_cast<double>(z_value) + node.intercept;
        int32_t rel = static_cast<int32_t>(std::floor(predicted));
        rel = clamp_int(rel, 0, node.child_count - 1);
        cur_idx = node.child_start + rel;

        if (layer + 1 < inner_layers_.size()) {
            cur_idx = clamp_int(cur_idx, 0, static_cast<int32_t>(inner_layers_[layer + 1].size()) - 1);
        } else {
            cur_idx = clamp_int(cur_idx, 0, static_cast<int32_t>(leaf_nodes_.size()) - 1);
        }
    }

    // 修正预测偏差，确保定位到正确叶子
    while (cur_idx > 0 && leaf_nodes_[cur_idx].key > z_value) --cur_idx;
    while (cur_idx + 1 < static_cast<int32_t>(leaf_nodes_.size())
           && leaf_nodes_[cur_idx + 1].key <= z_value) ++cur_idx;
    return cur_idx;
}

// 在GPL叶子的槽位范围[pos-ε, pos+ε]内搜索匹配z值的数据点
DataPoint* SULPlainIndex::search_leaf_for_zvalue(int32_t leaf_idx, __uint128_t z_value) const {
    const GPLLeafNode& leaf = leaf_nodes_[leaf_idx];
    double predicted = leaf.slope * static_cast<double>(z_value) + leaf.intercept;
    int32_t pos = clamp_int(static_cast<int32_t>(std::floor(predicted)), 0, leaf.slot_count - 1);
    int32_t lo  = clamp_int(pos - config_.error_bound, 0, leaf.slot_count - 1);
    int32_t hi  = clamp_int(pos + config_.error_bound, 0, leaf.slot_count - 1);

    for (int32_t p = lo; p <= hi; ++p) {
        if (leaf.occupied[p] && leaf.data_slots[p]
            && leaf.data_slots[p]->z_value == z_value) {
            return leaf.data_slots[p];
        }
    }
    return nullptr;
}

DataPoint* SULPlainIndex::point_query(const int32_t* coords) const {
    __uint128_t z = encoder_.encode(coords);
    int32_t leaf_idx = locate_leaf(z);
    if (leaf_idx < 0) return nullptr;

    // 先在GPL学习层槽位中查找
    DataPoint* hit = search_leaf_for_zvalue(leaf_idx, z);
    if (hit) {
        bool match = true;
        for (int32_t d = 0; d < config_.dim_count; ++d)
            if (hit->dimensions[d] != coords[d]) { match = false; break; }
        if (match) return hit;
    }

    // 学习层未命中则查询该叶子的ART树（冲突点）
    const ARTTree* art = art_trees_[leaf_idx].get();
    if (art && !art->empty()) {
        uint8_t kb[MAX_KEY_BYTES] = {};
        encoder_.encode_to_bytes(coords, kb);
        DataPoint* art_hit = art->search(kb);
        if (art_hit) {
            bool match = true;
            for (int32_t d = 0; d < config_.dim_count; ++d)
                if (art_hit->dimensions[d] != coords[d]) { match = false; break; }
            if (match) return art_hit;
        }
    }
    return nullptr;
}

std::vector<DataPoint*> SULPlainIndex::range_query(const int32_t* low,
                                                    const int32_t* high,
                                                    RangeQueryStats* stats) const {
    std::vector<DataPoint*> result;
    if (leaf_nodes_.empty()) return result;

    // 将查询边界编码为z值和ART key
    __uint128_t z_lo = encoder_.encode(low);
    __uint128_t z_hi = encoder_.encode(high);
    if (z_lo > z_hi) std::swap(z_lo, z_hi);

    const int32_t kl = config_.key_len();
    uint8_t kb_lo[MAX_KEY_BYTES] = {};
    uint8_t kb_hi[MAX_KEY_BYTES] = {};
    encoder_.encode_to_bytes(low,  kb_lo);
    encoder_.encode_to_bytes(high, kb_hi);

    int32_t left  = locate_leaf(z_lo);
    int32_t right = locate_leaf(z_hi);
    if (left < 0 || right < 0) return result;
    if (left > right) std::swap(left, right);

    std::vector<DataPoint*> candidates;

    // 收集叶子中所有有效槽位（不按 z 值过滤）
    // 注意：不能用 z 值范围预过滤候选——矩形 [low, high] 内的点其 z 值不一定
    // 落在 [z_lo, z_hi] 区间内（Z-curve 在位翻转处不连续），过滤会导致漏点。
    // 由后段的"维度坐标闭区间过滤"兜底正确性，与 cipher 端候选生成保持一致。
    auto collect_leaf_slots_all = [&](int32_t leaf_idx) {
        const GPLLeafNode& leaf = leaf_nodes_[leaf_idx];
        for (int32_t p = 0; p < leaf.slot_count; ++p)
            if (leaf.occupied[p] && leaf.data_slots[p]) candidates.push_back(leaf.data_slots[p]);
    };

    // ART操作计时helper：对每次ART搜索单独计时，累计到stats
    // 模拟"多ART并行查找"时，关键路径耗时 = 所有ART操作耗时中的最大值
    auto timed_art = [&](auto&& fn) {
        if (!stats) return fn();
        auto t0 = std::chrono::steady_clock::now();
        auto v = fn();
        auto t1 = std::chrono::steady_clock::now();
        double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        stats->art_total_us += us;
        if (us > stats->art_max_us) stats->art_max_us = us;
        ++stats->art_count;
        return v;
    };

    if (left == right) {
        // 查询范围在同一叶子内
        collect_leaf_slots_all(left);
        const ARTTree* art = art_trees_[left].get();
        if (art && !art->empty()) {
            auto v = timed_art([&]{ return art->range_search(kb_lo, kb_hi); });
            for (auto* p : v) candidates.push_back(p);
        }
    } else {
        // 左边界叶子：[z_lo, +∞)
        collect_leaf_slots_all(left);
        {
            const ARTTree* art = art_trees_[left].get();
            if (art && !art->empty()) {
                uint8_t kb_max[MAX_KEY_BYTES];
                std::memset(kb_max, 0xFF, static_cast<size_t>(kl));
                auto v = timed_art([&]{ return art->range_search(kb_lo, kb_max); });
                for (auto* p : v) candidates.push_back(p);
            }
        }
        // 中间叶子：完全收录
        for (int32_t i = left + 1; i < right; ++i) {
            collect_leaf_slots_all(i);
            const ARTTree* art = art_trees_[i].get();
            if (art && !art->empty()) {
                auto v = timed_art([&]{ return art->collect_all(); });
                for (auto* p : v) candidates.push_back(p);
            }
        }
        // 右边界叶子：(-∞, z_hi]
        collect_leaf_slots_all(right);
        {
            const ARTTree* art = art_trees_[right].get();
            if (art && !art->empty()) {
                uint8_t kb_min[MAX_KEY_BYTES];
                std::memset(kb_min, 0x00, static_cast<size_t>(kl));
                auto v = timed_art([&]{ return art->range_search(kb_min, kb_hi); });
                for (auto* p : v) candidates.push_back(p);
            }
        }
    }

    // 用原始维度坐标过滤候选集（去除z曲线范围查询的假阳性）
    for (DataPoint* dp : candidates) {
        bool in_rect = true;
        for (int32_t d = 0; d < config_.dim_count; ++d) {
            if (dp->dimensions[d] < low[d] || dp->dimensions[d] > high[d]) {
                in_rect = false; break;
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
    for (const auto& a : art_trees_) total += a->collect_all().size();
    return total;
}

size_t SULPlainIndex::art_total_inner_nodes() const {
    size_t total = 0;
    for (const auto& a : art_trees_) total += a->node_count();
    return total;
}

size_t SULPlainIndex::art_total_expand_4_to_16() const {
    size_t total = 0;
    for (const auto& a : art_trees_) total += a->expand_4_to_16();
    return total;
}

size_t SULPlainIndex::art_total_expand_16_to_48() const {
    size_t total = 0;
    for (const auto& a : art_trees_) total += a->expand_16_to_48();
    return total;
}

size_t SULPlainIndex::art_total_expand_48_to_256() const {
    size_t total = 0;
    for (const auto& a : art_trees_) total += a->expand_48_to_256();
    return total;
}

// 序列化反序列化：从外部注入 GPL 骨架
// 注：仅保留 inner/leaf 结构字段，data_slots 全部置空（密文版查询不会用到指针）
void SULPlainIndex::restore_skeleton(std::vector<std::vector<GPLInnerNode>> inner_in,
                                      std::vector<GPLLeafNode>               leaf_in) {
    all_points_.clear();
    inserted_points_.clear();
    inner_layers_ = std::move(inner_in);
    leaf_nodes_   = std::move(leaf_in);

    // 槽位指针在加载场景下不持有 plain DataPoint，统一置空
    for (auto& leaf : leaf_nodes_) {
        leaf.data_slots.assign(leaf.slot_count, nullptr);
        leaf.art_root = nullptr;
    }

    // 加载场景下 ART 树骨架未保存：每个叶子挂一个空 ARTTree，保证 art_trees_ 与 leaves 同长
    const int32_t kl = config_.key_len();
    art_trees_.clear();
    art_trees_.reserve(leaf_nodes_.size());
    for (size_t i = 0; i < leaf_nodes_.size(); ++i)
        art_trees_.emplace_back(std::make_unique<ARTTree>(kl));
}

// 单点插入：
// 1) 编码坐标 → z值 + key_bytes
// 2) 用GPL多层模型定位叶子
// 3) 拿叶子线性模型预测槽位pos
// 4) 槽位空 → 占位（学习层成功）
// 5) 槽位被占 → 进入对应叶子的ART树（自动处理扩容）
InsertResult SULPlainIndex::insert(const int32_t* coords) {
    if (leaf_nodes_.empty()) return InsertResult::Failed;

    // 用deque保证指针稳定：先构造一个新DataPoint放进容器，再用引用回写字段
    DataPoint stub{};
    stub.dim_count = config_.dim_count;
    for (int32_t d = 0; d < config_.dim_count; ++d) stub.dimensions[d] = coords[d];
    stub.orig_id  = static_cast<int32_t>(all_points_.size() + inserted_points_.size());
    stub.z_value  = encoder_.encode(coords);
    encoder_.encode_to_bytes(coords, stub.key_bytes);

    int32_t leaf_idx = locate_leaf(stub.z_value);
    if (leaf_idx < 0) return InsertResult::Failed;

    inserted_points_.push_back(stub);
    DataPoint* dp = &inserted_points_.back();

    GPLLeafNode& leaf = leaf_nodes_[leaf_idx];
    double predicted = leaf.slope * static_cast<double>(dp->z_value) + leaf.intercept;
    int32_t pos = clamp_int(static_cast<int32_t>(std::floor(predicted)),
                            0, leaf.slot_count - 1);

    if (!leaf.occupied[pos]) {
        // 学习层路径：直接占用空槽
        leaf.data_slots[pos] = dp;
        leaf.occupied[pos]   = 1;
        ++leaf.filled_count;
        return InsertResult::LearningLayer;
    }

    // ART层路径：槽位冲突，下放到该叶子的ART树
    art_trees_[leaf_idx]->insert(dp);
    leaf.art_root = art_trees_[leaf_idx]->root();
    return InsertResult::ARTLayer;
}

}
