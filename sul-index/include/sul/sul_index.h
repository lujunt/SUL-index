#pragma once

#include "sul/art_tree.h"
#include "sul/gpl_builder.h"
#include "sul/types.h"
#include "sul/z_order.h"

#include <deque>
#include <memory>
#include <vector>

namespace sul {

// Identifies the layer that received an inserted point.
enum class InsertResult : int32_t {
    Failed        = 0,  // The index is empty or lookup failed.
    LearningLayer = 1,  // The predicted slot was empty.
    ARTLayer      = 2   // The predicted slot was occupied, so the point fell back to ART.
};

// ART timing for range queries, used to model parallel searches across multiple trees.
// art_total_us is the sequential sum, art_max_us is the parallel critical path,
// and art_count is the number of ART operations in this range query.
struct RangeQueryStats {
    double  art_total_us = 0.0;
    double  art_max_us   = 0.0;
    int32_t art_count    = 0;
};

class SULPlainIndex {
public:
    explicit SULPlainIndex(const IndexConfig& config);
    ~SULPlainIndex();

    SULPlainIndex(const SULPlainIndex&) = delete;
    SULPlainIndex& operator=(const SULPlainIndex&) = delete;

    void bulk_load(std::vector<DataPoint> points);

    // Insert one point: locate the GPL leaf, predict a slot, and use ART on collision.
    // The return value identifies the destination layer for statistics.
    InsertResult insert(const int32_t* coords);

    DataPoint* point_query(const int32_t* coords) const;

    // Range query. A non-null stats pointer records individual ART timings so callers
    // can use max_us to model parallel searches across multiple ARTs.
    std::vector<DataPoint*> range_query(const int32_t* low,
                                        const int32_t* high,
                                        RangeQueryStats* stats = nullptr) const;

    size_t total_points() const { return all_points_.size() + inserted_points_.size(); }
    size_t inserted_count() const { return inserted_points_.size(); }
    size_t leaf_count() const { return leaf_nodes_.size(); }
    size_t inner_layer_count() const { return inner_layers_.size(); }
    size_t learning_layer_filled() const;
    size_t art_layer_points() const;
    size_t art_total_inner_nodes() const;

    // Cumulative growth-event count across all ARTs.
    size_t art_total_expand_4_to_16()   const;
    size_t art_total_expand_16_to_48()  const;
    size_t art_total_expand_48_to_256() const;

    const IndexConfig& config() const { return config_; }

    // Read-only access used to build the encrypted mirror.
    const std::vector<DataPoint>& all_points() const { return all_points_; }
    const std::vector<std::vector<GPLInnerNode>>& inner_layers() const { return inner_layers_; }
    const std::vector<GPLLeafNode>& leaf_nodes() const { return leaf_nodes_; }
    const std::vector<std::unique_ptr<ARTTree>>& art_trees() const { return art_trees_; }

    // Encrypted queries reuse leaf lookup and slot prediction to avoid duplicate logic.
    int32_t locate_leaf_for_cipher(__uint128_t z_value) const { return locate_leaf(z_value); }

    // Inject a serialized skeleton (structure and linear-model fields) without rebuilding
    // DataPoint or ART data. inner_in contains each inner layer; leaf_in contains leaf
    // metadata. Encrypted queries do not use data_slots. The resulting skeleton supports
    // only the encrypted SQQP query path.
    void restore_skeleton(std::vector<std::vector<GPLInnerNode>> inner_in,
                          std::vector<GPLLeafNode>               leaf_in);

private:
    IndexConfig config_;
    ZOrderEncoder encoder_;

    std::vector<DataPoint> all_points_;
    // Post-build points use deque so push_back does not invalidate pointers stored in slots or ARTs.
    std::deque<DataPoint> inserted_points_;

    std::vector<std::vector<GPLInnerNode>> inner_layers_;
    std::vector<GPLLeafNode> leaf_nodes_;
    std::vector<std::unique_ptr<ARTTree>> art_trees_;

    int32_t locate_leaf(__uint128_t z_value) const;
    DataPoint* search_leaf_for_zvalue(int32_t leaf_idx, __uint128_t z_value) const;
};

}
