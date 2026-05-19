#include "sul/util/csv_loader.h"

#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>

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

CsvLoadResult load_csv(const std::string& path, int32_t scale) {
    std::ifstream ifs(path);
    if (!ifs) {
        throw std::runtime_error("load_csv: cannot open " + path);
    }

    CsvLoadResult result;
    result.dim_count = 0;
    result.data.reserve(1024);

    const double  scale_d   = static_cast<double>(scale);
    const int32_t scale_max = scale - 1;

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
        if (result.dim_count == 0) {
            if (dim < 1 || dim > MAX_DIMS) {
                throw std::runtime_error("load_csv: dim_count=" + std::to_string(dim)
                                         + " out of [1," + std::to_string(MAX_DIMS) + "]");
            }
            result.dim_count = dim;
        } else if (dim != result.dim_count) {
            throw std::runtime_error("load_csv: line " + std::to_string(line_no)
                                     + " has dim_count=" + std::to_string(dim)
                                     + " expected " + std::to_string(result.dim_count));
        }

        DataPoint dp{};
        dp.dim_count = dim;
        try {
            for (int32_t d = 0; d < dim; ++d) {
                double v = std::stod(fields[d]);
                int32_t iv = static_cast<int32_t>(std::floor(v * scale_d));
                if (iv < 0)         iv = 0;
                if (iv > scale_max) iv = scale_max;
                dp.dimensions[d] = iv;
            }
            dp.orig_id = static_cast<int32_t>(std::stol(fields[dim]));
        } catch (const std::exception& e) {
            throw std::runtime_error("load_csv: line " + std::to_string(line_no)
                                     + " parse error: " + e.what());
        }
        result.data.push_back(dp);
    }

    if (result.data.empty()) {
        throw std::runtime_error("load_csv: no data rows in " + path);
    }
    return result;
}

} // namespace sul::util
