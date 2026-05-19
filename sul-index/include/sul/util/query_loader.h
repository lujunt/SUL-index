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

// 生成 5 个查询窗口文件到 output_dir
// 规则：
//   - 比例固定 {0.25%, 0.5%, 1%, 2%, 4%}
//   - 每个文件含 n_queries 条查询
//   - 每条查询：从 dataset 中随机抽 1 点作为中心，
//     窗口边长 = pow(ratio_fraction, 1/dim) 使预期返回点数 ≈ N × ratio（uniform 假设）
//   - 文件名：<stem>_<ratio_pct>.csv
//     stem 由 dataset 文件名前两个下划线分量构成，例：uniform_20000_1_2_.csv → uniform_20000
//   - 写入格式与 load_query_file 兼容（浮点 [0,1)）
//
// 返回成功写入的文件数（正常为 5）
size_t generate_query_files(const std::string& dataset_path,
                            const std::string& output_dir,
                            int32_t            n_queries = 100,
                            uint32_t           seed      = 42,
                            int32_t            scale     = 65536);

} // namespace sul::util
