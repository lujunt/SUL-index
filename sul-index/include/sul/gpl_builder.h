#pragma once

#include "sul/types.h"

#include <vector>

namespace sul {

struct Segment {
    int32_t start;
    int32_t end;
    double slope;
    double intercept;
};

struct GPLBuildResult {
    std::vector<std::vector<GPLInnerNode>> inner_layers;
    std::vector<GPLLeafNode> leaf_nodes;
    std::vector<DataPoint*> conflict_set;
    std::vector<int32_t> conflict_leaf_idx;
};

class GPLBuilder {
public:
    explicit GPLBuilder(int32_t error_bound);

    GPLBuildResult build(const std::vector<__uint128_t>& sorted_keys,
                         const std::vector<DataPoint*>& sorted_points,
                         int32_t max_layers);

private:
    int32_t error_bound_;

    std::vector<Segment> gpl_partition(const std::vector<__uint128_t>& keys);

    static void fit_least_squares(const std::vector<__uint128_t>& keys,
                                  int32_t start, int32_t end,
                                  double y_scale,
                                  double& slope, double& intercept);

    static int32_t find_leaf_for_key(const std::vector<GPLLeafNode>& leaves,
                                     __uint128_t key);
};

}
