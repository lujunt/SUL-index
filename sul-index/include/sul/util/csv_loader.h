#pragma once

#include "sul/types.h"

#include <string>
#include <vector>

namespace sul::util {

// CSV 文件格式（与 uniform_20000_1_2_.csv 一致）：
//   每行: dim_1, dim_2, ..., dim_N, id
//   dim_i 为 [0,1) 浮点；id 为整数；无表头；空行/以 # 开头的行被忽略
//
// dim_count 在加载时自动检测（首个有效数据行的逗号数 - 1）
// 浮点坐标按 floor(v * scale) 缩放为 int32_t；scale 默认 65536 对应 BITS_PER_DIM=16
struct CsvLoadResult {
    std::vector<DataPoint> data;
    int32_t                dim_count = 0;
};

// 加载失败抛 std::runtime_error
CsvLoadResult load_csv(const std::string& path, int32_t scale = 65536);

} // namespace sul::util
