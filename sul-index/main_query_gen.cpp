// 查询窗口生成器
// 用法：./sul_query_gen <dataset_csv> [n_queries=100] [output_dir=query] [--target-hits]
// 自动生成 5 个查询文件（0.25% / 0.5% / 1% / 2% / 4% 选择率），
// 文件名 <stem>_dim{d}_<ratio_pct>.csv，stem 取数据集前两个 _ 分量，dim 取数据集实际维度
//
// --target-hits: 二分搜索 edge 使每条 query 实测命中数 ≈ N × ratio（±5% 容差），
//                适合 skewed 数据；默认按 uniform 体积比反推 edge（在 skewed 上失真）

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
                  << "  --target-hits: 按目标命中数 N×ratio 二分 edge（适合 skewed）；\n"
                  << "                 默认按 uniform 体积比 edge=ratio^(1/dim)\n";
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
              << (target_hits_mode ? "target_hits (二分自适应)" : "uniform_volume (体积比)")
              << "\n";

    try {
        if (n_queries <= 0 || n_queries > 1000000 || pos.size() > 3)
            throw std::runtime_error("invalid query count or extra arguments");
        size_t written = sul::util::generate_query_files(
            dataset_path, output_dir, n_queries, seed, /*scale=*/65536,
            target_hits_mode);
        std::cout << "  写入 " << written << " 个查询文件 → " << output_dir << "/\n";
    } catch (const std::exception& e) {
        std::cerr << "[error] " << e.what() << "\n";
        return 2;
    }
    return 0;
}
