#include "sul/util/csv_loader.h"

#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace sul::util {

namespace {

std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> fields;
    std::string cur;
    cur.reserve(32);
    for (char c : line) {
        if (c == ',') {
            fields.push_back(std::move(cur));
            cur.clear();
        } else if (c != '\r') {
            cur.push_back(c);
        }
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

} // namespace

// 两遍扫描：先求每维 min/max，再按列 min-max 归一化到 [0, scale-1] 整数空间。
// 充分利用 BITS_PER_DIM 精度，避免亚整数级浮点数据被整数化坍缩到同一整数桶。
static CsvLoadResult load_csv_impl(const std::string& path, int32_t scale,
                                   const CsvNormalization* fixed) {
    if (scale < 2) throw std::runtime_error("load_csv: scale must be >= 2");
    std::ifstream ifs(path);
    if (!ifs) {
        throw std::runtime_error("load_csv: cannot open " + path);
    }

    // pass 1：读全部浮点行，统计 min/max
    std::vector<std::vector<double>> raw;
    std::vector<int32_t>             ids;
    raw.reserve(1024);
    ids.reserve(1024);
    int32_t dim_count = 0;

    std::vector<double> min_v, max_v;

    std::string line;
    size_t line_no = 0;
    while (std::getline(ifs, line)) {
        ++line_no;
        if (is_skip(line)) continue;

        auto fields = split_csv(line);
        if (fields.size() < 2) {
            throw std::runtime_error("load_csv: line " + std::to_string(line_no)
                                     + " has < 2 fields");
        }

        int32_t dim = static_cast<int32_t>(fields.size()) - 1;
        if (dim_count == 0) {
            if (dim < 1 || dim > MAX_DIMS) {
                throw std::runtime_error("load_csv: dim_count=" + std::to_string(dim)
                                         + " out of [1," + std::to_string(MAX_DIMS) + "]");
            }
            dim_count = dim;
            min_v.assign(dim,  std::numeric_limits<double>::infinity());
            max_v.assign(dim, -std::numeric_limits<double>::infinity());
        } else if (dim != dim_count) {
            throw std::runtime_error("load_csv: line " + std::to_string(line_no)
                                     + " has dim_count=" + std::to_string(dim)
                                     + " expected " + std::to_string(dim_count));
        }

        std::vector<double> row(dim);
        try {
            for (int32_t d = 0; d < dim; ++d) {
                size_t used = 0;
                double v = std::stod(fields[d], &used);
                if (used != fields[d].size()) throw std::runtime_error("invalid coordinate suffix");
                if (!std::isfinite(v)) throw std::runtime_error("non-finite coordinate");
                row[d] = v;
                if (v < min_v[d]) min_v[d] = v;
                if (v > max_v[d]) max_v[d] = v;
            }
            size_t used = 0;
            auto id = std::stoll(fields[dim], &used);
            if (used != fields[dim].size() || id < INT32_MIN || id > INT32_MAX)
                throw std::runtime_error("invalid int32 record ID");
            ids.push_back(static_cast<int32_t>(id));
        } catch (const std::exception& e) {
            throw std::runtime_error("load_csv: line " + std::to_string(line_no)
                                     + " parse error: " + e.what());
        }
        raw.push_back(std::move(row));
    }

    if (raw.empty()) {
        throw std::runtime_error("load_csv: no data rows in " + path);
    }

    if (fixed) {
        if (fixed->low.size() != static_cast<size_t>(dim_count) ||
            fixed->high.size() != static_cast<size_t>(dim_count))
            throw std::runtime_error("load_csv: normalization dimension mismatch");
        min_v = fixed->low;
        max_v = fixed->high;
        for (int32_t d = 0; d < dim_count; ++d)
            if (!std::isfinite(min_v[d]) || !std::isfinite(max_v[d]) || min_v[d] > max_v[d])
                throw std::runtime_error("load_csv: invalid normalization bounds");
    }

    // pass 2：按列 min-max 归一化到 [0, scale_max]
    CsvLoadResult result;
    result.dim_count = dim_count;
    result.normalization = {min_v, max_v, scale};
    result.data.reserve(raw.size());

    const double  scale_d   = static_cast<double>(scale);
    const int32_t scale_max = scale - 1;

    std::vector<double> range_v(dim_count);
    for (int32_t d = 0; d < dim_count; ++d) {
        range_v[d] = max_v[d] - min_v[d];
    }

    for (size_t i = 0; i < raw.size(); ++i) {
        DataPoint dp{};
        dp.dim_count = dim_count;
        for (int32_t d = 0; d < dim_count; ++d) {
            if (fixed && (raw[i][d] < min_v[d] || raw[i][d] > max_v[d]))
                throw std::runtime_error("load_csv: point outside base coordinate domain at data row "
                                         + std::to_string(i + 1));
            double norm = (range_v[d] > 0.0)
                ? (raw[i][d] - min_v[d]) / range_v[d]
                : 0.0;
            int32_t iv = static_cast<int32_t>(std::floor(norm * scale_d));
            if (iv < 0)         iv = 0;
            if (iv > scale_max) iv = scale_max;
            dp.dimensions[d] = iv;
        }
        dp.orig_id = ids[i];
        result.data.push_back(dp);
    }

    return result;
}

CsvLoadResult load_csv(const std::string& path, int32_t scale) {
    return load_csv_impl(path, scale, nullptr);
}

CsvLoadResult load_csv(const std::string& path, const CsvNormalization& normalization) {
    return load_csv_impl(path, normalization.scale, &normalization);
}

} // namespace sul::util
