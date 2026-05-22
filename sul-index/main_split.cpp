// 数据集划分工具：把 full csv 切成 train + insert
//
// 用法:
//   ./sul_split <full_csv> [out_dir=datasets] [ratio=0.9] [seed=42] [strata=64]
//
// 抽样策略:
//   strata >= 2 → Z-order 桶分层抽样：按 Z-curve 排序后切成 strata 个等量段，
//                 每段独立按 ratio 随机抽样，保证不均匀数据集的局部密度也被
//                 等比例划分到 train / insert（无放回）
//   strata <= 1 → 退化为简单随机抽样（mt19937 shuffle + 取前 ratio*N）
//
// 输出命名（自动派生，与 record 命名风格一致）:
//   <out_dir>/<stem>_dim{d}_N{n_train}_train.csv
//   <out_dir>/<stem>_dim{d}_N{n_insert}_insert.csv
//
//   stem 取数据集文件名前两个 `_` 段（与 sul_query_gen 一致）
//   d 由首行逗号数自动检测（行格式：dim_1,...,dim_d,id → 共 d 个逗号）
//
// 注释行（# 开头）与空行被丢弃；CSV 格式与 sul::util::load_csv 完全兼容。

#include "sul/z_order.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

namespace {

bool is_blank_or_comment(const std::string& s) {
    for (char c : s) {
        if (c == '#') return true;
        if (c != ' ' && c != '\t' && c != '\r') return false;
    }
    return true;
}

std::vector<std::string> read_lines(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (is_blank_or_comment(line)) continue;
        lines.push_back(std::move(line));
    }
    return lines;
}

void write_lines(const std::string& path,
                 const std::vector<std::string>& lines,
                 const std::vector<size_t>& indices) {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("cannot write " + path);
    for (size_t i : indices) out << lines[i] << '\n';
}

int detect_dim(const std::string& first_line) {
    // 行格式: dim_1, dim_2, ..., dim_d, id → 共 d+1 个字段，d 个逗号
    int commas = 0;
    for (char c : first_line) if (c == ',') ++commas;
    return commas;
}

// 解析前 dim 个浮点字段（第 dim+1 个为 id，忽略）
void parse_coords(const std::string& line, int dim, std::vector<double>& out) {
    out.clear();
    out.reserve(static_cast<size_t>(dim));
    size_t pos = 0;
    for (int i = 0; i < dim; ++i) {
        size_t next = line.find(',', pos);
        if (next == std::string::npos)
            throw std::runtime_error("行字段数少于 dim+1：" + line);
        out.push_back(std::stod(line.substr(pos, next - pos)));
        pos = next + 1;
    }
}

// uniform_20000_1_2_.csv → uniform_20000
std::string dataset_stem_first_two(const std::string& path) {
    auto slash = path.find_last_of("/\\");
    std::string fname = (slash == std::string::npos) ? path : path.substr(slash + 1);
    auto dot = fname.find_last_of('.');
    std::string stem = (dot == std::string::npos) ? fname : fname.substr(0, dot);
    auto u1 = stem.find('_');
    if (u1 == std::string::npos) return stem;
    auto u2 = stem.find('_', u1 + 1);
    if (u2 == std::string::npos) return stem;
    return stem.substr(0, u2);
}

void ensure_dir(const std::string& dir) {
    if (dir.empty() || dir == ".") return;
    struct stat st{};
    if (::stat(dir.c_str(), &st) == 0) return;
    ::mkdir(dir.c_str(), 0755);
}

// 简单随机抽样：等价于无放回 SRS，前 n_train 个归 train、其余归 insert
void simple_random_partition(size_t N, double ratio, uint32_t seed,
                             std::vector<size_t>& train_idx,
                             std::vector<size_t>& insert_idx) {
    std::vector<size_t> idx(N);
    std::iota(idx.begin(), idx.end(), 0);
    std::mt19937 rng(seed);
    std::shuffle(idx.begin(), idx.end(), rng);

    const size_t n_train = static_cast<size_t>(static_cast<double>(N) * ratio);
    train_idx.assign(idx.begin(),               idx.begin() + n_train);
    insert_idx.assign(idx.begin() + n_train,    idx.end());
}

