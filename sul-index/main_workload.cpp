// 场景实验 demo：固定 OPS=2000 操作按读/写比例交叉执行
//
// 用法:
//   ./sul_workload <train_csv> <insert_csv>
//                  [K=1024] [err=-1] [read_pct=50] [ops=2000]
//
// 行为:
//   1) train_csv → cipher 索引 bulk_load（建议 90% 训练集，可用 sul_split 划分）
//   2) 读操作 = 点查询，坐标从 train 数据集随机抽样（同分布、保证可命中）
//   3) 写操作 = insert_csv 按行顺序循环取用
//   4) 按 read_pct/100 的比例交叉执行 ops 次操作
//   5) 写 record/workload_<stem>_K..._err..._dim..._R{r}W{w}.csv

#include "sul/cipher/sul_cipher_index.h"
#include "sul/util/csv_loader.h"
#include "sul/util/experiment_recorder.h"
#include "sul/util/query_loader.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>

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
        std::cerr << "[error] read_pct 必须在 [0,100] 区间\n";
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
        std::cerr << "[error] dim 不一致 (train=" << DIM
                  << " insert=" << ins.dim_count << ")\n";
        return 4;
    }
    const int32_t err = (err_cli > 0) ? err_cli : std::max(8, N / 1000);

    std::cout << "=== sul_workload ===\n"
              << "  train : " << train_path  << " (N=" << N << ", dim=" << DIM << ")\n"
              << "  insert: " << insert_path << " (" << ins.data.size() << " 点)\n"
              << "  K=" << K << "  err=" << err
              << "  R%=" << read_pct << "  W%=" << write_pct
              << "  ops=" << OPS << "\n";

    // 读操作 = 点查询，坐标从 train 随机抽样（同分布、保证可命中学习层）
    // 在 bulk_load 之前预先抽 OPS 个点坐标（实际只用前 n_read 个）
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

    CryptoContext crypto(K);
    IndexConfig cfg;
    cfg.dim_count   = DIM;
    cfg.error_bound = err;

    SULCipherIndex idx(cfg, crypto);
    idx.bulk_load(std::move(train.data));

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

    std::cout << "\n--- 结果 ---\n"
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
