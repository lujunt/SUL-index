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

// 范围查询的ART耗时统计：用于模拟"多ART并行查找"的延迟
//   art_total_us：所有ART操作耗时之和（顺序执行的实际开销）
//   art_max_us  ：单次ART操作的最长耗时（并行执行下的关键路径）
//   art_count   ：本次range_query执行的ART操作次数
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

    // 单点插入：定位GPL叶 → 预测槽 → 槽空入学习层，否则入ART
    // 返回值标识落点层级，便于上层统计
    InsertResult insert(const int32_t* coords);

    DataPoint* point_query(const int32_t* coords) const;

    // 范围查询。可选参数stats：传入非空指针时会记录ART各次搜索耗时，
    // 供上层根据 max_us 模拟"多ART并行"下的查询延迟。
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

    // 所有ART树扩容事件累计计数
    size_t art_total_expand_4_to_16()   const;
    size_t art_total_expand_16_to_48()  const;
    size_t art_total_expand_48_to_256() const;

    const IndexConfig& config() const { return config_; }

    // 加密镜像构建用：只读访问内部结构
    const std::vector<DataPoint>& all_points() const { return all_points_; }
    const std::vector<std::vector<GPLInnerNode>>& inner_layers() const { return inner_layers_; }
    const std::vector<GPLLeafNode>& leaf_nodes() const { return leaf_nodes_; }
    const std::vector<std::unique_ptr<ARTTree>>& art_trees() const { return art_trees_; }

    // 加密版查询时需要使用 locate_leaf 与槽位预测（同源逻辑，避免重复实现）
    int32_t locate_leaf_for_cipher(uint64_t z_value) const { return locate_leaf(z_value); }

    // 序列化用：注入骨架（结构 + 线性模型字段），不重建 DataPoint/ART 数据
    //   - inner_in: 每层 inner 节点
    //   - leaf_in:  leaf 节点结构字段（key/slope/intercept/slot_count/filled/occupied
    //               /seg_start/seg_end），data_slots 在密文版查询不会用到
    // 调用后索引进入 "loaded skeleton 模式"，仅支持密文版查询走 SQQP 路径
    void restore_skeleton(std::vector<std::vector<GPLInnerNode>> inner_in,
                          std::vector<GPLLeafNode>               leaf_in);

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