// Z-order 桶分层抽样：
//   1) 用 ZOrderEncoder 计算每点 key_len 字节大端序 Z-key
//   2) 按 Z-key 字典序排序索引
//   3) 切成 S 个连续等量段（残差摊到前 (N%S) 段，使各段大小差至多 1）
//   4) 每段独立 shuffle；段配额用最大余数法分配，保证 train 总数严格等于
//      round(N*ratio)，且每段保留至少 1 个点给 train 和 insert（除非段长=1）
void stratified_partition(const std::vector<std::string>& lines,
                          int dim, double ratio, uint32_t seed, size_t strata,
                          std::vector<size_t>& train_idx,
                          std::vector<size_t>& insert_idx) {
    const size_t N = lines.size();
    sul::ZOrderEncoder enc(dim);
    const int key_len = enc.key_len();

    std::vector<uint8_t> all_keys(N * static_cast<size_t>(key_len));
    std::vector<double> coords;
    std::vector<int32_t> qcoords(static_cast<size_t>(dim));
    for (size_t i = 0; i < N; ++i) {
        parse_coords(lines[i], dim, coords);
        for (int d = 0; d < dim; ++d)
            qcoords[d] = sul::scale_unit_double_to_int32(coords[d]);
        enc.encode_to_bytes(qcoords.data(),
                            all_keys.data() + i * static_cast<size_t>(key_len));
    }

    std::vector<size_t> order(N);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const uint8_t* ka = all_keys.data() + a * key_len;
        const uint8_t* kb = all_keys.data() + b * key_len;
        return std::lexicographical_compare(ka, ka + key_len, kb, kb + key_len);
    });

    const size_t S = std::min(strata, N);
    const size_t base   = N / S;
    const size_t remain = N % S;

    // 段配额按最大余数法分配：先 floor，剩余名额按小数部分降序补到差距最大的段
    std::vector<size_t> seg_len(S);
    std::vector<size_t> quota(S);
    std::vector<double> frac(S);
    const size_t target_train =
        static_cast<size_t>(std::llround(static_cast<double>(N) * ratio));
    size_t allocated = 0;
    for (size_t s = 0; s < S; ++s) {
        seg_len[s] = base + (s < remain ? 1 : 0);
        const double q = static_cast<double>(seg_len[s]) * ratio;
        quota[s] = static_cast<size_t>(q);
        frac[s]  = q - static_cast<double>(quota[s]);
        // 保留至少 1 train + 1 insert（段长 >= 2 时）
        if (seg_len[s] >= 2) {
            if (quota[s] == 0)               quota[s] = 1;
            else if (quota[s] == seg_len[s]) quota[s] = seg_len[s] - 1;
        }
        allocated += quota[s];
    }
    // 用余数排序补齐到 target_train，仅在不破坏 1/1 边界的段上增减
    std::vector<size_t> order_by_frac(S);
    std::iota(order_by_frac.begin(), order_by_frac.end(), 0);
    if (allocated < target_train) {
        std::sort(order_by_frac.begin(), order_by_frac.end(),
                  [&](size_t a, size_t b) { return frac[a] > frac[b]; });
        for (size_t s : order_by_frac) {
            if (allocated >= target_train) break;
            if (quota[s] < seg_len[s] - (seg_len[s] >= 2 ? 1 : 0)) {
                ++quota[s];
                ++allocated;
            }
        }
    } else if (allocated > target_train) {
        std::sort(order_by_frac.begin(), order_by_frac.end(),
                  [&](size_t a, size_t b) { return frac[a] < frac[b]; });
        for (size_t s : order_by_frac) {
            if (allocated <= target_train) break;
            if (quota[s] > (seg_len[s] >= 2 ? 1 : 0)) {
                --quota[s];
                --allocated;
            }
        }
    }

    std::mt19937 rng(seed);
    train_idx.clear();
    insert_idx.clear();
    train_idx.reserve(target_train + S);
    insert_idx.reserve(N - target_train + S);

    size_t cursor = 0;
    for (size_t s = 0; s < S; ++s) {
        const size_t len = seg_len[s];
        if (len == 0) continue;

        std::vector<size_t> seg(order.begin() + cursor,
                                order.begin() + cursor + len);
        cursor += len;
        std::shuffle(seg.begin(), seg.end(), rng);

        size_t n_train_seg = quota[s];
        if (len == 1 && quota[s] == 0) {
            // len=1 段未拿到名额，全部留给 insert
            n_train_seg = 0;
        }
        train_idx.insert(train_idx.end(),  seg.begin(), seg.begin() + n_train_seg);
        insert_idx.insert(insert_idx.end(), seg.begin() + n_train_seg, seg.end());
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0]
                  << " <full_csv> [out_dir=datasets] [ratio=0.9] [seed=42] [strata=64]\n";
        return 1;
    }
    const std::string full_path = argv[1];
    const std::string out_dir   = (argc > 2) ? argv[2] : "datasets";
    const double      ratio     = (argc > 3) ? std::atof(argv[3]) : 0.9;
    const uint32_t    seed      = (argc > 4)
        ? static_cast<uint32_t>(std::atoi(argv[4])) : 42u;
    const int         strata_cli = (argc > 5) ? std::atoi(argv[5]) : 64;

    if (ratio <= 0.0 || ratio >= 1.0) {
        std::cerr << "[error] train_ratio 必须在 (0, 1) 区间\n";
        return 2;
    }

    std::vector<std::string> lines;
    try {
        lines = read_lines(full_path);
    } catch (const std::exception& e) {
        std::cerr << "[error] " << e.what() << "\n";
        return 3;
    }
    const size_t N = lines.size();
    if (N < 2) {
        std::cerr << "[error] 数据行数 " << N << " 不足以切分\n";
        return 4;
    }

    const int dim = detect_dim(lines.front());
    if (dim < 1 || dim > 16) {
        std::cerr << "[error] 无法检测维度（首行逗号数异常: dim=" << dim << "）\n";
        return 5;
    }

    std::vector<size_t> train_idx;
    std::vector<size_t> insert_idx;
    std::string strategy;
    try {
        if (strata_cli >= 2) {
            stratified_partition(lines, dim, ratio, seed,
                                 static_cast<size_t>(strata_cli),
                                 train_idx, insert_idx);
            strategy = "stratified(Z-order, S=" + std::to_string(strata_cli) + ")";
        } else {
            simple_random_partition(N, ratio, seed, train_idx, insert_idx);
            strategy = "simple_random";
        }
    } catch (const std::exception& e) {
        std::cerr << "[error] " << e.what() << "\n";
        return 6;
    }

    if (train_idx.empty() || insert_idx.empty()) {
        std::cerr << "[error] 切分后 train 或 insert 为空（N=" << N
                  << " ratio=" << ratio << "）\n";
        return 7;
    }

    std::sort(train_idx.begin(),  train_idx.end());
    std::sort(insert_idx.begin(), insert_idx.end());

    ensure_dir(out_dir);
    const std::string stem = dataset_stem_first_two(full_path);
    const std::string train_path =
        out_dir + "/" + stem + "_dim" + std::to_string(dim) +
        "_N" + std::to_string(train_idx.size()) + "_train.csv";
    const std::string insert_path =
        out_dir + "/" + stem + "_dim" + std::to_string(dim) +
        "_N" + std::to_string(insert_idx.size()) + "_insert.csv";

    try {
        write_lines(train_path,  lines, train_idx);
        write_lines(insert_path, lines, insert_idx);
    } catch (const std::exception& e) {
        std::cerr << "[error] " << e.what() << "\n";
        return 8;
    }

    std::cout << "=== sul_split ===\n"
              << "  input   : " << full_path << " (N=" << N << " dim=" << dim << ")\n"
              << "  ratio   : " << ratio << "  seed=" << seed << "\n"
              << "  strategy: " << strategy << "\n"
              << "  train   : " << train_path  << " (" << train_idx.size()  << " 行)\n"
              << "  insert  : " << insert_path << " (" << insert_idx.size() << " 行)\n";
    return 0;
}
