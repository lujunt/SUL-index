#include "sul/gpl_builder.h"

#include <algorithm>
#include <cmath>

namespace sul {

GPLBuilder::GPLBuilder(int32_t error_bound) : error_bound_(error_bound) {
    if (error_bound_ < 1) error_bound_ = 1;
}

void GPLBuilder::fit_least_squares(const std::vector<uint64_t>& keys,
                                   int32_t start, int32_t end,
                                   double y_scale,
                                   double& slope, double& intercept) {
    int32_t len = end - start + 1;
    if (len <= 1) {
        slope = 0.0;
        intercept = 0.0;
        return;
    }
    double sx = 0.0, sy = 0.0, sxy = 0.0, sxx = 0.0;
    for (int32_t t = 0; t < len; ++t) {
        double x = static_cast<double>(keys[start + t]);
        double y = static_cast<double>(t) * y_scale;
        sx += x;
        sy += y;
        sxy += x * y;
        sxx += x * x;
    }
    double n_d = static_cast<double>(len);
    double denom = n_d * sxx - sx * sx;
    if (std::abs(denom) < 1e-9) {
        slope = 0.0;
        intercept = (sy) / n_d;
    } else {
        slope = (n_d * sxy - sx * sy) / denom;
        intercept = (sy - slope * sx) / n_d;
    }
}

std::vector<Segment> GPLBuilder::gpl_partition(const std::vector<uint64_t>& keys) {
    std::vector<Segment> segments;
    int32_t n = static_cast<int32_t>(keys.size());
    int32_t i = 0;

    while (i < n) {
        Segment seg;
        seg.start = i;

        int32_t end_idx;
        if (i + error_bound_ >= n - 1) {
            end_idx = n - 1;
        } else {
            double k0 = static_cast<double>(keys[i]);
            double k1 = static_cast<double>(keys[i + error_bound_]);
            double denom_k = k1 - k0;
            if (denom_k < 1.0) denom_k = 1.0;
            double slope_init = static_cast<double>(error_bound_) / denom_k;
            double intercept_init = -slope_init * k0;

            int32_t j = i + error_bound_ + 1;
            while (j < n) {
                double predicted = slope_init * static_cast<double>(keys[j]) + intercept_init;
                double actual = static_cast<double>(j - i);
                if (std::abs(predicted - actual) > static_cast<double>(error_bound_)) {
                    break;
                }
                ++j;
            }
            end_idx = j - 1;
        }
        seg.end = end_idx;

        fit_least_squares(keys, seg.start, seg.end, 1.0, seg.slope, seg.intercept);

        segments.push_back(seg);
        i = end_idx + 1;
    }

    return segments;
}

int32_t GPLBuilder::find_leaf_for_key(const std::vector<GPLLeafNode>& leaves,
                                      uint64_t key) {
    int32_t lo = 0;
    int32_t hi = static_cast<int32_t>(leaves.size()) - 1;
    int32_t ans = 0;
    while (lo <= hi) {
        int32_t mid = (lo + hi) >> 1;
        if (leaves[mid].key <= key) {
            ans = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return ans;
}

GPLBuildResult GPLBuilder::build(const std::vector<uint64_t>& sorted_keys,
                                 const std::vector<DataPoint*>& sorted_points,
                                 int32_t max_layers) {
    GPLBuildResult result;
    int32_t n = static_cast<int32_t>(sorted_keys.size());
    if (n == 0) return result;

    std::vector<Segment> leaf_segs = gpl_partition(sorted_keys);

    result.leaf_nodes.reserve(leaf_segs.size());
    for (const auto& seg : leaf_segs) {
        GPLLeafNode leaf;
        int32_t len = seg.end - seg.start + 1;
        int32_t slot_count = std::max(len * 2 + 2 * error_bound_, 8);

        double slope_fit, intercept_fit;
        fit_least_squares(sorted_keys, seg.start, seg.end, 2.0, slope_fit, intercept_fit);

        leaf.key = sorted_keys[seg.start];
        leaf.slope = slope_fit;
        leaf.intercept = intercept_fit;
        leaf.slot_count = slot_count;
        leaf.filled_count = 0;
        leaf.data_slots.assign(slot_count, nullptr);
        leaf.occupied.assign(slot_count, 0);
        leaf.art_root = nullptr;
        leaf.seg_start = seg.start;
        leaf.seg_end = seg.end;
        result.leaf_nodes.push_back(std::move(leaf));
    }

    for (int32_t i = 0; i < n; ++i) {
        DataPoint* dp = sorted_points[i];
        uint64_t z = sorted_keys[i];
        int32_t leaf_idx = find_leaf_for_key(result.leaf_nodes, z);
        GPLLeafNode& leaf = result.leaf_nodes[leaf_idx];

        double predicted = leaf.slope * static_cast<double>(z) + leaf.intercept;
        int32_t pos = static_cast<int32_t>(std::floor(predicted));
        if (pos < 0) pos = 0;
        if (pos >= leaf.slot_count) pos = leaf.slot_count - 1;

        if (!leaf.occupied[pos]) {
            leaf.data_slots[pos] = dp;
            leaf.occupied[pos] = 1;
            ++leaf.filled_count;
        } else {
            result.conflict_set.push_back(dp);
            result.conflict_leaf_idx.push_back(leaf_idx);
        }
    }

    std::vector<uint64_t> child_keys;
    child_keys.reserve(result.leaf_nodes.size());
    for (const auto& leaf : result.leaf_nodes) child_keys.push_back(leaf.key);

    int32_t layer_count = 0;
    while (child_keys.size() > 1 && layer_count < max_layers) {
        std::vector<Segment> segs = gpl_partition(child_keys);

        std::vector<GPLInnerNode> parents;
        parents.reserve(segs.size());
        for (const auto& seg : segs) {
            GPLInnerNode parent;
            parent.key = child_keys[seg.start];
            int32_t len = seg.end - seg.start + 1;
            double slope_fit, intercept_fit;
            fit_least_squares(child_keys, seg.start, seg.end, 1.0, slope_fit, intercept_fit);
            parent.slope = slope_fit;
            parent.intercept = intercept_fit;
            parent.child_start = seg.start;
            parent.child_count = len;
            parents.push_back(parent);
        }

        result.inner_layers.insert(result.inner_layers.begin(), parents);

        std::vector<uint64_t> next_keys;
        next_keys.reserve(parents.size());
        for (const auto& p : parents) next_keys.push_back(p.key);
        child_keys = std::move(next_keys);

        ++layer_count;
        if (child_keys.size() <= 1) break;
    }

    return result;
}

}
