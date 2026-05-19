#include "sul/util/query_loader.h"
#include "sul/util/csv_loader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>

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

// 提取数据集 stem 的前两个 _ 分量
// 例：uniform_20000_1_2_.csv → uniform_20000
std::string dataset_stem_first_two(const std::string& dataset_path) {
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

// 0.25 → "0.25"  0.5 → "0.5"  1.0 → "1"  2.0 → "2"  4.0 → "4"
std::string format_ratio_pct(double pct) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", pct);
    return buf;
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

size_t generate_query_files(const std::string& dataset_path,
                            const std::string& output_dir,
                            int32_t            n_queries,
                            uint32_t           seed,
                            int32_t            scale) {
    CsvLoadResult ds = load_csv(dataset_path, scale);
    const int32_t N   = static_cast<int32_t>(ds.data.size());
    const int32_t DIM = ds.dim_count;
    if (N < 1) throw std::runtime_error("generate_query_files: empty dataset");

    const double scale_d = static_cast<double>(scale);
    const std::string stem = dataset_stem_first_two(dataset_path);

    const std::vector<double> RATIO_PCTS = {0.25, 0.5, 1.0, 2.0, 4.0};
    size_t written = 0;
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int32_t> pick(0, N - 1);

    for (double pct : RATIO_PCTS) {
        const double ratio_frac = pct / 100.0;
        // 超立方体边长（uniform 假设下选择率 = edge^dim）
        const double edge = std::pow(ratio_frac, 1.0 / static_cast<double>(DIM));
        const double half = edge / 2.0;

        std::string out_path = output_dir;
        if (!out_path.empty() && out_path.back() != '/' && out_path.back() != '\\') {
            out_path.push_back('/');
        }
        out_path += stem + "_" + format_ratio_pct(pct) + ".csv";

        std::ofstream ofs(out_path);
        if (!ofs) throw std::runtime_error("generate_query_files: cannot write " + out_path);

        // # 注释头（load 时会跳过）
        ofs << "# dataset=" << dataset_path
            << " N=" << N
            << " dim=" << DIM
            << " ratio_pct=" << pct
            << " edge=" << edge
            << " n_queries=" << n_queries
            << "\n";

        for (int32_t q = 0; q < n_queries; ++q) {
            const auto& center = ds.data[pick(rng)];
            // 先在 dim 维度上各自计算 lo/hi，再依序写
            std::vector<double> lo_v(DIM), hi_v(DIM);
            for (int32_t d = 0; d < DIM; ++d) {
                double c  = static_cast<double>(center.dimensions[d]) / scale_d;
                double lo = c - half;
                double hi = c + half;
                if (lo < 0.0) { lo = 0.0; hi = std::min(1.0, edge); }
                if (hi > 1.0) { hi = 1.0; lo = std::max(0.0, 1.0 - edge); }
                lo_v[d] = lo;
                hi_v[d] = hi;
            }
            for (int32_t d = 0; d < DIM; ++d) {
                if (d > 0) ofs << ',';
                ofs << lo_v[d];
            }
            for (int32_t d = 0; d < DIM; ++d) {
                ofs << ',' << hi_v[d];
            }
            ofs << '\n';
        }
        ++written;
    }
    return written;
}

} // namespace sul::util
