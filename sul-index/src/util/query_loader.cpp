#include "sul/util/query_loader.h"
#include "sul/util/csv_loader.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>

namespace sul::util {

namespace {

std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> fields;
    std::string cur;
    for (char c : line) {
        if (c == ',') { fields.push_back(std::move(cur)); cur.clear(); }
        else if (c != '\r') cur.push_back(c);
    }
    fields.push_back(std::move(cur));
    for (auto& f : fields) {
        size_t a = f.find_first_not_of(" \t");
        size_t b = f.find_last_not_of(" \t");
        if (a == std::string::npos) { f.clear(); continue; }
        f = f.substr(a, b - a + 1);
    }
    return fields;
}

bool is_skip(const std::string& line) {
    for (char c : line) {
        if (c == ' ' || c == '\t' || c == '\r') continue;
        return c == '#';
    }
    return true;
}

// 0.25 → "0.25"  0.5 → "0.5"  1.0 → "1"  2.0 → "2"  4.0 → "4"
std::string format_ratio_pct(double pct) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", pct);
    return buf;
}

// 给定 center 与 edge，计算每维平移后的窗口 [lo, hi]（均归一化到 [0,1]）
// 输出与最终写盘格式严格一致，count_in_window 复用此 helper 保证统计自洽
void compute_window(const DataPoint& center, int32_t DIM,
                    double edge, double scale_d,
                    std::vector<double>& lo_v,
                    std::vector<double>& hi_v) {
    const double half = edge / 2.0;
    lo_v.assign(DIM, 0.0);
    hi_v.assign(DIM, 0.0);
    for (int32_t d = 0; d < DIM; ++d) {
        double c  = static_cast<double>(center.dimensions[d]) / scale_d;
        double lo = c - half;
        double hi = c + half;
        if (lo < 0.0) { lo = 0.0; hi = std::min(1.0, edge); }
        if (hi > 1.0) { hi = 1.0; lo = std::max(0.0, 1.0 - edge); }
        lo_v[d] = lo;
        hi_v[d] = hi;
    }
}

// 暴力扫数据集，统计落入闭区间 [lo, hi]^DIM 的点数（坐标归一化到 [0,1]）
int32_t count_in_window(const std::vector<DataPoint>& data,
                        const std::vector<double>& lo_v,
                        const std::vector<double>& hi_v,
                        int32_t DIM, double scale_d) {
    const double inv_scale = 1.0 / scale_d;
    int32_t count = 0;
    for (const auto& dp : data) {
        bool inside = true;
        for (int32_t d = 0; d < DIM; ++d) {
            double pt = static_cast<double>(dp.dimensions[d]) * inv_scale;
            if (pt < lo_v[d] || pt > hi_v[d]) { inside = false; break; }
        }
        if (inside) ++count;
    }
    return count;
}

// 二分搜索 edge，使 count_in_window 落入 [target*(1-tol), target*(1+tol)]
// 30 轮收敛失败则返回最接近 target 的 edge（best-effort + converged=false）
struct BisectResult { double edge; int32_t hits; bool converged; };

BisectResult bisect_edge_for_target(const std::vector<DataPoint>& data,
                                    const DataPoint& center,
                                    int32_t DIM, double scale_d,
                                    int32_t target, double tol,
                                    double edge_init,
                                    int max_iters = 30) {
    const int32_t lo_count = std::max(1,
        static_cast<int32_t>(std::floor(target * (1.0 - tol))));
    const int32_t hi_count = std::max(lo_count,
        static_cast<int32_t>(std::ceil(target * (1.0 + tol))));

    double edge_lo = 0.0, edge_hi = 1.0;
    double edge    = std::clamp(edge_init, 1e-9, 1.0);

    int32_t best_diff = std::numeric_limits<int32_t>::max();
    double  best_edge = edge;
    int32_t best_hits = -1;

    std::vector<double> lv, hv;
    for (int it = 0; it < max_iters; ++it) {
        compute_window(center, DIM, edge, scale_d, lv, hv);
        int32_t hits = count_in_window(data, lv, hv, DIM, scale_d);

        int32_t diff = std::abs(hits - target);
        if (diff < best_diff) { best_diff = diff; best_edge = edge; best_hits = hits; }

        if (hits >= lo_count && hits <= hi_count) {
            return {edge, hits, true};
        }
        if (hits > hi_count) edge_hi = edge;
        else                  edge_lo = edge;
        edge = (edge_lo + edge_hi) / 2.0;
    }
    return {best_edge, best_hits, false};
}

} // namespace

