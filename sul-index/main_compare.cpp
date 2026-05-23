// 明文 vs 密文 范围查询对比（数据集 + 查询文件 双 CSV 驱动）
// 用法：./sul_compare_demo <dataset_csv> <query_csv>
//        [paillier_key=1024] [err=-1]
//
// 流程：
//   1) 构建：明文 bulk_load 现场构建；密文优先 load 已有 .scidx，
//      否则 build + save_to_file + 重新 load，始终在反序列化后的实例上查询
//      若走 build 路径，则写入 record/build_<stem>_K{K}_err{err}_dim{d}.csv
//   2) 范围查询：对每条 range_query 在明文/密文各跑一次，对比 orig_id 集合；
//      统一写入 record/rangequery_<stem>_K{K}_err{err}_dim{d}_sl{tag}.csv
//
// 退出码：0 = PASS（全部一致），1 = FAIL

#include "sul/cipher/sul_cipher_index.h"
#include "sul/sul_index.h"
#include "sul/util/csv_loader.h"
#include "sul/util/experiment_recorder.h"
#include "sul/util/query_loader.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace sul;
using namespace sul::cipher;
using sul::util::ExperimentRecorder;
using sul::util::ExpParams;

namespace {

struct CompareStats {
    int total = 0, matched = 0, mismatched = 0;
    void incr(bool ok) { ++total; if (ok) ++matched; else ++mismatched; }
};

void print_header(const char* title) { std::cout << "\n=== " << title << " ===\n"; }

void print_stats(const char* label, const CompareStats& s) {
    std::cout << "  " << label << ": " << s.matched << "/" << s.total
              << (s.mismatched ? "  [MISMATCH=" + std::to_string(s.mismatched) + "]" : "")
              << "\n";
}

// 从查询文件名解析选择率（uniform_20000_0.25.csv → 0.25）
double parse_sl_pct_from_path(const std::string& path) {
    auto slash = path.find_last_of('/');
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    auto dot = base.find_last_of('.');
    if (dot != std::string::npos) base.resize(dot);
    auto us = base.find_last_of('_');
    if (us == std::string::npos) return 0.0;
    try { return std::stod(base.substr(us + 1)); }
    catch (...) { return 0.0; }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0]
                  << " <dataset_csv> <query_csv>"
                  << " [paillier_key=1024] [err=-1]\n";
        return 1;
    }
    const std::string dataset_path = argv[1];
    const std::string query_path   = argv[2];
    const int32_t KSZ     = (argc > 3) ? std::atoi(argv[3]) : 1024;
    const int32_t err_cli = (argc > 4) ? std::atoi(argv[4]) : -1;

    std::cout << "=== plain vs cipher 对比 (dataset+query CSV) ===\n";
    std::cout << "  dataset = " << dataset_path << "\n";
    std::cout << "  query   = " << query_path << "\n";
    std::cout << "  paillier_key = " << KSZ
              << "  err(cli) = " << err_cli << " (<=0 → auto N/1000)\n";

    util::CsvLoadResult ds;
    util::QueryFile     qf;
    try {
        ds = util::load_csv(dataset_path);
        qf = util::load_query_file(query_path);
    } catch (const std::exception& e) {
        std::cerr << "[error] " << e.what() << "\n";
        return 2;
    }
    const int32_t N   = static_cast<int32_t>(ds.data.size());
    const int32_t DIM = ds.dim_count;
    if (DIM != qf.dim_count) {
        std::cerr << "[error] dataset dim=" << DIM
                  << " query dim=" << qf.dim_count << " 不匹配\n";
        return 3;
    }
    std::cout << "  dataset: N=" << N << "  dim=" << DIM << "\n";
    std::cout << "  query:   " << qf.queries.size() << " 条范围查询\n";

    auto data_plain  = ds.data;
    auto data_cipher = ds.data;
    IndexConfig cfg;
    cfg.dim_count   = DIM;
    cfg.error_bound = (err_cli > 0) ? err_cli : std::max<int32_t>(8, N / 1000);

    // ------ Phase 1: 构建 ------
    print_header("Phase 1: 构建索引");
    auto t0 = std::chrono::steady_clock::now();
    SULPlainIndex plain(cfg);
    plain.bulk_load(std::move(data_plain));
    auto t1 = std::chrono::steady_clock::now();
    std::cout << "  plain  bulk_load: "
              << std::chrono::duration<double, std::milli>(t1 - t0).count() << " ms"
              << "  leaf=" << plain.leaf_count()
              << "  learning=" << plain.learning_layer_filled()
              << "  art=" << plain.art_layer_points() << "\n";

    namespace fs = std::filesystem;
    const std::string scidx_path = "indexes/index_"
                                 + util::dataset_stem(dataset_path)
                                 + "_K"   + std::to_string(KSZ)
                                 + "_err" + std::to_string(cfg.error_bound)
                                 + "_dim" + std::to_string(DIM)
                                 + ".scidx";
    fs::create_directories("indexes");

    std::unique_ptr<CryptoContext>  crypto_holder;
    std::unique_ptr<SULCipherIndex> cipher_holder;
    bool   did_build      = false;
    double keygen_ms      = 0.0;
    double cipher_build_ms = 0.0;
    double save_ms        = 0.0;
    double load_ms        = 0.0;

    if (fs::exists(scidx_path)) {
        // 路径 A：直接反序列化已有索引
        std::cout << "  密文索引: 命中 " << scidx_path << " → load\n";
        t0 = std::chrono::steady_clock::now();
        auto pr = SULCipherIndex::load_from_file(scidx_path);
        crypto_holder = std::move(pr.first);
        cipher_holder = std::move(pr.second);
        t1 = std::chrono::steady_clock::now();
        load_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cout << "  load_from_file: " << load_ms << " ms\n";
    } else {
        // 路径 B：构建 → 序列化 → 释放 → 反序列化（保证查询在 loaded 实例上执行）
        std::cout << "  密文索引: 未找到 " << scidx_path
                  << " → build + save + reload\n";
        did_build = true;

        t0 = std::chrono::steady_clock::now();
        auto crypto_tmp = std::make_unique<CryptoContext>(KSZ);
        t1 = std::chrono::steady_clock::now();
        keygen_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cout << "  Paillier keygen: " << keygen_ms << " ms\n";

        t0 = std::chrono::steady_clock::now();
        auto cipher_tmp = std::make_unique<SULCipherIndex>(cfg, *crypto_tmp);
        cipher_tmp->bulk_load(std::move(data_cipher));
        t1 = std::chrono::steady_clock::now();
        cipher_build_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cout << "  cipher bulk_load: " << cipher_build_ms << " ms\n";

        t0 = std::chrono::steady_clock::now();
        cipher_tmp->save_to_file(scidx_path);
        t1 = std::chrono::steady_clock::now();
        save_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cout << "  save_to_file: " << scidx_path << " ("
                  << save_ms << " ms)\n";

        // 释放构建路径下的对象（顺序：cipher 先于 crypto）
        cipher_tmp.reset();
        crypto_tmp.reset();

        t0 = std::chrono::steady_clock::now();
        auto pr = SULCipherIndex::load_from_file(scidx_path);
        crypto_holder = std::move(pr.first);
        cipher_holder = std::move(pr.second);
        t1 = std::chrono::steady_clock::now();
        load_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cout << "  load_from_file: " << load_ms << " ms\n";
    }
    SULCipherIndex& cipher = *cipher_holder;

    size_t file_bytes = 0;
    {
        std::error_code ec;
        auto sz = fs::file_size(scidx_path, ec);
        if (!ec) file_bytes = sz;
    }

    std::cout << "  cipher: leaf=" << cipher.leaf_count()
              << "  learning=" << cipher.learning_layer_filled()
              << "  art=" << cipher.art_layer_points()
              << "  is_loaded=" << (cipher.is_loaded() ? "true" : "false") << "\n";

    // loaded 模式下 plain 骨架不保存 ART 叶子，art_layer_points()/learning_layer_filled()
    // 委托返回 0；只比较 leaf_count 即可
    bool struct_ok = cipher.is_loaded()
        ? (plain.leaf_count() == cipher.leaf_count())
        : (plain.leaf_count() == cipher.leaf_count()
        && plain.learning_layer_filled() == cipher.learning_layer_filled()
        && plain.art_layer_points() == cipher.art_layer_points());
    std::cout << "  结构规模一致? " << (struct_ok ? "YES" : "NO")
              << (cipher.is_loaded() ? " (loaded 模式仅比较 leaf_count)" : "") << "\n";

    const double sl_pct = parse_sl_pct_from_path(query_path);
    ExpParams exp_params{ KSZ, cfg.error_bound, DIM,
                          util::dataset_stem(dataset_path), "" };

    // 仅构建路径下记录 record/build（load 路径无构建数据）
    if (did_build) {
        // 等效存储开销 file_bytes_kl1：假设 EncDataPoint pool 中 ART-key
        // 只压成 1 个密文（其余 key_len-1 个不写盘），仅作为跨 dim 可比口径，
        // 真实磁盘文件大小仍为 file_bytes，不改任何序列化代码。
        // 单密文落盘 = 4B 长度前缀 + ceil(2K/8) 字节 raw 大数
        const int64_t key_len_actual    = cfg.key_len();
        const int64_t cipher_disk_bytes = 4 + ((2LL * KSZ + 7) / 8);
        const int64_t kl1_saved =
            (key_len_actual - 1) * static_cast<int64_t>(N) * cipher_disk_bytes;
        const size_t  file_bytes_kl1 =
            (static_cast<int64_t>(file_bytes) > kl1_saved)
            ? file_bytes - static_cast<size_t>(kl1_saved) : 0;

        fs::create_directories("record");
        std::string build_csv = ExperimentRecorder::build_path("build", exp_params);
        ExperimentRecorder::append_row(build_csv,
            {"timestamp","K","err","dim","N",
             "build_ms","keygen_ms","save_ms","load_ms",
             "file_bytes","file_bytes_kl1"},
            {ExperimentRecorder::now_iso(),
             std::to_string(KSZ),
             std::to_string(cfg.error_bound),
             std::to_string(DIM),
             std::to_string(N),
             ExperimentRecorder::ftoa(cipher_build_ms),
             ExperimentRecorder::ftoa(keygen_ms),
             ExperimentRecorder::ftoa(save_ms),
             ExperimentRecorder::ftoa(load_ms),
             std::to_string(file_bytes),
             std::to_string(file_bytes_kl1)});
        std::cout << "  → record: " << build_csv << "\n";
        std::cout << "  file_bytes      = " << file_bytes      << " B"
                  << "  (" << (file_bytes      / 1024.0 / 1024.0) << " MiB, 实际盘上)\n";
        std::cout << "  file_bytes_kl1  = " << file_bytes_kl1  << " B"
                  << "  (" << (file_bytes_kl1  / 1024.0 / 1024.0)
                  << " MiB, 等效: ART-key 仅 1 密文/点)\n";
    }

    // ------ Phase 2: 范围查询（来自查询文件） ------
    print_header("Phase 2: 范围查询对比（来自查询文件）");
    CompareStats rq_stats;
    size_t plain_total_returned  = 0;
    size_t cipher_total_returned = 0;
    size_t intersect_total       = 0;
    double plain_ms_total        = 0.0;
    double cipher_ms_total       = 0.0;
    double cipher_learn_us_sum   = 0.0;
    double cipher_art_us_sum     = 0.0;
    std::vector<size_t> plain_hits_per_query;
    plain_hits_per_query.reserve(qf.queries.size());

    for (size_t qi = 0; qi < qf.queries.size(); ++qi) {
        const auto& q = qf.queries[qi];

        auto pt0 = std::chrono::steady_clock::now();
        auto p_res = plain.range_query(q.lo.data(), q.hi.data());
        auto pt1 = std::chrono::steady_clock::now();
        plain_ms_total += std::chrono::duration<double, std::milli>(pt1 - pt0).count();

        SULCipherIndex::QueryStats cst;
        auto ct0 = std::chrono::steady_clock::now();
        auto c_res = cipher.range_query_with_stats(q.lo.data(), q.hi.data(), &cst);
        auto ct1 = std::chrono::steady_clock::now();
        cipher_ms_total     += std::chrono::duration<double, std::milli>(ct1 - ct0).count();
        cipher_learn_us_sum += cst.learning_us;
        cipher_art_us_sum   += cst.art_us;

        plain_total_returned  += p_res.size();
        cipher_total_returned += c_res.size();
        plain_hits_per_query.push_back(p_res.size());

        std::set<int32_t> p_ids, c_ids;
        for (auto* p : p_res) p_ids.insert(p->orig_id);
        for (auto* p : c_res) c_ids.insert(p->orig_id);
        size_t inter = 0;
        for (int x : p_ids) if (c_ids.count(x)) ++inter;
        intersect_total += inter;
        bool ok = (p_ids == c_ids);
        rq_stats.incr(ok);
        if (!ok) {
            int only_p = 0, only_c = 0;
            for (int x : p_ids) if (!c_ids.count(x)) ++only_p;
            for (int x : c_ids) if (!p_ids.count(x)) ++only_c;
            std::cout << "  q" << qi << " MISMATCH: plain=" << p_res.size()
                      << " cipher=" << c_res.size()
                      << " plain独有=" << only_p << " cipher独有=" << only_c << "\n";
        }
    }
    const size_t NQ = qf.queries.size();
    print_stats("range_query 一致", rq_stats);
    std::cout << "  平均返回点数: plain=" << (NQ ? plain_total_returned / NQ : 0)
              << "  cipher=" << (NQ ? cipher_total_returned / NQ : 0) << "\n";
    std::cout << "  总耗时: plain=" << plain_ms_total << " ms"
              << "  cipher=" << cipher_ms_total << " ms\n";
    const double plain_avg_ms  = NQ ? plain_ms_total  / static_cast<double>(NQ) : 0.0;
    const double cipher_avg_ms = NQ ? cipher_ms_total / static_cast<double>(NQ) : 0.0;
    const double cipher_learn_ms_avg =
        NQ ? (cipher_learn_us_sum / 1000.0) / static_cast<double>(NQ) : 0.0;
    const double cipher_art_ms_avg   =
        NQ ? (cipher_art_us_sum   / 1000.0) / static_cast<double>(NQ) : 0.0;
    std::cout << "  平均单查询: plain=" << plain_avg_ms << " ms/q"
              << "  cipher=" << cipher_avg_ms << " ms/q\n"
              << "  cipher 拆分: learning=" << cipher_learn_ms_avg << " ms/q"
              << "  art=" << cipher_art_ms_avg << " ms/q\n";

    const double recall    = plain_total_returned
        ? static_cast<double>(intersect_total) / static_cast<double>(plain_total_returned)
        : 1.0;
    const double precision = cipher_total_returned
        ? static_cast<double>(intersect_total) / static_cast<double>(cipher_total_returned)
        : 1.0;
    const double returned_avg =
        NQ ? static_cast<double>(cipher_total_returned) / static_cast<double>(NQ) : 0.0;

    // plain 端实测命中分布（真值），用于跨数据集口径对齐
    // skewed 数据上 sl_pct 仅是名义体积比，actual_ratio_pct 才是真选择率
    size_t hits_min = 0, hits_p50 = 0, hits_p95 = 0, hits_max = 0;
    double actual_ratio_pct = 0.0;
    if (!plain_hits_per_query.empty()) {
        std::vector<size_t> sorted_hits = plain_hits_per_query;
        std::sort(sorted_hits.begin(), sorted_hits.end());
        const size_t M = sorted_hits.size();
        hits_min = sorted_hits.front();
        hits_max = sorted_hits.back();
        hits_p50 = sorted_hits[M / 2];
        // p95 索引：ceil(0.95*M) - 1，截断到 [0, M-1]
        size_t idx95 = (M * 95 + 99) / 100;
        if (idx95 == 0) idx95 = 1;
        if (idx95 > M) idx95 = M;
        hits_p95 = sorted_hits[idx95 - 1];
        const double plain_returned_avg =
            static_cast<double>(plain_total_returned) / static_cast<double>(M);
        actual_ratio_pct = N > 0
            ? (plain_returned_avg / static_cast<double>(N)) * 100.0 : 0.0;
    }

    {
        fs::create_directories("record");
        ExpParams pr = exp_params;
        pr.extra = "_sl" + ExperimentRecorder::pct_tag(sl_pct);
        std::string rq_csv = ExperimentRecorder::build_path("rangequery", pr);
        ExperimentRecorder::append_row(rq_csv,
            {"timestamp","K","err","dim","N","sl_pct",
             "query_count","total_ms","avg_ms",
             "learning_ms_avg","art_ms_avg",
             "returned_avg","recall","precision",
             "actual_ratio_pct","returned_min","returned_p50",
             "returned_p95","returned_max"},
            {ExperimentRecorder::now_iso(),
             std::to_string(KSZ),
             std::to_string(cfg.error_bound),
             std::to_string(DIM),
             std::to_string(N),
             ExperimentRecorder::ftoa(sl_pct),
             std::to_string(NQ),
             ExperimentRecorder::ftoa(cipher_ms_total),
             ExperimentRecorder::ftoa(cipher_avg_ms),
             ExperimentRecorder::ftoa(cipher_learn_ms_avg),
             ExperimentRecorder::ftoa(cipher_art_ms_avg),
             ExperimentRecorder::ftoa(returned_avg),
             ExperimentRecorder::ftoa(recall),
             ExperimentRecorder::ftoa(precision),
             ExperimentRecorder::ftoa(actual_ratio_pct),
             std::to_string(hits_min),
             std::to_string(hits_p50),
             std::to_string(hits_p95),
             std::to_string(hits_max)});
        std::cout << "  → record: " << rq_csv << "\n";
        std::cout << "  实测选择率(plain): " << actual_ratio_pct << "%"
                  << "  (名义 sl_pct=" << sl_pct << "%)\n"
                  << "  plain 命中分布: min=" << hits_min
                  << " p50=" << hits_p50
                  << " p95=" << hits_p95
                  << " max=" << hits_max << "\n";
    }

    // ------ 总结 ------
    print_header("总结");
    std::cout << "  range_query 对比: " << rq_stats.matched << "/" << rq_stats.total << "\n";
    bool all_pass = (rq_stats.mismatched == 0) && struct_ok;
    std::cout << "  结论: "
              << (all_pass ? "PASS 密文索引与明文索引结果一致"
                           : "FAIL 存在不一致项")
              << "\n";
    return all_pass ? 0 : 1;
}
