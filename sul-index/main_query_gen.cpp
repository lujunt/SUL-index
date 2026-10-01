// Query-window generator.
// Usage: ./sul_query_gen <dataset_csv> [n_queries=100] [output_dir=query] [--target-hits]
// Generates five selectivity files (0.25%, 0.5%, 1%, 2%, and 4%) named
// <stem>_dim{d}_<ratio_pct>.csv.
//
// --target-hits binary-searches edge length for N*ratio hits within 5%, which is useful
// for skewed data. The default derives edge length from uniform volume.

#include "sul/util/query_loader.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    bool target_hits_mode = false;
    uint32_t seed = 42;
    std::vector<std::string> pos;
    pos.reserve(static_cast<size_t>(argc));
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--target-hits") target_hits_mode = true;
        else if (a == "--seed" && i + 1 < argc) {
            try {
                std::string value = argv[++i];
                size_t used = 0;
                auto parsed = std::stoull(value, &used);
                if (used != value.size() || value.empty() || value[0] == '-' || parsed > UINT32_MAX)
                    throw std::runtime_error("invalid seed");
                seed = static_cast<uint32_t>(parsed);
            } catch (const std::exception&) {
                std::cerr << "[error] --seed requires uint32\n";
                return 2;
            }
        } else if (a.rfind("--", 0) == 0) {
            std::cerr << "[error] unknown option: " << a << "\n";
            return 2;
        } else pos.push_back(std::move(a));
    }

    if (pos.empty()) {
        std::cerr << "Usage: " << argv[0]
                  << " <dataset_csv> [n_queries=100] [output_dir=query] [--target-hits] [--seed 42]\n"
                  << "  --target-hits: binary-search edge for N*ratio hits (recommended for skewed data);\n"
                  << "                 default: edge=ratio^(1/dim) from uniform volume\n";
        return 1;
    }
    const std::string dataset_path = pos[0];
    const int32_t     n_queries    = (pos.size() > 1) ? std::atoi(pos[1].c_str()) : 100;
    const std::string output_dir   = (pos.size() > 2) ? pos[2] : "query";

    std::cout << "=== Query window generator ===\n"
              << "  dataset    = " << dataset_path << "\n"
              << "  n_queries  = " << n_queries << "\n"
              << "  output_dir = " << output_dir << "\n"
              << "  mode       = "
              << (target_hits_mode ? "target_hits (adaptive binary search)" : "uniform_volume")
              << "\n";

    try {
        if (n_queries <= 0 || n_queries > 1000000 || pos.size() > 3)
            throw std::runtime_error("invalid query count or extra arguments");
        size_t written = sul::util::generate_query_files(
            dataset_path, output_dir, n_queries, seed, /*scale=*/65536,
            target_hits_mode);
        std::cout << "  Wrote " << written << " query files to " << output_dir << "/\n";
    } catch (const std::exception& e) {
        std::cerr << "[error] " << e.what() << "\n";
        return 2;
    }
    return 0;
}