QueryFile load_query_file(const std::string& path, int32_t scale) {
    std::ifstream ifs(path);
    if (!ifs) throw std::runtime_error("load_query_file: cannot open " + path);

    QueryFile result;
    const double  scale_d   = static_cast<double>(scale);
    const int32_t scale_max = scale - 1;

    std::string line;
    size_t line_no = 0;
    while (std::getline(ifs, line)) {
        ++line_no;
        if (is_skip(line)) continue;
        auto fields = split_csv(line);

        if (fields.size() < 2 || fields.size() % 2 != 0) {
            throw std::runtime_error("load_query_file: line " + std::to_string(line_no)
                                     + " has " + std::to_string(fields.size())
                                     + " fields (expected even >= 2)");
        }
        int32_t dim = static_cast<int32_t>(fields.size()) / 2;
        if (result.dim_count == 0) {
            if (dim < 1 || dim > MAX_DIMS) {
                throw std::runtime_error("load_query_file: dim_count="
                                         + std::to_string(dim) + " out of range");
            }
            result.dim_count = dim;
        } else if (dim != result.dim_count) {
            throw std::runtime_error("load_query_file: line " + std::to_string(line_no)
                                     + " dim mismatch");
        }

        QueryRect q;
        q.lo.resize(dim);
        q.hi.resize(dim);
        try {
            for (int32_t d = 0; d < dim; ++d) {
                double lo = std::stod(fields[d]);
                double hi = std::stod(fields[dim + d]);
                if (!std::isfinite(lo) || !std::isfinite(hi) || lo < 0 || lo > 1 || hi < 0 || hi > 1)
                    throw std::runtime_error("query coordinates must be finite and in [0,1]");
                int32_t ilo = static_cast<int32_t>(std::floor(lo * scale_d));
                int32_t ihi = static_cast<int32_t>(std::floor(hi * scale_d));
                if (ilo < 0)         ilo = 0;
                if (ihi > scale_max) ihi = scale_max;
                if (ilo > ihi)       std::swap(ilo, ihi);
                q.lo[d] = ilo;
                q.hi[d] = ihi;
            }
        } catch (const std::exception& e) {
            throw std::runtime_error("load_query_file: line " + std::to_string(line_no)
                                     + " parse error: " + e.what());
        }
        result.queries.push_back(std::move(q));
    }

    if (result.queries.empty()) {
        throw std::runtime_error("load_query_file: no data in " + path);
    }
    return result;
}

std::string dataset_stem(const std::string& dataset_path) {
    auto slash = dataset_path.find_last_of("/\\");
    std::string fname = (slash == std::string::npos) ? dataset_path
                                                     : dataset_path.substr(slash + 1);
    auto dot = fname.find_last_of('.');
    std::string stem = (dot == std::string::npos) ? fname : fname.substr(0, dot);

    auto u1 = stem.find('_');
    if (u1 == std::string::npos) return stem;
    auto u2 = stem.find('_', u1 + 1);
    if (u2 == std::string::npos) return stem;
    return stem.substr(0, u2);
}

