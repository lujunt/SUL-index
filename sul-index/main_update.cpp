// 更新对比 demo：按更新率 ul% 批量插入，再做范围查询对比更新前后性能
//
// 用法:
//   ./sul_update <train_csv> <insert_csv> <query_csv>
//                [K=1024] [err=-1] [ul_pct=0.25]
//
// 行为:
//   1) train_csv → cipher 索引 bulk_load
//   2) insert_csv 全量加载，按 ul_pct 截取前 ceil(N_train * ul_pct/100) 条作更新集
//   3) 顺序执行所有 insert，记录 update_total_ms / update_avg_ms
//   4) 在更新后的索引上批量跑 query_csv，记录 post_query_avg_ms / post_recall
//   5) 写 record/update_K..._err..._dim..._N..._ul{tag}.csv

#include "sul/cipher/sul_cipher_index.h"
#include "sul/util/csv_loader.h"
#include "sul/util/experiment_recorder.h"
#include "sul/util/query_loader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

using namespace sul;
using namespace sul::cipher;
using sul::util::ExperimentRecorder;
using sul::util::ExpParams;

namespace {

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
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0]
                  << " <train_csv> <insert_csv> <query_csv>"
                     " [K=1024] [err=-1] [ul_pct=0.25]\n";
        return 1;
    }
    const std::string train_path  = argv[1];
    const std::string insert_path = argv[2];
    const std::string query_path  = argv[3];
    const int32_t K       = (argc > 4) ? std::atoi(argv[4]) : 1024;
    int32_t       err_cli = (argc > 5) ? std::atoi(argv[5]) : -1;
    const double  ul_pct  = (argc > 6) ? std::atof(argv[6]) : 0.25;

    if (ul_pct <= 0.0) {
        std::cerr << "[error] ul_pct 必须 > 0\n";
        return 2;
    }

    util::CsvLoadResult train, ins;
    util::QueryFile     qf;
    try {
        train = util::load_csv(train_path);
        ins   = util::load_csv(insert_path);
        qf    = util::load_query_file(query_path);
    } catch (const std::exception& e) {
        std::cerr << "[error] " << e.what() << "\n";
        return 3;
    }
    const int32_t N   = static_cast<int32_t>(train.data.size());
    const int32_t DIM = train.dim_count;
    if (DIM != ins.dim_count || DIM != qf.dim_count) {
        std::cerr << "[error] dim 不一致\n";
        return 4;
    }
    const int32_t err = (err_cli > 0) ? err_cli : std::max(8, N / 1000);
    const int32_t target_updates =
        std::min<int32_t>(static_cast<int32_t>(ins.data.size()),
                          static_cast<int32_t>(std::ceil(
                              static_cast<double>(N) * ul_pct / 100.0)));

    std::cout << "=== sul_update ===\n"
              << "  train : " << train_path  << " (N=" << N << ", dim=" << DIM << ")\n"
              << "  insert: " << insert_path << " (" << ins.data.size() << " 点)\n"
              << "  query : " << query_path  << " (" << qf.queries.size() << " 条)\n"
              << "  K=" << K << "  err=" << err
              << "  ul%=" << ul_pct << "  → 更新数=" << target_updates << "\n";

    CryptoContext crypto(K);
    IndexConfig cfg;
    cfg.dim_count   = DIM;
    cfg.error_bound = err;

    SULCipherIndex idx(cfg, crypto);
    idx.bulk_load(std::move(train.data));

    double upd_total_us = 0;
    int32_t learn_cnt = 0, art_cnt = 0, fail_cnt = 0;
    auto u0 = std::chrono::steady_clock::now();
    for (int32_t i = 0; i < target_updates; ++i) {
        const auto& dp = ins.data[static_cast<size_t>(i)];
        auto t0 = std::chrono::steady_clock::now();
        InsertResult r = idx.insert(dp.dimensions);
        auto t1 = std::chrono::steady_clock::now();
        upd_total_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
        switch (r) {
            case InsertResult::LearningLayer: ++learn_cnt; break;
            case InsertResult::ARTLayer:      ++art_cnt;   break;
            default:                          ++fail_cnt;  break;
        }
    }
    auto u1 = std::chrono::steady_clock::now();
    double update_total_ms = std::chrono::duration<double, std::milli>(u1 - u0).count();
    double update_avg_ms = target_updates
        ? (upd_total_us / 1000.0) / static_cast<double>(target_updates) : 0.0;

    double rq_total_ms = 0;
    size_t rq_returned = 0;
    for (const auto& q : qf.queries) {
        auto t0 = std::chrono::steady_clock::now();
        auto res = idx.range_query(q.lo.data(), q.hi.data());
        auto t1 = std::chrono::steady_clock::now();
        rq_total_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
        rq_returned += res.size();
    }
    const size_t NQ = qf.queries.size();
    double post_query_avg_ms = NQ ? rq_total_ms / static_cast<double>(NQ) : 0.0;
    double post_recall = compute_range_recall(idx, qf.queries);

    std::cout << "\n--- 结果 ---\n"
              << "  更新: learn=" << learn_cnt << " art=" << art_cnt
              << " fail=" << fail_cnt << "\n"
              << "  update_total=" << update_total_ms << " ms"
              << "  update_avg=" << update_avg_ms << " ms\n"
              << "  post_query_avg=" << post_query_avg_ms << " ms/q"
              << "  post_recall=" << post_recall
              << "  returned_avg=" << (NQ ? static_cast<double>(rq_returned) / NQ : 0) << "\n";

    ExpParams p{ K, err, DIM, N,
                 "_ul" + ExperimentRecorder::pct_tag(ul_pct) };
    std::string path = ExperimentRecorder::build_path("update", p);
    ExperimentRecorder::append_row(path,
        {"timestamp","K","err","dim","N","ul_pct",
         "update_count","update_total_ms","update_avg_ms",
         "post_query_avg_ms","post_recall"},
        {ExperimentRecorder::now_iso(),
         std::to_string(K), std::to_string(err),
         std::to_string(DIM), std::to_string(N),
         ExperimentRecorder::ftoa(ul_pct),
         std::to_string(target_updates),
         ExperimentRecorder::ftoa(update_total_ms),
         ExperimentRecorder::ftoa(update_avg_ms),
         ExperimentRecorder::ftoa(post_query_avg_ms),
         ExperimentRecorder::ftoa(post_recall)});
    std::cout << "  → record: " << path << "\n";

    // 序列化更新后的索引，命名加 update 标记，方便后续查询
    namespace fs = std::filesystem;
    fs::create_directories("indexes");
    const std::string scidx_path = "indexes/index_K" + std::to_string(K)
                                 + "_err" + std::to_string(err)
                                 + "_dim" + std::to_string(DIM)
                                 + "_N"   + std::to_string(N)
                                 + "_ul"  + ExperimentRecorder::pct_tag(ul_pct)
                                 + "_update.scidx";
    auto s0 = std::chrono::steady_clock::now();
    try {
        idx.save_to_file(scidx_path);
    } catch (const std::exception& e) {
        std::cerr << "[save error] " << e.what() << "\n";
        return 5;
    }
    auto s1 = std::chrono::steady_clock::now();
    std::cout << "  → save_to_file: " << scidx_path << " ("
              << std::chrono::duration<double, std::milli>(s1 - s0).count() << " ms, "
              << fs::file_size(scidx_path) << " bytes)\n";
    return 0;
}
