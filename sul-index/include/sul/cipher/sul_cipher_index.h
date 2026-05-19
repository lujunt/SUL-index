#pragma once

#include "sul/cipher/cipher_types.h"
#include "sul/cipher/crypto_context.h"
#include "sul/sul_index.h"
#include "sul/types.h"
#include "sul/z_order.h"

#include <memory>
#include <vector>

namespace sul::cipher {

// 前向声明：加密 ART 树
class EncARTTree;

// SUL-cipher-index 主类
//
// 设计要点
//   1) 内部持有一份明文索引 plain_ 作为 DAP 私钥侧视图，构建逻辑完全复用明文版本
//   2) 同时为每个数据结构维护一份加密镜像（enc_inner_layers_/enc_leaf_nodes_/enc_art_trees_）
//   3) 查询时通过 SQQP/SARTQ/SHRQ 协议在加密镜像上完成定位，最终返回 EncDataPoint*
//   4) OSM/SIC/SPI 协议位于 sul-index/agreements/agreements/，在本实现中以单线程逻辑模拟
//
// 双方角色：DSP 持有所有加密参数，DAP 持有 Paillier 私钥
// 本实现将两者合并为同一进程，以注释 // DSP / // DAP 标记每段逻辑归属
class SULCipherIndex {
public:
    SULCipherIndex(const IndexConfig& config, CryptoContext& crypto);
    ~SULCipherIndex();

    SULCipherIndex(const SULCipherIndex&) = delete;
    SULCipherIndex& operator=(const SULCipherIndex&) = delete;

    void bulk_load(std::vector<DataPoint> points);

    // 安全点查询：返回加密数据点指针，not-found 返回 nullptr
    EncDataPoint* point_query(const int32_t* coords);

    // 安全范围查询：返回所有命中加密数据点
    std::vector<EncDataPoint*> range_query(const int32_t* low,
                                           const int32_t* high);

    // 安全插入：先 SQQP 定位，再走学习层或 ART 层
    InsertResult insert(const int32_t* coords);

    // 只读统计
    size_t total_points()           const { return plain_.total_points(); }
    size_t leaf_count()             const { return enc_leaf_nodes_.size(); }
    size_t inner_layer_count()      const { return enc_inner_layers_.size(); }
    size_t learning_layer_filled()  const { return plain_.learning_layer_filled(); }
    size_t art_layer_points()       const { return plain_.art_layer_points(); }

    const IndexConfig& config() const { return config_; }

    // 暴露明文索引（仅供调试 / 真值验证用，生产环境应隐藏）
    const SULPlainIndex& plain() const { return plain_; }

private:
    IndexConfig    config_;
    CryptoContext& crypto_;
    ZOrderEncoder  encoder_;

    SULPlainIndex plain_;

    std::vector<std::vector<EncGPLInnerNode>> enc_inner_layers_;
    std::vector<EncGPLLeafNode>               enc_leaf_nodes_;
    std::vector<std::unique_ptr<EncARTTree>>  enc_art_trees_;

    std::vector<std::unique_ptr<EncDataPoint>> enc_points_;
    std::vector<std::unique_ptr<EncDataPoint>> enc_inserted_points_;

    void encrypt_data_point(const DataPoint& src, EncDataPoint& dst);
    void build_encrypted_mirror();

    // SQQP：返回目标叶子下标 leaf_idx
    int32_t sqqp(uint64_t plain_v);

    // SARTQ：在指定 ART 树上对加密 key_bytes 做安全点查询
    EncDataPoint* sartq(int32_t art_tree_idx,
                        const std::vector<Ciphertext>& enc_key_bytes,
                        const uint8_t* plain_key_bytes);
};

// 加密 ART 树
class EncARTTree {
public:
    explicit EncARTTree(int32_t key_len, CryptoContext& crypto);
    ~EncARTTree();

    EncARTTree(const EncARTTree&) = delete;
    EncARTTree& operator=(const EncARTTree&) = delete;

    void insert(EncDataPoint* edp, const uint8_t* plain_key_bytes);

    // 安全点查询：返回命中数据点或 nullptr
    EncDataPoint* search(const std::vector<Ciphertext>& enc_key_bytes,
                         const uint8_t* plain_key_bytes,
                         ophelib::PaillierFast& paillier);

    std::vector<EncDataPoint*> range_search(const uint8_t* low,
                                            const uint8_t* high) const;

    std::vector<EncDataPoint*> collect_all() const;

    bool   empty()      const { return root_ == nullptr; }
    size_t node_count() const { return inner_count_; }
    size_t leaf_count() const { return leaf_count_; }
    void*  root()       const { return root_; }

private:
    void*   root_;
    size_t  inner_count_;
    size_t  leaf_count_;
    int32_t key_len_;
    CryptoContext& crypto_;

    void* insert_inner(void* node, EncDataPoint* edp,
                       const uint8_t* plain_key_bytes, int32_t depth);
    void* expand_node4_to_node16(EncARTNode4*);
    void* expand_node16_to_node48(EncARTNode16*);
    void* expand_node48_to_node256(EncARTNode48*);
    void  destroy(void* node, int32_t depth);

    void collect_subtree(void* node, int32_t depth,
                         std::vector<EncDataPoint*>& out) const;

    void range_collect(void* node, int32_t depth,
                       const uint8_t* low, const uint8_t* high,
                       bool tight_low, bool tight_high,
                       std::vector<EncDataPoint*>& out) const;
};

} // namespace sul::cipher