size_t generate_query_files(const std::string& dataset_path,
                            const std::string& output_dir,
                            int32_t            n_queries,
                            uint32_t           seed,
                            int32_t            scale,
                            bool               target_hits_mode) {
    CsvLoadResult ds = load_csv(dataset_path, scale);
    const int32_t N   = static_cast<int32_t>(ds.data.size());
    const int32_t DIM = ds.dim_count;
    if (N < 1) throw std::runtime_error("generate_query_files: empty dataset");

    const double scale_d = static_cast<double>(scale);
    const std::string stem = dataset_stem(dataset_path);

    const std::vector<double> RATIO_PCTS = {0.25, 0.5, 1.0, 2.0, 4.0};
    constexpr double TOL = 0.05;  // ±5% 容差
    size_t written = 0;
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int32_t> pick(0, N - 1);

    for (double pct : RATIO_PCTS) {
        const double ratio_frac   = pct / 100.0;
        const double edge_uniform = std::pow(ratio_frac, 1.0 / static_cast<double>(DIM));
        const int32_t target = std::max(1,
            static_cast<int32_t>(std::round(ratio_frac * N)));

        std::string out_path = output_dir;
        if (!out_path.empty() && out_path.back() != '/' && out_path.back() != '\\') {
            out_path.push_back('/');
        }
        // 命名：<stem>_dim{D}_<ratio>.csv
        // 维度放中间——下游 parse_sl_pct_from_path 取最后 "_" 后内容，仍能正确解析 ratio
        out_path += stem + "_dim" + std::to_string(DIM)
                  + "_" + format_ratio_pct(pct) + ".csv";

        // 先把所有窗口算好（target_hits 模式下要先跑完二分才能写完整 header）
        std::vector<std::vector<double>> all_lo, all_hi;
        all_lo.reserve(n_queries);
        all_hi.reserve(n_queries);

        int64_t hits_sum = 0;
        int32_t hits_min = std::numeric_limits<int32_t>::max();
        int32_t hits_max = 0;
        int32_t converged_cnt = 0;
        int32_t skipped_cnt   = 0;

        // target_hits 模式：未收敛窗口 skip 重试，直到收齐 n_queries 或达到 attempts 上限
        // uniform_volume 模式：每次循环必收（无验证），等同于原 for-loop
        const int32_t MAX_ATTEMPTS = target_hits_mode
            ? n_queries * 30   // 30:1 重试预算
            : n_queries;
        int32_t attempts = 0;

        while (static_cast<int32_t>(all_lo.size()) < n_queries
               && attempts < MAX_ATTEMPTS) {
            ++attempts;
            const auto& center = ds.data[pick(rng)];

            double edge_used  = edge_uniform;
            int32_t hits_used = -1;
            if (target_hits_mode) {
                auto br = bisect_edge_for_target(ds.data, center, DIM, scale_d,
                                                  target, TOL, edge_uniform);
                if (!br.converged) {
                    ++skipped_cnt;
                    continue;  // ← 实测命中数偏离容差，丢弃该窗口
                }
                edge_used = br.edge;
                hits_used = br.hits;
                ++converged_cnt;
            }

            std::vector<double> lv, hv;
            compute_window(center, DIM, edge_used, scale_d, lv, hv);

            if (target_hits_mode) {
                hits_sum += hits_used;
                hits_min = std::min(hits_min, hits_used);
                hits_max = std::max(hits_max, hits_used);
            }
            all_lo.push_back(std::move(lv));
            all_hi.push_back(std::move(hv));
        }

        if (target_hits_mode
            && static_cast<int32_t>(all_lo.size()) < n_queries) {
            std::cerr << "[error] " << out_path << ": 尝试 " << attempts
                      << " 次仅收齐 " << all_lo.size() << "/" << n_queries
                      << " 个合规窗口（skipped=" << skipped_cnt
                      << "）。建议放宽 tol 或换数据集。\n";
        }

        if (static_cast<int32_t>(all_lo.size()) != n_queries)
            throw std::runtime_error("generate_query_files: insufficient valid queries");
        std::ofstream ofs(out_path);
        if (!ofs) throw std::runtime_error("generate_query_files: cannot write " + out_path);

        // # 注释头（load 时会跳过）
        ofs << "# dataset=" << dataset_path
            << " N=" << N
            << " dim=" << DIM
            << " ratio_pct=" << pct
            << " mode=" << (target_hits_mode ? "target_hits" : "uniform_volume")
            << " seed=" << seed
            << " n_queries=" << n_queries;
        if (target_hits_mode) {
            const double hit_mean =
                static_cast<double>(hits_sum) / static_cast<double>(n_queries);
            ofs << " target=" << target
                << " tol=" << TOL
                << " hit_min=" << hits_min
                << " hit_mean=" << hit_mean
                << " hit_max=" << hits_max
                << " converged=" << converged_cnt << "/" << n_queries
                << " skipped=" << skipped_cnt
                << " attempts=" << attempts;
        } else {
            ofs << " edge=" << edge_uniform;
        }
        ofs << "\n";

        for (size_t q = 0; q < all_lo.size(); ++q) {
            for (int32_t d = 0; d < DIM; ++d) {
                if (d > 0) ofs << ',';
                ofs << all_lo[q][d];
            }
            for (int32_t d = 0; d < DIM; ++d) {
                ofs << ',' << all_hi[q][d];
            }
            ofs << '\n';
        }
        ++written;
    }
    return written;
}

} // namespace sul::util
