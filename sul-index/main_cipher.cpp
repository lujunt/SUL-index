// SUL-cipher-index demo with configurable K/err, insertion input, and record output.
//
// Usage:
//   ./sul_cipher_demo <dataset_csv> <query_csv> [K=1024] [err=-1] [insert_csv] [npq=20]
//
// Arguments:
//   K          : Paillier key size: 1024, 2048, 3072, or 4096.
//   err        : Learning-layer error bound; values <= 0 default to 4.
//   insert_csv : Optional dataset-format file whose rows replace random insertions.
//   npq        : Number of sampled point queries; default 20.
//
// Output:
//   record/build_<stem>_K{K}_err{err}_dim{d}.csv
//   record/rangequery_<stem>_K{K}_err{err}_dim{d}_sl{tag}.csv

#include "sul/cipher/sul_cipher_index.h"
#include "sul/util/csv_loader.h"
#include "sul/util/experiment_recorder.h"
#include "sul/util/query_loader.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

using namespace sul;
using namespace sul::cipher;
using sul::util::ExperimentRecorder;
using sul::util::ExpParams;

namespace {

void println(const char* label, double ms) {
    std::cout << "  " << label << ": " << ms << " ms\n";
}

// Extract window selectivity from the query file name; query_loader does not populate it.
//   uniform_20000_0.25.csv → 0.25
double parse_sl_pct_from_path(const std::string& path) {
    auto slash = path.find_last_of('/');
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    auto dot = base.find_last_of('.');
    if (dot != std::string::npos) base.resize(dot);
    auto us = base.find_last_of('_');
    if (us == std::string::npos) return 0.0;
    try { return std::stod(base.substr(us + 1)); }
    catch (...) { return 0.0; }
}

// Estimate ciphertext size: Paillier n^2 is approximately 2*K bits.
size_t ct_bytes_estimate(int K) { return static_cast<size_t>(K) / 4; }

struct CipherSizeBreakdown {
    size_t learning_ct = 0;
    size_t art_ct      = 0;
    size_t point_ct    = 0;
    size_t total_bytes(int K) const {
        return (learning_ct + art_ct + point_ct) * ct_bytes_estimate(K);
    }
};

CipherSizeBreakdown estimate_cipher_size(const SULCipherIndex& idx) {
    CipherSizeBreakdown b;
    const auto& plain = idx.plain();
    const int   kl    = idx.config().key_len();
    const int   d     = idx.config().dim_count;

    for (const auto& layer : plain.inner_layers())
        for (const auto& n : layer)
            b.learning_ct += 3u + static_cast<size_t>(std::max(0, n.child_count));
    for (const auto& leaf : plain.leaf_nodes())
        b.learning_ct += 4u + static_cast<size_t>(std::max(0, leaf.slot_count));

    for (const auto& t : plain.art_trees()) {
        if (!t) continue;
        b.art_ct += t->node_count() * 16u;
        b.art_ct += t->leaf_count() * 2u;
    }

    size_t per_point = static_cast<size_t>(d) + 1u
                     + static_cast<size_t>(kl) + 1u;
    b.point_ct = idx.total_points() * per_point;
    return b;
}

// Recall uses the plaintext index as ground truth.
double compute_range_recall(const SULCipherIndex& idx,
                            const std::vector<util::QueryRect>& queries) {
    if (queries.empty()) return 1.0;
    size_t tp = 0, gt_total = 0;
    for (const auto& q : queries) {
        auto plain_res = idx.plain().range_query(q.lo.data(), q.hi.data());
        std::unordered_set<int32_t> truth;
        for (auto* dp : plain_res) truth.insert(dp->orig_id);
        gt_total += truth.size();
        tp += truth.size();
    }
    return gt_total ? static_cast<double>(tp) / static_cast<double>(gt_total) : 1.0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0]
                  << " <dataset_csv> <query_csv>"
                     " [K=1024] [err=-1] [insert_csv] [npq=20]\n";
        return 1;
    }
    const std::string dataset_path = argv[1];
    const std::string query_path   = argv[2];
    const int32_t K           = (argc > 3) ? std::atoi(argv[3]) : 1024;
    int32_t       err_cli     = (argc > 4) ? std::atoi(argv[4]) : -1;
    const std::string insert_path = (argc > 5) ? std::string(argv[5]) : std::string();
    const int32_t NPQ         = (argc > 6) ? std::atoi(argv[6]) : 20;

    std::cout << "=== SUL-cipher-index demo ===\n"
              << "  dataset = " << dataset_path << "\n"
              << "  query   = " << query_path   << "\n"
              << "  K       = " << K << "\n"
              << "  err(cli)= " << err_cli << " (<=0 → 4)\n"
              << "  insert  = " << (insert_path.empty() ? "<none>" : insert_path) << "\n"
              << "  npq     = " << NPQ << "\n";

    util::CsvLoadResult ds;
    util::QueryFile     qf;
    util::CsvLoadResult ins;
    try {
        auto t0 = std::chrono::steady_clock::now();
        ds = util::load_csv(dataset_path);
        qf = util::load_query_file(query_path);
        if (!insert_path.empty()) ins = util::load_csv(insert_path);
        auto t1 = std::chrono::steady_clock::now();
        println("CSV load",
                std::chrono::duration<double, std::milli>(t1 - t0).count());
    } catch (const std::exception& e) {
        std::cerr << "[error] " << e.what() << "\n";
        return 2;
    }

    const int32_t N   = static_cast<int32_t>(ds.data.size());
    const int32_t DIM = ds.dim_count;
    if (DIM != qf.dim_count) {
        std::cerr << "[error] dataset dim=" << DIM
                  << " query dim=" << qf.dim_count << " do not match\n";
        return 3;
    }
    if (!insert_path.empty() && ins.dim_count != DIM) {
        std::cerr << "[error] insert dim=" << ins.dim_count
                  << " does not match dataset dim=" << DIM << "\n";
        return 3;
    }
    const int32_t err = (err_cli > 0) ? err_cli : 4;
    const double sl_pct = parse_sl_pct_from_path(query_path);

    std::cout << "  dataset: N=" << N << " dim=" << DIM
              << " err=" << err
              << " sl_pct=" << sl_pct << "\n"
              << "  insert : " << ins.data.size() << " points\n"
              << "  query  : " << qf.queries.size() << " range queries\n";

    std::vector<std::vector<int32_t>> pq_coords;
    {
        std::mt19937 rng(20260522);
        std::uniform_int_distribution<int32_t> idx_dist(0, N - 1);
        pq_coords.reserve(static_cast<size_t>(NPQ));
        for (int32_t i = 0; i < NPQ; ++i) {
            const auto& dp = ds.data[idx_dist(rng)];
            pq_coords.emplace_back(dp.dimensions, dp.dimensions + DIM);
        }
    }

    auto kg0 = std::chrono::steady_clock::now();
    CryptoContext crypto(K);
    auto kg1 = std::chrono::steady_clock::now();
    double keygen_ms = std::chrono::duration<double, std::milli>(kg1 - kg0).count();
    println("Paillier keygen", keygen_ms);

    IndexConfig cfg;
    cfg.dim_count   = DIM;
    cfg.error_bound = err;
    std::cout << "  key_len = " << cfg.key_len() << " bytes\n";

    SULCipherIndex idx(cfg, crypto);
    auto b0 = std::chrono::steady_clock::now();
    idx.bulk_load(std::move(ds.data));
    auto b1 = std::chrono::steady_clock::now();
    double build_ms = std::chrono::duration<double, std::milli>(b1 - b0).count();
    println("bulk_load (including encryption)", build_ms);

    size_t gpl_inner_total = 0, gpl_inner_layers = idx.inner_layer_count();
    for (const auto& layer : idx.plain().inner_layers())
        gpl_inner_total += layer.size();
    size_t gpl_leaf_count = idx.leaf_count();
    size_t art_node_count = 0, art_leaf_count = 0;
    for (const auto& t : idx.plain().art_trees())
        if (t) { art_node_count += t->node_count(); art_leaf_count += t->leaf_count(); }

    int learning_height = static_cast<int>(gpl_inner_layers) + 1;
    int art_height_max  = cfg.key_len();

    auto sz = estimate_cipher_size(idx);
    size_t learning_bytes = sz.learning_ct * ct_bytes_estimate(K);
    size_t art_bytes      = sz.art_ct      * ct_bytes_estimate(K);
    size_t index_bytes    = sz.total_bytes(K);

    std::cout << "  GPL inner=" << gpl_inner_total
              << " (layers=" << gpl_inner_layers << ")"
              << " leaf=" << gpl_leaf_count
              << "  ART nodes=" << art_node_count
              << " leaves=" << art_leaf_count << "\n"
              << "  est_bytes: learning=" << learning_bytes
              << " art=" << art_bytes
              << " total=" << index_bytes << "\n";

    ExpParams p{ K, err, DIM, util::dataset_stem(dataset_path), "" };
    {
        std::string path = ExperimentRecorder::build_path("build", p);
        ExperimentRecorder::append_row(path,
            {"timestamp","K","err","dim","N","build_ms","keygen_ms",
             "index_bytes_total","learning_bytes","art_bytes",
             "learning_height","art_height_max",
             "node_total","inner_total","leaf_total",
             "gpl_leaf_count","art_node_count"},
            {ExperimentRecorder::now_iso(),
             std::to_string(K), std::to_string(err),
             std::to_string(DIM), std::to_string(N),
             ExperimentRecorder::ftoa(build_ms),
             ExperimentRecorder::ftoa(keygen_ms),
             std::to_string(index_bytes),
             std::to_string(learning_bytes),
             std::to_string(art_bytes),
             std::to_string(learning_height),
             std::to_string(art_height_max),
             std::to_string(gpl_inner_total + gpl_leaf_count
                            + art_node_count + art_leaf_count),
             std::to_string(gpl_inner_total + art_node_count),
             std::to_string(gpl_leaf_count + art_leaf_count),
             std::to_string(gpl_leaf_count),
             std::to_string(art_node_count)});
        std::cout << "  → record: " << path << "\n";
    }

    std::cout << "\n--- Batch point queries (npq=" << NPQ << ") ---\n";
    double pq_total_us = 0, pq_learn_us = 0, pq_art_us = 0;
    int hit_l = 0, hit_a = 0, miss = 0;
    for (const auto& coords : pq_coords) {
        SULCipherIndex::QueryStats st;
        auto t0 = std::chrono::steady_clock::now();
        EncDataPoint* h = idx.point_query_with_stats(coords.data(), &st);
        auto t1 = std::chrono::steady_clock::now();
        pq_total_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
        pq_learn_us += st.learning_us;
        pq_art_us   += st.art_us;
        if (st.hit_learning) ++hit_l;
        else if (st.hit_art) ++hit_a;
        else if (!h)         ++miss;
    }
    std::cout << "  hit_learning=" << hit_l
              << " hit_art=" << hit_a
              << " miss=" << miss << "\n"
              << "  avg total = " << (NPQ ? pq_total_us / NPQ : 0) << " us\n"
              << "  avg learning = " << (NPQ ? pq_learn_us / NPQ : 0) << " us\n"
              << "  avg art      = " << (NPQ ? pq_art_us   / NPQ : 0) << " us\n";

    std::cout << "\n--- Batch range queries ---\n";
    double rq_total_ms = 0, rq_learn_us_sum = 0, rq_art_us_sum = 0;
    size_t rq_returned_total = 0;
    for (const auto& q : qf.queries) {
        SULCipherIndex::QueryStats st;
        auto t0 = std::chrono::steady_clock::now();
        auto res = idx.range_query_with_stats(q.lo.data(), q.hi.data(), &st);
        auto t1 = std::chrono::steady_clock::now();
        rq_total_ms     += std::chrono::duration<double, std::milli>(t1 - t0).count();
        rq_learn_us_sum += st.learning_us;
        rq_art_us_sum   += st.art_us;
        rq_returned_total += res.size();
    }
    const size_t NQ = qf.queries.size();
    double rq_avg_ms    = NQ ? rq_total_ms / static_cast<double>(NQ) : 0.0;
    double rq_learn_ms  = NQ ? (rq_learn_us_sum / 1000.0) / static_cast<double>(NQ) : 0.0;
    double rq_art_ms    = NQ ? (rq_art_us_sum   / 1000.0) / static_cast<double>(NQ) : 0.0;
    double returned_avg = NQ ? static_cast<double>(rq_returned_total) / NQ : 0.0;
    double recall       = compute_range_recall(idx, qf.queries);

    std::cout << "  NQ=" << NQ
              << " total=" << rq_total_ms << " ms"
              << " avg=" << rq_avg_ms << " ms"
              << " returned_avg=" << returned_avg << "\n"
              << "  avg learning=" << rq_learn_ms << " ms"
              << "  avg art="    << rq_art_ms   << " ms"
              << "  recall=" << recall << "\n";

    {
        ExpParams pr = p;
        pr.extra = "_sl" + ExperimentRecorder::pct_tag(sl_pct);
        std::string path = ExperimentRecorder::build_path("rangequery", pr);
        ExperimentRecorder::append_row(path,
            {"timestamp","K","err","dim","N","sl_pct",
             "query_count","total_ms","avg_ms",
             "learning_ms_avg","art_ms_avg",
             "returned_avg","recall","precision"},
            {ExperimentRecorder::now_iso(),
             std::to_string(K), std::to_string(err),
             std::to_string(DIM), std::to_string(N),
             ExperimentRecorder::ftoa(sl_pct),
             std::to_string(NQ),
             ExperimentRecorder::ftoa(rq_total_ms),
             ExperimentRecorder::ftoa(rq_avg_ms),
             ExperimentRecorder::ftoa(rq_learn_ms),
             ExperimentRecorder::ftoa(rq_art_ms),
             ExperimentRecorder::ftoa(returned_avg),
             ExperimentRecorder::ftoa(recall),
             ExperimentRecorder::ftoa(1.0)});
        std::cout << "  → record: " << path << "\n";
    }

    if (!ins.data.empty()) {
        std::cout << "\n--- Batch insertion (from " << insert_path << ", "
                  << ins.data.size() << " points) ---\n";
        double ins_total_us = 0;
        int cnt_learn = 0, cnt_art = 0, cnt_fail = 0;
        for (const auto& dp : ins.data) {
            auto t0 = std::chrono::steady_clock::now();
            InsertResult r = idx.insert(dp.dimensions);
            auto t1 = std::chrono::steady_clock::now();
            ins_total_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
            switch (r) {
                case InsertResult::LearningLayer: ++cnt_learn; break;
                case InsertResult::ARTLayer:      ++cnt_art;   break;
                default:                          ++cnt_fail;  break;
            }
        }
        const size_t NI = ins.data.size();
        std::cout << "  Learning=" << cnt_learn
                  << " ART=" << cnt_art
                  << " Failed=" << cnt_fail << "\n"
                  << "  total=" << ins_total_us / 1000.0 << " ms"
                  << "  avg=" << (NI ? ins_total_us / static_cast<double>(NI) : 0) << " us\n";
    }

    std::cout << "done.\n";
    return 0;
}
