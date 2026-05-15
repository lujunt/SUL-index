#pragma once

#include "sul/art_tree.h"
#include "sul/gpl_builder.h"
#include "sul/types.h"
#include "sul/z_order.h"

#include <deque>
#include <memory>
#include <vector>

namespace sul {

// 插入结果：标识新点最终落在哪一层
enum class InsertResult : int32_t {
    Failed        = 0,  // 索引为空或定位失败
    LearningLayer = 1,  // 预测槽位为空，直接进入学习层
    ARTLayer      = 2   // 预测槽位已被占用，回退到ART层
};

class SULPlainIndex {
public:
    explicit SULPlainIndex(const IndexConfig& config);
    ~SULPlainIndex();

    SULPlainIndex(const SULPlainIndex&) = delete;
    SULPlainIndex& operator=(const SULPlainIndex&) = delete;

    void bulk_load(std::vector<DataPoint> points);

    // 单点插入：定位GPL叶 → 预测槽 → 槽空入学习层，否则入ART
    // 返回值标识落点层级，便于上层统计
    InsertResult insert(const int32_t* coords);

    DataPoint* point_query(const int32_t* coords) const;

    std::vector<DataPoint*> range_query(const int32_t* low,
                                        const int32_t* high) const;

    size_t total_points() const { return all_points_.size() + inserted_points_.size(); }
    size_t inserted_count() const { return inserted_points_.size(); }
    size_t leaf_count() const { return leaf_nodes_.size(); }
    size_t inner_layer_count() const { return inner_layers_.size(); }
    size_t learning_layer_filled() const;
    size_t art_layer_points() const;
    size_t art_total_inner_nodes() const;

    // 所有ART树扩容事件累计计数
    size_t art_total_expand_4_to_16()   const;
    size_t art_total_expand_16_to_48()  const;
    size_t art_total_expand_48_to_256() const;

    const IndexConfig& config() const { return config_; }

private:
    IndexConfig config_;
    ZOrderEncoder encoder_;

    std::vector<DataPoint> all_points_;
    // 插入后新增的点：用deque保证push_back不失效已存放在槽位/ART的指针
    std::deque<DataPoint> inserted_points_;

    std::vector<std::vector<GPLInnerNode>> inner_layers_;
    std::vector<GPLLeafNode> leaf_nodes_;
    std::vector<std::unique_ptr<ARTTree>> art_trees_;

    int32_t locate_leaf(uint64_t z_value) const;
    DataPoint* search_leaf_for_zvalue(int32_t leaf_idx, uint64_t z_value) const;
};

}
