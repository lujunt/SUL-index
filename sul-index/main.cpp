#include "sul/sul_index.h"
#include "sul/z_order.h"

#include <algorithm>
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
    cfg.error_bound = std::max<int32_t>(8, static_cast<int32_t>(dps.size()) / 1000);
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
    auto rq_t0 = std::chrono::steady_clock::now();

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

        auto idx_res = index.range_query(lo, hi);
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
        rq_returned_sum += idx_res.size();
        rq_gt_sum += gt_res.size();
        rq_tp_sum += tp;
    }
    auto rq_t1 = std::chrono::steady_clock::now();
    double rq_ms = std::chrono::duration<double, std::milli>(rq_t1 - rq_t0).count();

    double recall = rq_gt_sum > 0 ? (double)rq_tp_sum / (double)rq_gt_sum : 1.0;
    double precision = rq_returned_sum > 0 ? (double)rq_tp_sum / (double)rq_returned_sum : 1.0;

    std::cout << "[rq] queries=" << rq_total
              << " avg_returned=" << ((double)rq_returned_sum / rq_total)
              << " avg_truth=" << ((double)rq_gt_sum / rq_total)
              << "\n";
    std::cout << "[rq] recall=" << recall
              << " precision=" << precision
              << "\n";
    std::cout << "[rq] avg latency = " << (rq_ms * 1000.0 / rq_total) << " us\n";

    std::cout << "\n[done]\n";
    return 0;
}
