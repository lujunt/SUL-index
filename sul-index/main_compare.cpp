// 明文 vs 密文索引正确性对比（CSV 驱动）
// 用同一份 CSV 数据建两个独立索引，跑相同查询，逐项比对结果
//
// 用法：./sul_compare_demo <csv_path> [paillier_key_size=1024]
//        [n_point_queries=40] [n_range_queries=5] [n_inserts=5]
//
// 对比维度：
//   1) 点查询：命中性一致 + 命中时 orig_id 一致
//   2) 范围查询：结果集 orig_id 集合相等（顺序无关）
//   3) 插入：InsertResult 一致 + 插入后再次点查能命中
//
// 退出码：0 = PASS，1 = FAIL

#include "sul/cipher/sul_cipher_index.h"
#include "sul/sul_index.h"
#include "sul/util/csv_loader.h"

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

void print_header(const char* title) {
    std::cout << "\n=== " << title << " ===\n";
}

void print_stats(const char* label, const CompareStats& s) {
    std::cout << "  " << label << ": " << s.matched << "/" << s.total
              << (s.mismatched ? "  [MISMATCH=" + std::to_string(s.mismatched) + "]" : "")
              << "\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0]
                  << " <csv_path> [paillier_key_size=1024]"
                  << " [n_point_queries=40] [n_range_queries=5] [n_inserts=5]\n";
        return 1;
    }
    const std::string csv_path = argv[1];
    const int32_t KSZ   = (argc > 2) ? std::atoi(argv[2]) : 1024;
    const int32_t N_PQ  = (argc > 3) ? std::atoi(argv[3]) : 40;  // 20 hit + 20 miss 各半
    const int32_t N_RQ  = (argc > 4) ? std::atoi(argv[4]) : 5;
    const int32_t N_INS = (argc > 5) ? std::atoi(argv[5]) : 5;

    std::cout << "=== SUL plain vs cipher 正确性对比 (CSV) ===\n";
    std::cout << "  csv = " << csv_path << "\n";
    std::cout << "  paillier_key = " << KSZ
              << "  N_PQ = " << N_PQ
              << "  N_RQ = " << N_RQ
              << "  N_INS = " << N_INS << "\n";

    // 加载 CSV
    util::CsvLoadResult loaded;
    try {
        loaded = util::load_csv(csv_path);
    } catch (const std::exception& e) {
        std::cerr << "[error] " << e.what() << "\n";
        return 2;
    }
    const int32_t N   = static_cast<int32_t>(loaded.data.size());
    const int32_t DIM = loaded.dim_count;
    std::cout << "  N = " << N << "  dim = " << DIM << "\n";

    // 两套独立索引各用一份拷贝
    auto data_plain  = loaded.data;
    auto data_cipher = loaded.data;
    IndexConfig cfg;
    cfg.dim_count   = DIM;
    cfg.error_bound = std::max<int32_t>(8, N / 1000);

    print_header("Phase 1: 构建两个索引（同一数据）");

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

    // ---- 点查询 ----
    print_header("Phase 2: 点查询对比");

    const auto& all_points = plain.all_points();
    std::mt19937 qgen(123);
    std::uniform_int_distribution<int32_t> coord_dist(0, 65535);
    std::uniform_int_distribution<size_t>  idx_dist(0, all_points.size() - 1);

    CompareStats pq_stats;
    auto run_point_query = [&](const int32_t* coords) {
        DataPoint*    p_hit = plain.point_query(coords);
        EncDataPoint* c_hit = cipher.point_query(coords);
        bool ok;
        if (!p_hit && !c_hit)     ok = true;
        else if (p_hit && c_hit)  ok = (p_hit->orig_id == c_hit->orig_id);
        else                      ok = false;
        pq_stats.incr(ok);
        if (!ok) {
            std::cout << "  [MISMATCH] plain="
                      << (p_hit ? std::to_string(p_hit->orig_id) : "null")
                      << "  cipher="
                      << (c_hit ? std::to_string(c_hit->orig_id) : "null") << "\n";
        }
    };

    const int32_t half_pq = N_PQ / 2;
    // 一半用 CSV 中实际存在的点
    for (int i = 0; i < half_pq; ++i) {
        const auto& probe = all_points[idx_dist(qgen)];
        int32_t coords[MAX_DIMS] = {};
        for (int32_t d = 0; d < DIM; ++d) coords[d] = probe.dimensions[d];
        run_point_query(coords);
    }
    // 另一半用随机坐标（大概率不命中）
    for (int i = 0; i < N_PQ - half_pq; ++i) {
        int32_t coords[MAX_DIMS] = {};
        for (int32_t d = 0; d < DIM; ++d) coords[d] = coord_dist(qgen);
        run_point_query(coords);
    }
    print_stats("point_query 一致", pq_stats);

    // ---- 范围查询 ----
    print_header("Phase 3: 范围查询对比");

    CompareStats rq_stats;
    for (int q = 0; q < N_RQ; ++q) {
        int32_t lo[MAX_DIMS] = {};
        int32_t hi[MAX_DIMS] = {};
        for (int32_t d = 0; d < DIM; ++d) {
            int32_t a = coord_dist(qgen);
            int32_t b = coord_dist(qgen);
            lo[d] = std::min(a, b);
            hi[d] = std::max(a, b);
        }

        auto p_res = plain.range_query(lo, hi);
        auto c_res = cipher.range_query(lo, hi);

        std::set<int32_t> p_ids, c_ids;
        for (auto* p : p_res) p_ids.insert(p->orig_id);
        for (auto* p : c_res) c_ids.insert(p->orig_id);

        bool ok = (p_ids == c_ids);
        rq_stats.incr(ok);

        std::cout << "  q" << q << ": plain=" << p_res.size()
                  << " cipher=" << c_res.size()
                  << (ok ? "  OK" : "  MISMATCH");
        if (!ok) {
            int only_p = 0, only_c = 0;
            for (int x : p_ids) if (!c_ids.count(x)) ++only_p;
            for (int x : c_ids) if (!p_ids.count(x)) ++only_c;
            std::cout << "  plain独有=" << only_p << " cipher独有=" << only_c;
        }
        std::cout << "\n";
    }
    print_stats("range_query 一致", rq_stats);

    // ---- 插入 ----
    print_header("Phase 4: 插入对比");

    CompareStats ins_stats;
    CompareStats post_pq_stats;
    for (int i = 0; i < N_INS; ++i) {
        int32_t new_pt[MAX_DIMS] = {};
        for (int32_t d = 0; d < DIM; ++d) new_pt[d] = coord_dist(qgen);

        InsertResult pr = plain.insert(new_pt);
        InsertResult cr = cipher.insert(new_pt);
        bool ok = (pr == cr);
        ins_stats.incr(ok);
        std::cout << "  ins" << i << ": plain="
                  << static_cast<int>(pr) << " cipher="
                  << static_cast<int>(cr)
                  << (ok ? "  OK" : "  MISMATCH") << "\n";

        DataPoint*    p_hit = plain.point_query(new_pt);
        EncDataPoint* c_hit = cipher.point_query(new_pt);
        bool post_ok = (p_hit != nullptr) && (c_hit != nullptr)
                    && p_hit->orig_id == c_hit->orig_id;
        post_pq_stats.incr(post_ok);
        if (!post_ok) {
            std::cout << "    [POST-INSERT MISMATCH] plain="
                      << (p_hit ? std::to_string(p_hit->orig_id) : "null")
                      << "  cipher="
                      << (c_hit ? std::to_string(c_hit->orig_id) : "null") << "\n";
        }
    }
    print_stats("insert 返回层级一致", ins_stats);
    print_stats("插入后点查命中一致", post_pq_stats);

    // ---- 总结 ----
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
