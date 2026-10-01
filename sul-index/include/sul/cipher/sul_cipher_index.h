#pragma once

#include "sul/cipher/cipher_types.h"
#include "sul/cipher/crypto_context.h"
#include "sul/sul_index.h"
#include "sul/types.h"
#include "sul/z_order.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace sul::cipher {

// Forward declaration for the encrypted ART.
class EncARTTree;

// Main SUL-cipher-index class.
//
// Design:
//   1) plain_ is the DAP private-key view and reuses the plaintext build logic.
//   2) Each structure has an encrypted mirror.
//   3) SQQP, SARTQ, and SHRQ locate records in the encrypted mirror.
//   4) OSM, SIC, and SPI live under agreements/agreements and are simulated in one process.
//
// The DSP holds encrypted parameters and the DAP holds the Paillier private key. This
// implementation combines both roles and marks their logic with DSP/DAP comments.
class SULCipherIndex {
public:
    SULCipherIndex(const IndexConfig& config, CryptoContext& crypto);
    ~SULCipherIndex();

    SULCipherIndex(const SULCipherIndex&) = delete;
    SULCipherIndex& operator=(const SULCipherIndex&) = delete;

    void bulk_load(std::vector<DataPoint> points);

    // Query timing breakdown in microseconds. For range queries,
    // art_us = collect_us + spi_setup_us + spi_filter_us. Point queries populate only
    // learning_us and art_us.
    struct QueryStats {
        double learning_us = 0.0;  // GPL/SQQP plus learning-leaf slot scan.
        double art_us      = 0.0;  // Total ART/SARTQ, candidate merge, and SPI filtering.
        // Range-query-only breakdown; point-query fields remain zero.
        double collect_us    = 0.0; // Candidate collection, including encrypted bbox/SIC pruning.
        double spi_setup_us  = 0.0; // Encrypt query bounds inside ART buckets.
        double spi_filter_us = 0.0; // Run SPI over each candidate inside ART buckets.
        size_t candidates_total = 0; // Candidates before SPI evaluation.
        size_t candidates_kept  = 0; // Candidates accepted by SPI (the result size).
        // Layer 1 OUTSIDE-pruning statistics.
        size_t middle_leaves_total  = 0; // Middle leaves in a cross-leaf query.
        size_t middle_leaves_pruned = 0; // Middle leaves rejected by the coordinate bbox.
        bool   hit_learning = false;
        bool   hit_art      = false;
    };

    // Secure point query; return nullptr when no encrypted point matches.
    EncDataPoint* point_query(const int32_t* coords);

    // Point query with optional timing; a null stats pointer is equivalent to point_query.
    EncDataPoint* point_query_with_stats(const int32_t* coords,
                                         QueryStats* stats);

    // Secure range query returning all matching encrypted points.
    std::vector<EncDataPoint*> range_query(const int32_t* low,
                                           const int32_t* high);

    // Range query with a timing breakdown.
    std::vector<EncDataPoint*> range_query_with_stats(const int32_t* low,
                                                      const int32_t* high,
                                                      QueryStats* stats);

    // Reuse candidate collection and encrypted bbox pruning, but skip SPI and hit statistics.
    size_t count_range_candidates(const int32_t* low, const int32_t* high,
                                  QueryStats* stats = nullptr);

    // Secure insertion: locate with SQQP, then insert into the learning or ART layer.
    InsertResult insert(const int32_t* coords);

    // Read-only statistics. Loaded indexes have no plaintext DataPoint objects, so use
    // the encrypted pool size instead.
    size_t total_points()           const {
        return is_loaded_ ? enc_points_.size() : plain_.total_points();
    }
    size_t leaf_count()             const { return enc_leaf_nodes_.size(); }
    size_t inner_layer_count()      const { return enc_inner_layers_.size(); }
    size_t learning_layer_filled()  const { return plain_.learning_layer_filled(); }
    size_t art_layer_points()       const { return plain_.art_layer_points(); }

    const IndexConfig& config() const { return config_; }

    // Expose the plaintext index only for debugging and ground-truth validation.
    const SULPlainIndex& plain() const { return plain_; }

    // ========================================================================
    // Serialization and deserialization.
    // ========================================================================
    // Persist IndexConfig, the Paillier key pair, the GPL skeleton, and all ciphertexts.
    void save_to_file(const std::string& path) const;

    // Load (CryptoContext, SULCipherIndex) from a file. Loaded indexes are query-only;
    // insert returns Failed.
    static std::pair<std::unique_ptr<CryptoContext>,
                     std::unique_ptr<SULCipherIndex>>
        load_from_file(const std::string& path);

    bool is_loaded() const { return is_loaded_; }

private:
    std::vector<EncDataPoint*> range_query_impl(const int32_t* low, const int32_t* high,
                                               QueryStats* stats, bool filter_candidates);
    IndexConfig    config_;
    CryptoContext& crypto_;
    ZOrderEncoder  encoder_;

    SULPlainIndex plain_;

    std::vector<std::vector<EncGPLInnerNode>> enc_inner_layers_;
    std::vector<EncGPLLeafNode>               enc_leaf_nodes_;
    std::vector<std::unique_ptr<EncARTTree>>  enc_art_trees_;

    std::vector<std::unique_ptr<EncDataPoint>> enc_points_;
    std::vector<std::unique_ptr<EncDataPoint>> enc_inserted_points_;

    // DAP-only plaintext bbox for each leaf, used to update encrypted bboxes on insertion.
    // size = leaf_count × dim_count
    std::vector<std::vector<int32_t>> plain_bbox_lo_;
    std::vector<std::vector<int32_t>> plain_bbox_hi_;

    // True after deserialization; insertion is rejected because the skeleton has no DataPoints.
    bool is_loaded_ = false;

    void encrypt_data_point(const DataPoint& src, EncDataPoint& dst);
    void build_encrypted_mirror();

    // SQQP returns the target leaf index.
    int32_t sqqp(__uint128_t plain_v);

    // SARTQ securely searches encrypted key_bytes in the selected ART.
    EncDataPoint* sartq(int32_t art_tree_idx,
                        const std::vector<Ciphertext>& enc_key_bytes,
                        const uint8_t* plain_key_bytes);
};

// Encrypted ART.
class EncARTTree {
public:
    explicit EncARTTree(int32_t key_len, CryptoContext& crypto);
    ~EncARTTree();

    EncARTTree(const EncARTTree&) = delete;
    EncARTTree& operator=(const EncARTTree&) = delete;

    void insert(EncDataPoint* edp, const uint8_t* plain_key_bytes);

    // Secure point query returning a matched point or nullptr.
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

    // Serialization helpers.
    // Clear the tree and counters before deserialization.
    void clear();

    // Install a root subtree constructed by the deserializer.
    void set_root(void* root, size_t inner_count, size_t leaf_count) {
        root_ = root; inner_count_ = inner_count; leaf_count_ = leaf_count;
    }

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
