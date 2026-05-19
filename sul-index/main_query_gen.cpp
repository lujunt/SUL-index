// 查询窗口生成器
// 用法：./sul_query_gen <dataset_csv> [n_queries=100] [output_dir=query]
// 自动生成 5 个查询文件（0.25% / 0.5% / 1% / 2% / 4% 选择率），
// 文件名 <stem>_<ratio_pct>.csv，stem 取数据集前两个 _ 分量

#include "sul/util/query_loader.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0]
                  << " <dataset_csv> [n_queries=100] [output_dir=query]\n";
        return 1;
    }
    const std::string dataset_path = argv[1];
    const int32_t     n_queries    = (argc > 2) ? std::atoi(argv[2]) : 100;
    const std::string output_dir   = (argc > 3) ? argv[3] : "query";

    std::cout << "=== Query window generator ===\n";
    std::cout << "  dataset    = " << dataset_path << "\n";
    std::cout << "  n_queries  = " << n_queries << "\n";
    std::cout << "  output_dir = " << output_dir << "\n";

    try {
        size_t written = sul::util::generate_query_files(
            dataset_path, output_dir, n_queries);
        std::cout << "  写入 " << written << " 个查询文件 → " << output_dir << "/\n";
    } catch (const std::exception& e) {
        std::cerr << "[error] " << e.what() << "\n";
        return 2;
    }
    return 0;
}
