// 明文 vs 密文 范围查询对比（数据集 + 查询文件 双 CSV 驱动）
// 用法：./sul_compare_demo <dataset_csv> <query_csv>
//        [paillier_key=1024] [n_inserts=5]
//
// 流程：
//   1) 读 dataset 建明文/密文两套独立索引
//   2) 读 query 文件，对每条 range_query 在两边各跑一次，对比 orig_id 集合
//   3) 额外做点查询（随机抽 hit/miss 各半）+ 插入对比，保证综合一致性
//
// 退出码：0 = PASS（全部一致），1 = FAIL

#include "sul/cipher/sul_cipher_index.h"
#include "sul/sul_index.h"
#include "sul/util/csv_loader.h"
#include "sul/util/query_loader.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace sul;
using namespace sul::cipher;

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

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0]
                  << " <dataset_csv> <query_csv>"
                  << " [paillier_key=1024] [n_inserts=5]\n";
        return 1;
    }
    const std::string dataset_path = argv[1];
    const std::string query_path   = argv[2];
    const int32_t KSZ    = (argc > 3) ? std::atoi(argv[3]) : 1024;
    const int32_t N_INS  = (argc > 4) ? std::atoi(argv[4]) : 5;

    std::cout << "=== plain vs cipher 对比 (dataset+query CSV) ===\n";
    std::cout << "  dataset = " << dataset_path << "\n";
    std::cout << "  query   = " << query_path << "\n";
    std::cout << "  paillier_key = " << KSZ
              << "  n_inserts = " << N_INS << "\n";

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
    cfg.error_bound = std::max<int32_t>(8, N / 1000);

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

    t0 = std::chrono::steady_clock::now();
    CryptoContext crypto(KSZ);
    t1 = std::chrono::steady_clock::now();
    std::cout << "  Paillier keygen: "
              << std::chrono::duration<double, std::milli>(t1 - t0).count() << " ms\n";

    t0 = std::chrono::steady_clock::now();
    SULCipherIndex cipher(cfg, crypto);
    cipher.bulk_load(std::move(data_cipher));
    t1 = std::chrono::steady_clock::now();
    std::cout << "  cipher bulk_load: "
              << std::chrono::duration<double, std::milli>(t1 - t0).count() << " ms"
              << "  leaf=" << cipher.leaf_count()
              << "  learning=" << cipher.learning_layer_filled()
              << "  art=" << cipher.art_layer_points() << "\n";

    bool struct_ok = (plain.leaf_count() == cipher.leaf_count()
                   && plain.learning_layer_filled() == cipher.learning_layer_filled()
                   && plain.art_layer_points() == cipher.art_layer_points());
    std::cout << "  结构规模一致? " << (struct_ok ? "YES" : "NO") << "\n";

    // ------ Phase 2: 点查询（随机 hit/miss 各 20） ------
    print_header("Phase 2: 点查询对比（20 hit + 20 miss）");
    const auto& all_points = plain.all_points();
    std::mt19937 qgen(123);
    std::uniform_int_distribution<int32_t> coord_dist(0, 65535);
    std::uniform_int_distribution<size_t>  idx_dist(0, all_points.size() - 1);

    CompareStats pq_stats;
    auto run_pq = [&](const int32_t* coords) {
        DataPoint*    p_hit = plain.point_query(coords);
        EncDataPoint* c_hit = cipher.point_query(coords);
        bool ok;
        if (!p_hit && !c_hit)    ok = true;
        else if (p_hit && c_hit) ok = (p_hit->orig_id == c_hit->orig_id);
        else                     ok = false;
        pq_stats.incr(ok);
    };
    for (int i = 0; i < 20; ++i) {
        const auto& probe = all_points[idx_dist(qgen)];
        int32_t coords[MAX_DIMS] = {};
        for (int32_t d = 0; d < DIM; ++d) coords[d] = probe.dimensions[d];
        run_pq(coords);
    }
    for (int i = 0; i < 20; ++i) {
        int32_t coords[MAX_DIMS] = {};
        for (int32_t d = 0; d < DIM; ++d) coords[d] = coord_dist(qgen);
        run_pq(coords);
    }
    print_stats("point_query 一致", pq_stats);

    // ------ Phase 3: 范围查询（来自查询文件） ------
    print_header("Phase 3: 范围查询对比（来自查询文件）");
    CompareStats rq_stats;
    size_t plain_total_returned  = 0;
    size_t cipher_total_returned = 0;
    double plain_ms_total  = 0.0;
    double cipher_ms_total = 0.0;

    for (size_t qi = 0; qi < qf.queries.size(); ++qi) {
        const auto& q = qf.queries[qi];

        auto pt0 = std::chrono::steady_clock::now();
        auto p_res = plain.range_query(q.lo.data(), q.hi.data());
        auto pt1 = std::chrono::steady_clock::now();
        plain_ms_total += std::chrono::duration<double, std::milli>(pt1 - pt0).count();

        auto ct0 = std::chrono::steady_clock::now();
        auto c_res = cipher.range_query(q.lo.data(), q.hi.data());
        auto ct1 = std::chrono::steady_clock::now();
        cipher_ms_total += std::chrono::duration<double, std::milli>(ct1 - ct0).count();

        plain_total_returned  += p_res.size();
        cipher_total_returned += c_res.size();

        std::set<int32_t> p_ids, c_ids;
        for (auto* p : p_res) p_ids.insert(p->orig_id);
        for (auto* p : c_res) c_ids.insert(p->orig_id);
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
    std::cout << "  平均单查询: plain="
              << (NQ ? plain_ms_total / static_cast<double>(NQ) : 0.0) << " ms/q"
              << "  cipher="
              << (NQ ? cipher_ms_total / static_cast<double>(NQ) : 0.0) << " ms/q\n";

    // ------ Phase 4: 插入 ------
    print_header("Phase 4: 插入对比");
    CompareStats ins_stats, post_pq_stats;
    for (int i = 0; i < N_INS; ++i) {
        int32_t new_pt[MAX_DIMS] = {};
        for (int32_t d = 0; d < DIM; ++d) new_pt[d] = coord_dist(qgen);
        InsertResult pr = plain.insert(new_pt);
        InsertResult cr = cipher.insert(new_pt);
        bool ok = (pr == cr);
        ins_stats.incr(ok);

        DataPoint*    p_hit = plain.point_query(new_pt);
        EncDataPoint* c_hit = cipher.point_query(new_pt);
        bool post_ok = (p_hit && c_hit && p_hit->orig_id == c_hit->orig_id);
        post_pq_stats.incr(post_ok);
    }
    print_stats("insert 层级一致", ins_stats);
    print_stats("插入后点查命中一致", post_pq_stats);

    // ------ 总结 ------
    print_header("总结");
    int total   = pq_stats.total + rq_stats.total + ins_stats.total + post_pq_stats.total;
    int matched = pq_stats.matched + rq_stats.matched + ins_stats.matched + post_pq_stats.matched;
    std::cout << "  全部对比: " << matched << "/" << total << "\n";
    bool all_pass = (matched == total) && struct_ok;
    std::cout << "  结论: "
              << (all_pass ? "PASS 密文索引与明文索引结果一致"
                           : "FAIL 存在不一致项")
              << "\n";
    return all_pass ? 0 : 1;
}
