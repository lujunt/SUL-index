#pragma once

#include <string>
#include <vector>

namespace sul::util {

// 实验参数标记。文件命名拼接：
//   <kind>_K{K}_err{err}_dim{dim}_N{N}{extra}.csv
// extra 由调用者按实验类型自定义，例如：
//   range  → "_sl0p25"
//   work   → "_R80W20"
//   update → "_ul0p25"
struct ExpParams {
    int K   = 0;
    int err = 0;
    int dim = 0;
    int N   = 0;
    std::string extra;
};

class ExperimentRecorder {
public:
    // 生成 record/{kind}_K..._err..._dim..._N...{extra}.csv 绝对路径
    // record_dir 为空时回退到 ./record/（相对当前工作目录）
    static std::string build_path(const std::string& kind,
                                  const ExpParams& p,
                                  const std::string& record_dir = "");

    // 追加一行；若文件不存在则先创建并写入 header
    // header 与 row 必须长度一致
    static void append_row(const std::string& path,
                           const std::vector<std::string>& header,
                           const std::vector<std::string>& row);

    // ISO-8601 局部时间戳，例如 2026-05-22T15:03:01
    static std::string now_iso();

    // 浮点 → 字符串（避免科学计数）
    static std::string ftoa(double v);

    // 小数 → 文件名安全标记
    //   pct_tag(0.25) -> "0p25"
    static std::string pct_tag(double pct);
};

} // namespace sul::util
