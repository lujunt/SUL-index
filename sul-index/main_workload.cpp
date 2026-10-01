// Workload demo: interleave 2,000 operations at a selected read/write ratio.
//
// Usage:
//   ./sul_workload <train_csv> <insert_csv>
//                  [K=1024] [err=-1] [read_pct=50] [ops=2000]
//
// Behavior:
//   1) Bulk-load train_csv into the encrypted index.
//   2) Serialize the index and write build metrics.
//   3) Sample point reads from training data and cycle writes through insert_csv.
//   4) Interleave ops operations according to read_pct and write workload metrics.

#include "sul/cipher/sul_cipher_index.h"
#include "sul/util/csv_loader.h"
#include "sul/util/experiment_recorder.h"
#include "sul/util/query_loader.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using namespace sul;
using namespace sul::cipher;
using sul::util::ExperimentRecorder;
using sul::util::ExpParams;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0]
                  << " <train_csv> <insert_csv>"
                     " [K=1024] [err=-1] [read_pct=50] [ops=2000]\n";
        return 1;
    }
    const std::string train_path  = argv[1];
    const std::string insert_path = argv[2];
    const int32_t K        = (argc > 3) ? std::atoi(argv[3]) : 1024;
    int32_t       err_cli  = (argc > 4) ? std::atoi(argv[4]) : -1;
    const int32_t read_pct = (argc > 5) ? std::atoi(argv[5]) : 50;
    const int32_t OPS      = (argc > 6) ? std::atoi(argv[6]) : 2000;

    if (read_pct < 0 || read_pct > 100) {
        std::cerr << "[error] read_pct must be in [0, 100]\n";
        return 2;
    }
    const int32_t write_pct = 100 - read_pct;

    util::CsvLoadResult train, ins;
    try {
        train = util::load_csv(train_path);
        ins   = util::load_csv(insert_path);
    } catch (const std::exception& e) {
        std::cerr << "[error] " << e.what() << "\n";
        return 3;
    }
    const int32_t N   = static_cast<int32_t>(train.data.size());
    const int32_t DIM = train.dim_count;
    if (DIM != ins.dim_count) {
        std::cerr << "[error] dimension mismatch (train=" << DIM
                  << " insert=" << ins.dim_count << ")\n";
        return 4;
    }
    const int32_t err = (err_cli > 0) ? err_cli : 4;

    std::cout << "=== sul_workload ===\n"
              << "  train : " << train_path  << " (N=" << N << ", dim=" << DIM << ")\n"
              << "  insert: " << insert_path << " (" << ins.data.size() << " points)\n"
              << "  K=" << K << "  err=" << err
              << "  R%=" << read_pct << "  W%=" << write_pct
              << "  ops=" << OPS << "\n";

    // Reads are point queries sampled from the training distribution. Sample OPS
    // coordinates before bulk_load; only the first n_read are used.
    std::vector<std::vector<int32_t>> read_coords;
    {
        std::mt19937 rng(20260522);
        std::uniform_int_distribution<int32_t> idx_dist(0, N - 1);
        read_coords.reserve(static_cast<size_t>(OPS));
        for (int32_t i = 0; i < OPS; ++i) {
            const auto& dp = train.data[idx_dist(rng)];
            read_coords.emplace_back(dp.dimensions, dp.dimensions + DIM);
        }
    }

    IndexConfig cfg;
    cfg.dim_count   = DIM;
    cfg.error_bound = err;

    auto kb_t0 = std::chrono::steady_clock::now();
    CryptoContext crypto(K);
    auto kb_t1 = std::chrono::steady_clock::now();
    double keygen_ms = std::chrono::duration<double, std::milli>(kb_t1 - kb_t0).count();
    std::cout << "  Paillier keygen: " << keygen_ms << " ms\n";

    SULCipherIndex idx(cfg, crypto);
    auto build_t0 = std::chrono::steady_clock::now();
    idx.bulk_load(std::move(train.data));
    auto build_t1 = std::chrono::steady_clock::now();
    double cipher_build_ms = std::chrono::duration<double, std::milli>(build_t1 - build_t0).count();
    std::cout << "  cipher bulk_load: " << cipher_build_ms << " ms\n";

    // Learning-layer and ART node, leaf, and height metrics.
    size_t learn_inner_total = 0;
    for (const auto& layer : idx.plain().inner_layers()) learn_inner_total += layer.size();
    const size_t learn_leaf_count = idx.leaf_count();
    const size_t learn_node_count = learn_inner_total + learn_leaf_count;
    const int    learn_height     = static_cast<int>(idx.inner_layer_count()) + 1;
    size_t art_inner_total = 0, art_leaf_count = 0;
    for (const auto& t : idx.plain().art_trees())
        if (t) { art_inner_total += t->node_count(); art_leaf_count += t->leaf_count(); }
    const size_t art_node_count = art_inner_total + art_leaf_count;
    const int    art_height     = cfg.key_len();
    std::cout << "  learn nodes=" << learn_node_count
              << " (leaf=" << learn_leaf_count << ") height=" << learn_height
              << "  art nodes=" << art_node_count
              << " (leaf=" << art_leaf_count << ") height=" << art_height << "\n";

    // Serialize and record build fields aligned with main_compare; append _N{N} to avoid collisions.
    const std::string stem_train  = util::dataset_stem(train_path);
    const std::string stem_with_N = stem_train + "_N" + std::to_string(N);
    fs::create_directories("indexes");
    const std::string scidx_path = "indexes/workload_" + stem_train
                                 + "_N"   + std::to_string(N)
                                 + "_K"   + std::to_string(K)
                                 + "_err" + std::to_string(err)
                                 + "_dim" + std::to_string(DIM)
                                 + ".scidx";

    auto save_t0 = std::chrono::steady_clock::now();
    idx.save_to_file(scidx_path);
    auto save_t1 = std::chrono::steady_clock::now();
    double save_ms = std::chrono::duration<double, std::milli>(save_t1 - save_t0).count();
    std::cout << "  save_to_file: " << scidx_path << " (" << save_ms << " ms)\n";

    size_t file_bytes = 0;
    {
        std::error_code ec;
        auto sz = fs::file_size(scidx_path, ec);
        if (!ec) file_bytes = sz;
    }
    // Equivalent file_bytes_kl1 uses the main_compare convention of one ART-key ciphertext.
    const int64_t key_len_actual    = cfg.key_len();
    const int64_t cipher_disk_bytes = 4 + ((2LL * K + 7) / 8);
    const int64_t kl1_saved =
        (key_len_actual - 1) * static_cast<int64_t>(N) * cipher_disk_bytes;
    const size_t  file_bytes_kl1 =
        (static_cast<int64_t>(file_bytes) > kl1_saved)
        ? file_bytes - static_cast<size_t>(kl1_saved) : 0;

    fs::create_directories("record");
    {
        ExpParams bp{ K, err, DIM, stem_with_N, "" };
        std::string build_csv = ExperimentRecorder::build_path("build", bp);
        ExperimentRecorder::append_row(build_csv,
            {"timestamp","K","err","dim","N",
             "build_ms","keygen_ms","save_ms","load_ms",
             "file_bytes","file_bytes_kl1",
             "learn_node_count","learn_leaf_count","learn_height",
             "art_node_count","art_leaf_count","art_height"},
            {ExperimentRecorder::now_iso(),
             std::to_string(K),
             std::to_string(err),
             std::to_string(DIM),
             std::to_string(N),
             ExperimentRecorder::ftoa(cipher_build_ms),
             ExperimentRecorder::ftoa(keygen_ms),
             ExperimentRecorder::ftoa(save_ms),
             ExperimentRecorder::ftoa(0.0),   // Workloads do not reload; keep the field aligned with compare.
             std::to_string(file_bytes),
             std::to_string(file_bytes_kl1),
             std::to_string(learn_node_count),
             std::to_string(learn_leaf_count),
             std::to_string(learn_height),
             std::to_string(art_node_count),
             std::to_string(art_leaf_count),
             std::to_string(art_height)});
        std::cout << "  → record: " << build_csv << "\n";
        std::cout << "  file_bytes     = " << file_bytes
                  << " B (" << (file_bytes/1024.0/1024.0) << " MiB)\n"
                  << "  file_bytes_kl1 = " << file_bytes_kl1
                  << " B (" << (file_bytes_kl1/1024.0/1024.0) << " MiB)\n";
    }

    std::vector<char> ops_seq(static_cast<size_t>(OPS));
    {
        int32_t n_read = OPS * read_pct / 100;
        int32_t n_write = OPS - n_read;
        for (int32_t i = 0; i < n_read;  ++i) ops_seq[i] = 'R';
        for (int32_t i = 0; i < n_write; ++i) ops_seq[n_read + i] = 'W';
        std::mt19937 rng(20260523);
        std::shuffle(ops_seq.begin(), ops_seq.end(), rng);
    }

    double read_total_us = 0, read_learn_us = 0, read_art_us = 0;
    double write_total_us = 0, write_locate_us = 0;
    int32_t read_count = 0, write_count = 0, write_fail = 0;
    size_t ri = 0, ii = 0;

    auto t_start = std::chrono::steady_clock::now();
    for (char op : ops_seq) {
        if (op == 'R') {
            const auto& coords = read_coords[ri++ % read_coords.size()];
            SULCipherIndex::QueryStats st;
            auto t0 = std::chrono::steady_clock::now();
            (void)idx.point_query_with_stats(coords.data(), &st);
            auto t1 = std::chrono::steady_clock::now();
            read_total_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
            read_learn_us += st.learning_us;
            read_art_us   += st.art_us;
            ++read_count;
        } else if (op == 'W' && !ins.data.empty()) {
            const auto& dp = ins.data[ii++ % ins.data.size()];
            SULCipherIndex::QueryStats locate_st;
            auto t0 = std::chrono::steady_clock::now();
            (void)idx.point_query_with_stats(dp.dimensions, &locate_st);
            auto t1 = std::chrono::steady_clock::now();
            InsertResult r = idx.insert(dp.dimensions);
            auto t2 = std::chrono::steady_clock::now();
            write_total_us  += std::chrono::duration<double, std::micro>(t2 - t0).count();
            write_locate_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
            ++write_count;
            if (r == InsertResult::Failed) ++write_fail;
        }
    }
    auto t_end = std::chrono::steady_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
    double total_s  = total_ms / 1000.0;

    double thr_total = total_s > 0 ? OPS / total_s : 0;
    double thr_read  = total_s > 0 ? read_count  / total_s : 0;
    double thr_write = total_s > 0 ? write_count / total_s : 0;

    double q_avg_ms      = read_count  ? (read_total_us / 1000.0)  / read_count  : 0;
    double q_learn_avg   = read_count  ? (read_learn_us / 1000.0)  / read_count  : 0;
    double q_art_avg     = read_count  ? (read_art_us   / 1000.0)  / read_count  : 0;
    double upd_avg_ms    = write_count ? (write_total_us  / 1000.0) / write_count : 0;
    double locate_avg_ms = write_count ? (write_locate_us / 1000.0) / write_count : 0;
    double update_only_ms = upd_avg_ms - locate_avg_ms;

    std::cout << "\n--- Results ---\n"
              << "  read=" << read_count << " write=" << write_count
              << " write_fail=" << write_fail << "\n"
              << "  total=" << total_ms << " ms"
              << "  throughput=" << thr_total << " ops/s"
              << " (R=" << thr_read << " W=" << thr_write << ")\n"
              << "  query_avg=" << q_avg_ms << " ms"
              << " (learn=" << q_learn_avg << " art=" << q_art_avg << ")\n"
              << "  update_avg=" << upd_avg_ms << " ms"
              << " (locate=" << locate_avg_ms
              << " update_only=" << update_only_ms << ")\n";

    char extra_buf[64];
    std::snprintf(extra_buf, sizeof(extra_buf), "_R%dW%d", read_pct, write_pct);
    ExpParams p{ K, err, DIM, util::dataset_stem(train_path), extra_buf };
    std::string path = ExperimentRecorder::build_path("workload", p);
    ExperimentRecorder::append_row(path,
        {"timestamp","K","err","dim","N",
         "read_pct","write_pct","ops_total","total_ms",
         "throughput_total","throughput_read","throughput_write",
         "query_latency_avg_ms","learning_query_avg_ms","art_query_avg_ms",
         "update_latency_avg_ms","locate_avg_ms","update_avg_ms"},
        {ExperimentRecorder::now_iso(),
         std::to_string(K), std::to_string(err),
         std::to_string(DIM), std::to_string(N),
         std::to_string(read_pct), std::to_string(write_pct),
         std::to_string(OPS),
         ExperimentRecorder::ftoa(total_ms),
         ExperimentRecorder::ftoa(thr_total),
         ExperimentRecorder::ftoa(thr_read),
         ExperimentRecorder::ftoa(thr_write),
         ExperimentRecorder::ftoa(q_avg_ms),
         ExperimentRecorder::ftoa(q_learn_avg),
         ExperimentRecorder::ftoa(q_art_avg),
         ExperimentRecorder::ftoa(upd_avg_ms),
         ExperimentRecorder::ftoa(locate_avg_ms),
         ExperimentRecorder::ftoa(update_only_ms)});
    std::cout << "  → record: " << path << "\n";
    return 0;
}
