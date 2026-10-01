#pragma once

#include "sul/types.h"

#include <string>
#include <vector>

namespace sul::util {

// CSV format (as in uniform_20000_1_2_.csv):
//   dim_1, dim_2, ..., dim_N, id
// Coordinates are [0, 1) floats and id is an integer. Empty and comment lines are ignored.
//
// dim_count is detected from the first valid row. Values are normalized per dimension
// by default; update experiments must reuse base.normalization.
struct CsvNormalization {
    std::vector<double> low;
    std::vector<double> high;
    int32_t scale = 65536;
};

struct CsvLoadResult {
    std::vector<DataPoint> data;
    int32_t                dim_count = 0;
    CsvNormalization       normalization;
};

// Throws std::runtime_error on failure.
CsvLoadResult load_csv(const std::string& path, int32_t scale = 65536);

// Reuse an existing coordinate mapping; reject out-of-domain values instead of rescaling or clipping.
CsvLoadResult load_csv(const std::string& path, const CsvNormalization& normalization);

} // namespace sul::util
