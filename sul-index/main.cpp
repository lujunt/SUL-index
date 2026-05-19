#include "sul/sul_index.h"
#include "sul/z_order.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

struct RawPoint {
    double x;
    double y;
    int32_t id;
};

bool load_csv(const std::string& path, std::vector<RawPoint>& out) {
    std::ifstream in(path);
    if (!in) {
        std::cerr << "[error] cannot open: " << path << std::endl;
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string tok;
        RawPoint p;
        if (!std::getline(ss, tok, ',')) continue;
        p.x = std::stod(tok);
        if (!std::getline(ss, tok, ',')) continue;
        p.y = std::stod(tok);
        if (!std::getline(ss, tok, ',')) {
            p.id = static_cast<int32_t>(out.size());
        } else {
            p.id = std::atoi(tok.c_str());
        }
        out.push_back(p);
    }
    return true;
}

std::vector<sul::DataPoint> make_data_points(const std::vector<RawPoint>& raw) {
    std::vector<sul::DataPoint> dps;
    dps.reserve(raw.size());
    for (const auto& r : raw) {
        sul::DataPoint dp{};
        dp.dim_count = 2;
        dp.dimensions[0] = sul::scale_unit_double_to_int32(r.x);
        dp.dimensions[1] = sul::scale_unit_double_to_int32(r.y);
        dp.orig_id = r.id;
        dp.z_value = 0;
        std::memset(dp.key_bytes, 0, sizeof(dp.key_bytes));
        dps.push_back(dp);
    }
    return dps;
}

class BruteForceScanner {
public:
    explicit BruteForceScanner(const std::vector<sul::DataPoint>& data) : data_(data) {}

    const sul::DataPoint* point_query(const int32_t* coords, int32_t dim_count) const {
        for (const auto& dp : data_) {
            bool match = true;
            for (int32_t d = 0; d < dim_count; ++d) {
                if (dp.dimensions[d] != coords[d]) { match = false; break; }
            }
            if (match) return &dp;
        }
        return nullptr;
    }

    std::vector<const sul::DataPoint*> range_query(
        const int32_t* low, const int32_t* high, int32_t dim_count) const {
        std::vector<const sul::DataPoint*> res;
        for (const auto& dp : data_) {
            bool in_rect = true;
            for (int32_t d = 0; d < dim_count; ++d) {
                if (dp.dimensions[d] < low[d] || dp.dimensions[d] > high[d]) {
                    in_rect = false; break;
                }
            }
            if (in_rect) res.push_back(&dp);
        }
        return res;
    }

private:
    const std::vector<sul::DataPoint>& data_;
};

}

