#pragma once

#include "sul/types.h"

#include <string>
#include <vector>

namespace sul::util {

// One hypercube range-query window.
struct QueryRect {
    std::vector<int32_t> lo;  // size = dim_count
    std::vector<int32_t> hi;
};

struct QueryFile {
    std::vector<QueryRect> queries;
    int32_t                dim_count = 0;
    double                 ratio_pct = 0.0;  // Supplied by the caller from the file name or arguments.
};

// Load query rows: lo_1, ..., lo_N, hi_1, ..., hi_N (2N floats in [0, 1)).
// scale converts floats to int32 using the dataset scale (65,536 for 16 bits by default).
// Throws std::runtime_error on failure.
QueryFile load_query_file(const std::string& path, int32_t scale = 65536);

// Extract the first two underscore-delimited components of a dataset file name.
// Example: datasets/uniform_20000_1_2_.csv -> "uniform_20000".
//     datasets/skewed_20000_4_2_.csv  → "skewed_20000"
//     a.csv                           → "a"
// Provides consistent names across query and index-generation tools.
std::string dataset_stem(const std::string& dataset_path);

// Generate five query-window files under output_dir.
// Rules:
//   - Fixed selectivities: {0.25%, 0.5%, 1%, 2%, 4%}.
//   - Each file contains n_queries queries centered on random dataset points.
//   - target_hits_mode controls edge length:
//     * false (default): edge = pow(ratio_fraction, 1/dim), derived from uniform volume;
//       observed hits on skewed data can differ from the nominal ratio by several times.
//     * true: binary-search the edge for each center until the observed hit count is in
//              [target × (1 - tol), target × (1 + tol)]，target = round(N × ratio)，
//       [target*(1-tol), target*(1+tol)]. tol is 5%; after 30 unsuccessful rounds,
//       retain the closest edge and emit a warning.
//   - File name: <stem>_dim{d}_<ratio_pct>.csv. stem uses the first two dataset-name
//     components, and dim is inferred to prevent cross-dimensional overwrites.
//   - Output uses [0, 1) floats compatible with load_query_file.
//
// Return the number of files written successfully (normally five).
size_t generate_query_files(const std::string& dataset_path,
                            const std::string& output_dir,
                            int32_t            n_queries        = 100,
                            uint32_t           seed             = 42,
                            int32_t            scale            = 65536,
                            bool               target_hits_mode = false);

} // namespace sul::util
