#pragma once

#include "sul/types.h"

#include <string>
#include <vector>

namespace sul::util {

// 单个查询窗口（超立方体范围查询）
struct QueryRect {
    std::vector<int32_t> lo;  // size = dim_count
    std::vector<int32_t> hi;
};

struct QueryFile {
    std::vector<QueryRect> queries;
    int32_t                dim_count = 0;
    double                 ratio_pct = 0.0;  // 由调用方按文件名/参数提供
};

// 加载查询文件：每行 lo_1, ..., lo_N, hi_1, ..., hi_N（2N 个 [0,1) 浮点）
// scale: 浮点 → int32 的缩放因子，与 dataset 一致（默认 65536 对应 BITS_PER_DIM=16）
// 加载失败抛 std::runtime_error
QueryFile load_query_file(const std::string& path, int32_t scale = 65536);

// 提取数据集文件名的 stem（前两个 _ 之间的部分）
// 例：datasets/uniform_20000_1_2_.csv → "uniform_20000"
//     datasets/skewed_20000_4_2_.csv  → "skewed_20000"
//     a.csv                           → "a"
// 用于跨工具一致命名：query/<stem>_<ratio>.csv 与 indexes/index_<stem>_K..._N....scidx
std::string dataset_stem(const std::string& dataset_path);

// 生成 5 个查询窗口文件到 output_dir
// 规则：
//   - 比例固定 {0.25%, 0.5%, 1%, 2%, 4%}
//   - 每个文件含 n_queries 条查询
//   - 每条查询：从 dataset 中随机抽 1 点作为中心
//   - 边长选取由 target_hits_mode 决定：
//     · false（默认）：边长 = pow(ratio_fraction, 1/dim)，按 uniform 体积比反推；
//                       在 skewed 数据上实测命中率可与名义比例差几倍
//     · true ：对每个 center 二分搜索 edge，使实测命中数落入
//              [target × (1 - tol), target × (1 + tol)]，target = round(N × ratio)，
//              tol = 5%；30 轮收敛失败则保留最接近的 edge（尽力而为 + 警告）
//   - 文件名：<stem>_<ratio_pct>.csv
//     stem 由 dataset 文件名前两个下划线分量构成，例：uniform_20000_1_2_.csv → uniform_20000
//   - 写入格式与 load_query_file 兼容（浮点 [0,1)）
//
// 返回成功写入的文件数（正常为 5）
size_t generate_query_files(const std::string& dataset_path,
                            const std::string& output_dir,
                            int32_t            n_queries        = 100,
                            uint32_t           seed             = 42,
                            int32_t            scale            = 65536,
                            bool               target_hits_mode = false);

} // namespace sul::util