int main(int argc, char** argv) {
    std::string csv_path = "/path/to/SUL-index/uniform_20000_1_2_.csv";
    if (argc > 1) csv_path = argv[1];

    std::cout << "===== SUL-plain-index Demo =====\n";
    std::cout << "[load] csv: " << csv_path << "\n";

    std::vector<RawPoint> raw;
    if (!load_csv(csv_path, raw)) return 1;
    std::cout << "[load] read " << raw.size() << " rows\n";

    std::vector<sul::DataPoint> dps = make_data_points(raw);

    sul::IndexConfig cfg;
    cfg.dim_count = 2;
    if (argc > 2) {
        cfg.error_bound = std::max<int32_t>(1, std::atoi(argv[2]));
    } else {
        cfg.error_bound = std::max<int32_t>(8, static_cast<int32_t>(dps.size()) / 1000);
    }
    cfg.max_layers = 16;
    std::cout << "[cfg]  error_bound=" << cfg.error_bound
              << " dim=" << cfg.dim_count << "\n";

    sul::SULPlainIndex index(cfg);

    std::vector<sul::DataPoint> dps_copy = dps;
    auto t0 = std::chrono::steady_clock::now();
    index.bulk_load(std::move(dps_copy));
    auto t1 = std::chrono::steady_clock::now();
    double build_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::cout << "[build] total_points=" << index.total_points()
              << " leaves=" << index.leaf_count()
              << " inner_layers=" << index.inner_layer_count()
              << "\n";
    std::cout << "[build] learning_filled=" << index.learning_layer_filled()
              << " art_points=" << index.art_layer_points()
              << " art_inner_nodes=" << index.art_total_inner_nodes()
              << "\n";
    std::cout << "[build] elapsed " << build_ms << " ms\n";

    BruteForceScanner scanner(dps);

    std::mt19937 rng(42);
    std::uniform_int_distribution<size_t> pick(0, dps.size() - 1);

    int32_t pq_total = 0, pq_hit = 0, pq_recall_ok = 0;
    const int32_t N_PQ = 500;
    auto pq_t0 = std::chrono::steady_clock::now();
    for (int32_t k = 0; k < N_PQ; ++k) {
        const sul::DataPoint& q = dps[pick(rng)];
        int32_t coords[2] = { q.dimensions[0], q.dimensions[1] };
        sul::DataPoint* found = index.point_query(coords);
        const sul::DataPoint* gt = scanner.point_query(coords, 2);
        ++pq_total;
        if (found) ++pq_hit;
        if ((found == nullptr && gt == nullptr) ||
            (found != nullptr && gt != nullptr
             && found->dimensions[0] == gt->dimensions[0]
             && found->dimensions[1] == gt->dimensions[1])) {
            ++pq_recall_ok;
        }
    }
    auto pq_t1 = std::chrono::steady_clock::now();
    double pq_ms = std::chrono::duration<double, std::milli>(pq_t1 - pq_t0).count();

    std::cout << "\n----- Point Query -----\n";
    std::cout << "[pq] queries=" << pq_total
              << " hits=" << pq_hit
              << " matches_ground_truth=" << pq_recall_ok
              << "\n";
    std::cout << "[pq] avg latency = " << (pq_ms * 1000.0 / pq_total) << " us\n";

    int32_t miss_total = 0, miss_correct = 0;
    for (int32_t k = 0; k < 200; ++k) {
        int32_t coords[2] = {
            static_cast<int32_t>(rng() & 0x7FFFFFFF),
            static_cast<int32_t>(rng() & 0x7FFFFFFF)
        };
        sul::DataPoint* found = index.point_query(coords);
        const sul::DataPoint* gt = scanner.point_query(coords, 2);
        ++miss_total;
        if (found == nullptr && gt == nullptr) ++miss_correct;
    }
    std::cout << "[pq-miss] random_misses=" << miss_total
              << " correctly_null=" << miss_correct << "\n";

    std::cout << "\n----- Range Query -----\n";
    int32_t rq_total = 0;
    size_t rq_returned_sum = 0;
    size_t rq_gt_sum = 0;
    size_t rq_tp_sum = 0;
    // 顺序累计：墙钟总耗时；并行模拟累计：(墙钟 - ART总耗时 + ART最大耗时)
    double rq_wall_us_sum = 0.0;
    double rq_parallel_us_sum = 0.0;
    int64_t rq_art_count_sum = 0;

    const int32_t N_RQ = 50;
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    for (int32_t k = 0; k < N_RQ; ++k) {
        double cx = u01(rng);
        double cy = u01(rng);
        double half = 0.05;
        double x_lo = std::max(0.0, cx - half);
        double y_lo = std::max(0.0, cy - half);
        double x_hi = std::min(1.0 - 1e-9, cx + half);
        double y_hi = std::min(1.0 - 1e-9, cy + half);

        int32_t lo[2] = {
            sul::scale_unit_double_to_int32(x_lo),
            sul::scale_unit_double_to_int32(y_lo)
        };
        int32_t hi[2] = {
            sul::scale_unit_double_to_int32(x_hi),
            sul::scale_unit_double_to_int32(y_hi)
        };

        sul::RangeQueryStats stats;
        auto rq_one_t0 = std::chrono::steady_clock::now();
        auto idx_res = index.range_query(lo, hi, &stats);
        auto rq_one_t1 = std::chrono::steady_clock::now();
        double wall_us = std::chrono::duration<double, std::micro>(rq_one_t1 - rq_one_t0).count();
        // 并行模拟：把ART顺序累加耗时替换为ART最大单次耗时
        double parallel_us = wall_us - stats.art_total_us + stats.art_max_us;

        auto gt_res = scanner.range_query(lo, hi, 2);

        std::unordered_set<uint64_t> truth_set;
        truth_set.reserve(gt_res.size() * 2);
        for (auto* p : gt_res) {
            uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(p->dimensions[0])) << 32)
                         | static_cast<uint32_t>(p->dimensions[1]);
            truth_set.insert(key);
        }
        size_t tp = 0;
        for (auto* p : idx_res) {
            uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(p->dimensions[0])) << 32)
                         | static_cast<uint32_t>(p->dimensions[1]);
            if (truth_set.count(key)) ++tp;
        }

        rq_total++;
        rq_returned_sum    += idx_res.size();
        rq_gt_sum          += gt_res.size();
        rq_tp_sum          += tp;
        rq_wall_us_sum     += wall_us;
        rq_parallel_us_sum += parallel_us;
        rq_art_count_sum   += stats.art_count;
    }

    double recall = rq_gt_sum > 0 ? (double)rq_tp_sum / (double)rq_gt_sum : 1.0;
    double precision = rq_returned_sum > 0 ? (double)rq_tp_sum / (double)rq_returned_sum : 1.0;

    std::cout << "[rq] queries=" << rq_total
              << " avg_returned=" << ((double)rq_returned_sum / rq_total)
              << " avg_truth=" << ((double)rq_gt_sum / rq_total)
              << "\n";
    std::cout << "[rq] recall=" << recall
              << " precision=" << precision
              << "\n";
    std::cout << "[rq] avg ART ops per query = "
              << (static_cast<double>(rq_art_count_sum) / rq_total) << "\n";
    std::cout << "[rq] avg latency (sequential) = "
              << (rq_wall_us_sum / rq_total) << " us\n";
    std::cout << "[rq] avg latency (ART并行模拟) = "
              << (rq_parallel_us_sum / rq_total) << " us\n";

    // ============= 插入测试 =============
    // 测试目标：覆盖三种插入场景
    //   场景A：插入学习层（预测槽位为空）—— 用全新随机坐标
    //   场景B：插入ART层（预测槽位已占）—— 用已有点坐标的副本
    //   场景C：触发ART节点扩容（Node4→16 / 16→48 / 48→256）
    //          —— 大量重复/相近坐标插入会让冲突叶子ART持续增长
    std::cout << "\n----- Insertion Test -----\n";

    // 插入前快照
    size_t learn_before = index.learning_layer_filled();
    size_t art_pts_before = index.art_layer_points();
    size_t art_inner_before = index.art_total_inner_nodes();
    size_t exp4_before = index.art_total_expand_4_to_16();
    size_t exp16_before = index.art_total_expand_16_to_48();
    size_t exp48_before = index.art_total_expand_48_to_256();

    // 准备插入数据：1500 个全新随机点 + 1500 个已有坐标副本
    const int32_t N_INS_NEW = 1500;
    const int32_t N_INS_DUP = 1500;
    std::vector<std::array<int32_t, 2>> ins_coords;
    ins_coords.reserve(N_INS_NEW + N_INS_DUP);

    std::uniform_real_distribution<double> u01_ins(0.0, 1.0);
    for (int32_t k = 0; k < N_INS_NEW; ++k) {
        int32_t cx = sul::scale_unit_double_to_int32(u01_ins(rng));
        int32_t cy = sul::scale_unit_double_to_int32(u01_ins(rng));
        ins_coords.push_back({cx, cy});
    }
    for (int32_t k = 0; k < N_INS_DUP; ++k) {
        const sul::DataPoint& q = dps[pick(rng)];
        ins_coords.push_back({q.dimensions[0], q.dimensions[1]});
    }

    // 执行插入并按层级计数
    int32_t cnt_learn = 0, cnt_art = 0, cnt_fail = 0;
    auto ins_t0 = std::chrono::steady_clock::now();
    for (const auto& c : ins_coords) {
        int32_t coords[2] = { c[0], c[1] };
        sul::InsertResult r = index.insert(coords);
        if (r == sul::InsertResult::LearningLayer)      ++cnt_learn;
        else if (r == sul::InsertResult::ARTLayer)      ++cnt_art;
        else                                            ++cnt_fail;
    }
    auto ins_t1 = std::chrono::steady_clock::now();
    double ins_us_total = std::chrono::duration<double, std::micro>(ins_t1 - ins_t0).count();

    std::cout << "[ins] total=" << ins_coords.size()
              << " (new=" << N_INS_NEW << " dup=" << N_INS_DUP << ")\n";
    std::cout << "[ins] learning_layer_inserts=" << cnt_learn
              << " art_layer_inserts=" << cnt_art
              << " failed=" << cnt_fail << "\n";
    std::cout << "[ins] avg insert latency = "
              << (ins_us_total / ins_coords.size()) << " us/point\n";

    // 插入后快照对比
    std::cout << "[ins] learning_filled "  << learn_before
              << " -> " << index.learning_layer_filled()
              << "  (+ " << (index.learning_layer_filled() - learn_before) << ")\n";
    std::cout << "[ins] art_points "       << art_pts_before
              << " -> " << index.art_layer_points()
              << "  (+ " << (index.art_layer_points() - art_pts_before) << ")\n";
    std::cout << "[ins] art_inner_nodes "  << art_inner_before
              << " -> " << index.art_total_inner_nodes()
              << "  (+ " << (index.art_total_inner_nodes() - art_inner_before) << ")\n";
    std::cout << "[ins] ART expansions: "
              << "N4->N16="    << (index.art_total_expand_4_to_16()   - exp4_before)
              << ", N16->N48="  << (index.art_total_expand_16_to_48()  - exp16_before)
              << ", N48->N256=" << (index.art_total_expand_48_to_256() - exp48_before)
              << "\n";

    // 插入后查询：用相同坐标做点查询，统计命中率与平均查询延迟
    int32_t q_total = 0, q_hit = 0;
    auto pq2_t0 = std::chrono::steady_clock::now();
    for (const auto& c : ins_coords) {
        int32_t coords[2] = { c[0], c[1] };
        sul::DataPoint* found = index.point_query(coords);
        ++q_total;
        if (found) ++q_hit;
    }
    auto pq2_t1 = std::chrono::steady_clock::now();
    double pq2_us_total = std::chrono::duration<double, std::micro>(pq2_t1 - pq2_t0).count();

    std::cout << "[ins-pq] queries=" << q_total
              << " hits=" << q_hit
              << " hit_rate=" << (static_cast<double>(q_hit) / q_total) << "\n";
    std::cout << "[ins-pq] avg query latency = "
              << (pq2_us_total / q_total) << " us/point\n";

    std::cout << "\n[done]\n";
    return 0;
}
