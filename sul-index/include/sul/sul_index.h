#pragma once

#include "sul/art_tree.h"
#include "sul/gpl_builder.h"
#include "sul/types.h"
#include "sul/z_order.h"

#include <memory>
#include <vector>

namespace sul {

class SULPlainIndex {
public:
    explicit SULPlainIndex(const IndexConfig& config);
    ~SULPlainIndex();

    SULPlainIndex(const SULPlainIndex&) = delete;
    SULPlainIndex& operator=(const SULPlainIndex&) = delete;

    void bulk_load(std::vector<DataPoint> points);

    DataPoint* point_query(const int32_t* coords) const;

    std::vector<DataPoint*> range_query(const int32_t* low,
                                        const int32_t* high) const;

    size_t total_points() const { return all_points_.size(); }
    size_t leaf_count() const { return leaf_nodes_.size(); }
    size_t inner_layer_count() const { return inner_layers_.size(); }
    size_t learning_layer_filled() const;
    size_t art_layer_points() const;
    size_t art_total_inner_nodes() const;

    const IndexConfig& config() const { return config_; }

private:
    IndexConfig config_;
    ZOrderEncoder encoder_;

    std::vector<DataPoint> all_points_;
    std::vector<std::vector<GPLInnerNode>> inner_layers_;
    std::vector<GPLLeafNode> leaf_nodes_;
    std::vector<std::unique_ptr<ARTTree>> art_trees_;

    int32_t locate_leaf(uint64_t z_value) const;
    DataPoint* search_leaf_for_zvalue(int32_t leaf_idx, uint64_t z_value) const;
};

}
