// Plaintext-versus-encrypted range-query comparison driven by dataset and query CSVs.
// Usage: ./sul_compare_demo <dataset_csv> <query_csv>
//        [paillier_key=1024] [err=-1]
//
// Workflow:
//   1) Build the plaintext index. Load an existing encrypted .scidx or build, save, and
//      reload it so queries always run against a deserialized instance.
//   2) Run every range query against both indexes, compare orig_id sets, and record results.
//
// Exit status: 0 when every result matches, otherwise 1.

#include "sul/cipher/sul_cipher_index.h"
#include "sul/sul_index.h"
#include "sul/util/csv_loader.h"
#include "sul/util/experiment_recorder.h"
#include "sul/util/query_loader.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace sul;
using namespace sul::cipher;
using sul::util::ExperimentRecorder;
using sul::util::ExpParams;

namespace {

struct CompareStats {
    int total = 0, matched = 0, mismatched = 0;
    void incr(bool ok) { ++total; if (ok) ++matched; else ++mismatched; }
};

void print_header(const char* title) { std::cout << "\n=== " << title << " ===\n"; }

void print_stats(const char* label, const CompareStats& s) {
    std::cout << "  " << label << ": " << s.matched << "/" << s.total
              << (s.mismatched ? "  [MISMATCH=" + std::to_string(s.mismatched) + "]" : "")
              << "\n";
}

// Parse selectivity from a query file name (uniform_20000_0.25.csv -> 0.25).
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

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0]
                  << " <dataset_csv> <query_csv>"
                  << " [paillier_key=1024] [err=-1]\n";
        return 1;
    }
    const std::string dataset_path = argv[1];
    const std::string query_path   = argv[2];
    const int32_t KSZ     = (argc > 3) ? std::atoi(argv[3]) : 1024;
    const int32_t err_cli = (argc > 4) ? std::atoi(argv[4]) : -1;

    std::cout << "=== Plaintext vs encrypted comparison (dataset and query CSVs) ===\n";
    std::cout << "  dataset = " << dataset_path << "\n";
    std::cout << "  query   = " << query_path << "\n";
    std::cout << "  paillier_key = " << KSZ
              << "  err(cli) = " << err_cli << " (<=0 → auto 4)\n";

    util::CsvLoadResult ds;
    util::QueryFile     qf;
    try {
        ds = util::load_csv(dataset_path);
        qf = util::load_query_file(query_path);
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
    std::cout << "  dataset: N=" << N << "  dim=" << DIM << "\n";
    std::cout << "  query:   " << qf.queries.size() << " range queries\n";

    auto data_plain  = ds.data;
    auto data_cipher = ds.data;
    IndexConfig cfg;
    cfg.dim_count   = DIM;
    cfg.error_bound = (err_cli > 0) ? err_cli : 4;

    // ------ Phase 1: Build ------
    print_header("Phase 1: Build indexes");
    auto t0 = std::chrono::steady_clock::now();
    SULPlainIndex plain(cfg);
    plain.bulk_load(std::move(data_plain));
    auto t1 = std::chrono::steady_clock::now();
    std::cout << "  plain  bulk_load: "
              << std::chrono::duration<double, std::milli>(t1 - t0).count() << " ms"
              << "  leaf=" << plain.leaf_count()
              << "  learning=" << plain.learning_layer_filled()
              << "  art=" << plain.art_layer_points() << "\n";

    namespace fs = std::filesystem;
    const std::string scidx_path = "indexes/index_"
                                 + util::dataset_stem(dataset_path)
                                 + "_K"   + std::to_string(KSZ)
                                 + "_err" + std::to_string(cfg.error_bound)
                                 + "_dim" + std::to_string(DIM)
                                 + ".scidx";
    fs::create_directories("indexes");

    std::unique_ptr<CryptoContext>  crypto_holder;
    std::unique_ptr<SULCipherIndex> cipher_holder;
    bool   did_build      = false;
    double keygen_ms      = 0.0;
    double cipher_build_ms = 0.0;
    double save_ms        = 0.0;
    double load_ms        = 0.0;
    // Structural metrics are available only on the build path.
    size_t learn_node_count = 0, learn_leaf_count = 0;
    size_t art_node_count   = 0, art_leaf_count   = 0;
    int    learn_height = 0;
    int    art_height   = 0;

    if (fs::exists(scidx_path)) {
        // Path A: deserialize an existing index.
        std::cout << "  Encrypted index found: loading " << scidx_path << "\n";
        t0 = std::chrono::steady_clock::now();
        auto pr = SULCipherIndex::load_from_file(scidx_path);
        crypto_holder = std::move(pr.first);
        cipher_holder = std::move(pr.second);
        t1 = std::chrono::steady_clock::now();
        load_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cout << "  load_from_file: " << load_ms << " ms\n";
    } else {
        // Path B: build, serialize, release, and deserialize before querying.
        std::cout << "  Encrypted index not found: " << scidx_path
                  << " → build + save + reload\n";
        did_build = true;

        t0 = std::chrono::steady_clock::now();
        auto crypto_tmp = std::make_unique<CryptoContext>(KSZ);
        t1 = std::chrono::steady_clock::now();
        keygen_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cout << "  Paillier keygen: " << keygen_ms << " ms\n";

        t0 = std::chrono::steady_clock::now();
        auto cipher_tmp = std::make_unique<SULCipherIndex>(cfg, *crypto_tmp);
        cipher_tmp->bulk_load(std::move(data_cipher));
        t1 = std::chrono::steady_clock::now();
        cipher_build_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cout << "  cipher bulk_load: " << cipher_build_ms << " ms\n";

        // Capture metrics before reset because loaded instances do not retain ART leaves.
        {
            size_t learn_inner_total = 0;
            for (const auto& layer : cipher_tmp->plain().inner_layers())
                learn_inner_total += layer.size();
            learn_leaf_count = cipher_tmp->leaf_count();
            learn_node_count = learn_inner_total + learn_leaf_count;
            learn_height     = static_cast<int>(cipher_tmp->inner_layer_count()) + 1;
            size_t art_inner_total = 0;
            for (const auto& tr : cipher_tmp->plain().art_trees())
                if (tr) { art_inner_total += tr->node_count(); art_leaf_count += tr->leaf_count(); }
            art_node_count = art_inner_total + art_leaf_count;
            art_height     = cfg.key_len();
            std::cout << "  learn nodes=" << learn_node_count
                      << " (leaf=" << learn_leaf_count << ") height=" << learn_height
                      << "  art nodes=" << art_node_count
                      << " (leaf=" << art_leaf_count << ") height=" << art_height << "\n";
        }

        t0 = std::chrono::steady_clock::now();
        cipher_tmp->save_to_file(scidx_path);
        t1 = std::chrono::steady_clock::now();
        save_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cout << "  save_to_file: " << scidx_path << " ("
                  << save_ms << " ms)\n";

        // Release build objects with cipher before crypto.
        cipher_tmp.reset();
        crypto_tmp.reset();

        t0 = std::chrono::steady_clock::now();
        auto pr = SULCipherIndex::load_from_file(scidx_path);
        crypto_holder = std::move(pr.first);
        cipher_holder = std::move(pr.second);
        t1 = std::chrono::steady_clock::now();
        load_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cout << "  load_from_file: " << load_ms << " ms\n";
    }
    SULCipherIndex& cipher = *cipher_holder;

    size_t file_bytes = 0;
    {
        std::error_code ec;
        auto sz = fs::file_size(scidx_path, ec);
        if (!ec) file_bytes = sz;
    }

    std::cout << "  cipher: leaf=" << cipher.leaf_count()
              << "  learning=" << cipher.learning_layer_filled()
              << "  art=" << cipher.art_layer_points()
              << "  is_loaded=" << (cipher.is_loaded() ? "true" : "false") << "\n";

    // Loaded plaintext skeletons omit ART leaves, so compare leaf_count only.
    bool struct_ok = cipher.is_loaded()
        ? (plain.leaf_count() == cipher.leaf_count())
        : (plain.leaf_count() == cipher.leaf_count()
        && plain.learning_layer_filled() == cipher.learning_layer_filled()
        && plain.art_layer_points() == cipher.art_layer_points());
    std::cout << "  Structure size matches? " << (struct_ok ? "YES" : "NO")
              << (cipher.is_loaded() ? " (loaded mode compares leaf_count only)" : "") << "\n";

    const double sl_pct = parse_sl_pct_from_path(query_path);
    ExpParams exp_params{ KSZ, cfg.error_bound, DIM,
                          util::dataset_stem(dataset_path), "" };

    // Record build metrics only on the build path.
    if (did_build) {
        // Equivalent file_bytes_kl1 assumes one ciphertext per ART key in EncDataPoint.
        // It is a cross-dimensional comparison metric; file_bytes remains the real size.
        const int64_t key_len_actual    = cfg.key_len();
        const int64_t cipher_disk_bytes = 4 + ((2LL * KSZ + 7) / 8);
        const int64_t kl1_saved =
            (key_len_actual - 1) * static_cast<int64_t>(N) * cipher_disk_bytes;
        const size_t  file_bytes_kl1 =
            (static_cast<int64_t>(file_bytes) > kl1_saved)
            ? file_bytes - static_cast<size_t>(kl1_saved) : 0;

        fs::create_directories("record");
        std::string build_csv = ExperimentRecorder::build_path("build", exp_params);
        ExperimentRecorder::append_row(build_csv,
            {"timestamp","K","err","dim","N",
             "build_ms","keygen_ms","save_ms","load_ms",
             "file_bytes","file_bytes_kl1",
             "learn_node_count","learn_leaf_count","learn_height",
             "art_node_count","art_leaf_count","art_height"},
            {ExperimentRecorder::now_iso(),
             std::to_string(KSZ),
             std::to_string(cfg.error_bound),
             std::to_string(DIM),
             std::to_string(N),
             ExperimentRecorder::ftoa(cipher_build_ms),
             ExperimentRecorder::ftoa(keygen_ms),
             ExperimentRecorder::ftoa(save_ms),
             ExperimentRecorder::ftoa(load_ms),
             std::to_string(file_bytes),
             std::to_string(file_bytes_kl1),
             std::to_string(learn_node_count),
             std::to_string(learn_leaf_count),
             std::to_string(learn_height),
             std::to_string(art_node_count),
             std::to_string(art_leaf_count),
             std::to_string(art_height)});
        std::cout << "  → record: " << build_csv << "\n";
        std::cout << "  file_bytes      = " << file_bytes      << " B"
                  << "  (" << (file_bytes      / 1024.0 / 1024.0) << " MiB on disk)\n";
        std::cout << "  file_bytes_kl1  = " << file_bytes_kl1  << " B"
                  << "  (" << (file_bytes_kl1  / 1024.0 / 1024.0)
                  << " MiB equivalent with one ART-key ciphertext per point)\n";
    }

    // ------ Phase 2: Range queries ------
    print_header("Phase 2: Compare range queries from the query file");
    CompareStats rq_stats;
    size_t plain_total_returned  = 0;
    size_t cipher_total_returned = 0;
    size_t intersect_total       = 0;
    double plain_ms_total        = 0.0;
    double cipher_ms_total       = 0.0;
    double cipher_learn_us_sum   = 0.0;
    double cipher_art_us_sum     = 0.0;
    double cipher_collect_us_sum    = 0.0;
    double cipher_spi_setup_us_sum  = 0.0;
    double cipher_spi_filter_us_sum = 0.0;
    size_t cipher_cand_total        = 0;
    size_t cipher_cand_kept         = 0;
    size_t cipher_mid_total_sum     = 0;
    size_t cipher_mid_pruned_sum    = 0;
    std::vector<size_t> plain_hits_per_query;
    plain_hits_per_query.reserve(qf.queries.size());

    for (size_t qi = 0; qi < qf.queries.size(); ++qi) {
        const auto& q = qf.queries[qi];

        auto pt0 = std::chrono::steady_clock::now();
        auto p_res = plain.range_query(q.lo.data(), q.hi.data());
        auto pt1 = std::chrono::steady_clock::now();
        plain_ms_total += std::chrono::duration<double, std::milli>(pt1 - pt0).count();

        SULCipherIndex::QueryStats cst;
        auto ct0 = std::chrono::steady_clock::now();
        auto c_res = cipher.range_query_with_stats(q.lo.data(), q.hi.data(), &cst);
        auto ct1 = std::chrono::steady_clock::now();
        cipher_ms_total          += std::chrono::duration<double, std::milli>(ct1 - ct0).count();
        cipher_learn_us_sum      += cst.learning_us;
        cipher_art_us_sum        += cst.art_us;
        cipher_collect_us_sum    += cst.collect_us;
        cipher_spi_setup_us_sum  += cst.spi_setup_us;
        cipher_spi_filter_us_sum += cst.spi_filter_us;
        cipher_cand_total        += cst.candidates_total;
        cipher_cand_kept         += cst.candidates_kept;
        cipher_mid_total_sum     += cst.middle_leaves_total;
        cipher_mid_pruned_sum    += cst.middle_leaves_pruned;

        plain_total_returned  += p_res.size();
        cipher_total_returned += c_res.size();
        plain_hits_per_query.push_back(p_res.size());

        std::set<int32_t> p_ids, c_ids;
        for (auto* p : p_res) p_ids.insert(p->orig_id);
        for (auto* p : c_res) c_ids.insert(p->orig_id);
        size_t inter = 0;
        for (int x : p_ids) if (c_ids.count(x)) ++inter;
        intersect_total += inter;
        bool ok = (p_ids == c_ids);
        rq_stats.incr(ok);
        if (!ok) {
            int only_p = 0, only_c = 0;
            for (int x : p_ids) if (!c_ids.count(x)) ++only_p;
            for (int x : c_ids) if (!p_ids.count(x)) ++only_c;
            std::cout << "  q" << qi << " MISMATCH: plain=" << p_res.size()
                      << " cipher=" << c_res.size()
                      << " plain-only=" << only_p << " cipher-only=" << only_c << "\n";
        }
    }
    const size_t NQ = qf.queries.size();
    print_stats("matching range_query results", rq_stats);
    std::cout << "  Average result size: plain=" << (NQ ? plain_total_returned / NQ : 0)
              << "  cipher=" << (NQ ? cipher_total_returned / NQ : 0) << "\n";
    std::cout << "  Total time: plain=" << plain_ms_total << " ms"
              << "  cipher=" << cipher_ms_total << " ms\n";
    const double plain_avg_ms  = NQ ? plain_ms_total  / static_cast<double>(NQ) : 0.0;
    const double cipher_avg_ms = NQ ? cipher_ms_total / static_cast<double>(NQ) : 0.0;
    const double cipher_learn_ms_avg =
        NQ ? (cipher_learn_us_sum / 1000.0) / static_cast<double>(NQ) : 0.0;
    const double cipher_art_ms_avg   =
        NQ ? (cipher_art_us_sum   / 1000.0) / static_cast<double>(NQ) : 0.0;
    const double cipher_collect_ms_avg =
        NQ ? (cipher_collect_us_sum / 1000.0) / static_cast<double>(NQ) : 0.0;
    const double cipher_spi_setup_ms_avg =
        NQ ? (cipher_spi_setup_us_sum / 1000.0) / static_cast<double>(NQ) : 0.0;
    const double cipher_spi_filter_ms_avg =
        NQ ? (cipher_spi_filter_us_sum / 1000.0) / static_cast<double>(NQ) : 0.0;
    const double candidates_avg =
        NQ ? static_cast<double>(cipher_cand_total) / static_cast<double>(NQ) : 0.0;
    const double kept_avg =
        NQ ? static_cast<double>(cipher_cand_kept)  / static_cast<double>(NQ) : 0.0;
    const double spi_filtered_avg = candidates_avg - kept_avg;
    const double spi_pass_rate = candidates_avg > 0
        ? kept_avg / candidates_avg : 0.0;
    const double middle_total_avg = NQ
        ? static_cast<double>(cipher_mid_total_sum)  / static_cast<double>(NQ) : 0.0;
    const double middle_pruned_avg = NQ
        ? static_cast<double>(cipher_mid_pruned_sum) / static_cast<double>(NQ) : 0.0;
    const double middle_prune_rate = middle_total_avg > 0
        ? middle_pruned_avg / middle_total_avg : 0.0;
    std::cout << "  Average per query: plain=" << plain_avg_ms << " ms/q"
              << "  cipher=" << cipher_avg_ms << " ms/q\n"
              << "  Cipher breakdown: learning=" << cipher_learn_ms_avg << " ms/q"
              << "  art=" << cipher_art_ms_avg << " ms/q\n"
              << "  ART breakdown: collect=" << cipher_collect_ms_avg << " ms/q"
              << "  spi_setup=" << cipher_spi_setup_ms_avg << " ms/q"
              << "  spi_filter=" << cipher_spi_filter_ms_avg << " ms/q\n"
              << "  SPI candidates: total=" << candidates_avg
              << " kept=" << kept_avg
              << " filtered=" << spi_filtered_avg
              << " pass_rate=" << (spi_pass_rate * 100.0) << "%\n"
              << "  Middle-leaf pruning: total=" << middle_total_avg
              << " pruned=" << middle_pruned_avg
              << " prune_rate=" << (middle_prune_rate * 100.0) << "%\n";

    const double recall    = plain_total_returned
        ? static_cast<double>(intersect_total) / static_cast<double>(plain_total_returned)
        : 1.0;
    const double precision = cipher_total_returned
        ? static_cast<double>(intersect_total) / static_cast<double>(cipher_total_returned)
        : 1.0;
    const double returned_avg =
        NQ ? static_cast<double>(cipher_total_returned) / static_cast<double>(NQ) : 0.0;

    // Plaintext hit distribution is ground truth for cross-dataset comparisons.
    // On skewed data, actual_ratio_pct is the meaningful selectivity.
    size_t hits_min = 0, hits_p50 = 0, hits_p95 = 0, hits_max = 0;
    double actual_ratio_pct = 0.0;
    if (!plain_hits_per_query.empty()) {
        std::vector<size_t> sorted_hits = plain_hits_per_query;
        std::sort(sorted_hits.begin(), sorted_hits.end());
        const size_t M = sorted_hits.size();
        hits_min = sorted_hits.front();
        hits_max = sorted_hits.back();
        hits_p50 = sorted_hits[M / 2];
        // p95 index = ceil(0.95*M) - 1, clamped to [0, M-1].
        size_t idx95 = (M * 95 + 99) / 100;
        if (idx95 == 0) idx95 = 1;
        if (idx95 > M) idx95 = M;
        hits_p95 = sorted_hits[idx95 - 1];
        const double plain_returned_avg =
            static_cast<double>(plain_total_returned) / static_cast<double>(M);
        actual_ratio_pct = N > 0
            ? (plain_returned_avg / static_cast<double>(N)) * 100.0 : 0.0;
    }

    {
        fs::create_directories("record");
        ExpParams pr = exp_params;
        pr.extra = "_sl" + ExperimentRecorder::pct_tag(sl_pct);
        std::string rq_csv = ExperimentRecorder::build_path("rangequery", pr);
        ExperimentRecorder::append_row(rq_csv,
            {"timestamp","K","err","dim","N","sl_pct",
             "query_count","total_ms","avg_ms",
             "learning_ms_avg","art_ms_avg",
             "collect_ms_avg","spi_setup_ms_avg","spi_filter_ms_avg",
             "candidates_avg","kept_avg",
             "middle_total_avg","middle_pruned_avg",
             "returned_avg","recall","precision",
             "actual_ratio_pct","returned_min","returned_p50",
             "returned_p95","returned_max"},
            {ExperimentRecorder::now_iso(),
             std::to_string(KSZ),
             std::to_string(cfg.error_bound),
             std::to_string(DIM),
             std::to_string(N),
             ExperimentRecorder::ftoa(sl_pct),
             std::to_string(NQ),
             ExperimentRecorder::ftoa(cipher_ms_total),
             ExperimentRecorder::ftoa(cipher_avg_ms),
             ExperimentRecorder::ftoa(cipher_learn_ms_avg),
             ExperimentRecorder::ftoa(cipher_art_ms_avg),
             ExperimentRecorder::ftoa(cipher_collect_ms_avg),
             ExperimentRecorder::ftoa(cipher_spi_setup_ms_avg),
             ExperimentRecorder::ftoa(cipher_spi_filter_ms_avg),
             ExperimentRecorder::ftoa(candidates_avg),
             ExperimentRecorder::ftoa(kept_avg),
             ExperimentRecorder::ftoa(middle_total_avg),
             ExperimentRecorder::ftoa(middle_pruned_avg),
             ExperimentRecorder::ftoa(returned_avg),
             ExperimentRecorder::ftoa(recall),
             ExperimentRecorder::ftoa(precision),
             ExperimentRecorder::ftoa(actual_ratio_pct),
             std::to_string(hits_min),
             std::to_string(hits_p50),
             std::to_string(hits_p95),
             std::to_string(hits_max)});
        std::cout << "  → record: " << rq_csv << "\n";
        std::cout << "  Observed plaintext selectivity: " << actual_ratio_pct << "%"
                  << "  (nominal sl_pct=" << sl_pct << "%)\n"
                  << "  Plaintext hit distribution: min=" << hits_min
                  << " p50=" << hits_p50
                  << " p95=" << hits_p95
                  << " max=" << hits_max << "\n";
    }

    // ------ Summary ------
    print_header("Summary");
    std::cout << "  Matching range queries: " << rq_stats.matched << "/" << rq_stats.total << "\n";
    bool all_pass = (rq_stats.mismatched == 0) && struct_ok;
    std::cout << "  Result: "
              << (all_pass ? "PASS: encrypted and plaintext results match"
                           : "FAIL: one or more results differ")
              << "\n";
    return all_pass ? 0 : 1;
}
