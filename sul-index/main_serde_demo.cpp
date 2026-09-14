// SUL-cipher-index 序列化往返 demo
// 用法：./sul_serde_demo <dataset_csv> <query_csv> [paillier_key=1024] [out_dir=indexes]
//
// 流程：
//   1) 读 dataset 建密文索引（含插入计时统计）
//   2) idx.save_to_file(<out_dir>/<dataset_stem>.scidx)
//   3) SULCipherIndex::load_from_file 重载新实例
//   4) 在 loaded 实例上跑 query 文件全部范围查询
//   5) 与原实例结果比对 orig_id 集合，输出 PASS / FAIL

#include "sul/cipher/sul_cipher_index.h"
#include "sul/util/csv_loader.h"
#include "sul/util/query_loader.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace sul;
using namespace sul::cipher;

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0]
                  << " <dataset_csv> <query_csv> [paillier_key=1024] [out_dir=indexes]\n";
        return 1;
    }
    const std::string dataset_path = argv[1];
    const std::string query_path   = argv[2];
    const int32_t     KSZ          = (argc > 3) ? std::atoi(argv[3]) : 1024;
    const std::string out_dir      = (argc > 4) ? argv[4] : "indexes";

    std::cout << "=== SUL-cipher-index 序列化往返 demo ===\n";
    std::cout << "  dataset = " << dataset_path << "\n";
    std::cout << "  query   = " << query_path   << "\n";
    std::cout << "  paillier_key = " << KSZ
              << "  out_dir = " << out_dir << "\n";

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

    IndexConfig cfg;
    cfg.dim_count   = DIM;
    cfg.error_bound = 4;

    // ---- Phase 1: 构建 ----
    std::cout << "\n=== Phase 1: 构建原始密文索引 ===\n";
    CryptoContext crypto(KSZ);
    SULCipherIndex idx(cfg, crypto);
    auto t0 = std::chrono::steady_clock::now();
    idx.bulk_load(std::move(ds.data));
    auto t1 = std::chrono::steady_clock::now();
    std::cout << "  bulk_load = "
              << std::chrono::duration<double, std::milli>(t1 - t0).count() << " ms\n";
    std::cout << "  leaf=" << idx.leaf_count()
              << "  learning=" << idx.learning_layer_filled()
              << "  art=" << idx.art_layer_points() << "\n";

    // ---- Phase 2: 插入计时（插入后再 serde，让 inserted 池也纳入文件） ----
    std::cout << "\n=== Phase 2: 插入计时（含插入后序列化覆盖 inserted 池） ===\n";
    const int32_t N_INS = 20;
    std::mt19937 ins_rng(20260520);
    std::uniform_int_distribution<int32_t> ins_coord(0, 65535);
    double ins_total_us = 0.0, ins_min_us = 1e18, ins_max_us = 0.0;
    int32_t cnt_learn = 0, cnt_art = 0, cnt_fail = 0;
    for (int32_t i = 0; i < N_INS; ++i) {
        int32_t new_pt[MAX_DIMS] = {};
        for (int32_t d = 0; d < DIM; ++d) new_pt[d] = ins_coord(ins_rng);
        auto it0 = std::chrono::steady_clock::now();
        InsertResult r = idx.insert(new_pt);
        auto it1 = std::chrono::steady_clock::now();
        double us = std::chrono::duration<double, std::micro>(it1 - it0).count();
        ins_total_us += us;
        if (us < ins_min_us) ins_min_us = us;
        if (us > ins_max_us) ins_max_us = us;
        if      (r == InsertResult::LearningLayer) ++cnt_learn;
        else if (r == InsertResult::ARTLayer)      ++cnt_art;
        else                                       ++cnt_fail;
    }
    std::cout << "  共 " << N_INS << " 次插入 (Learning=" << cnt_learn
              << "  ART=" << cnt_art << "  Failed=" << cnt_fail << ")\n";
    std::cout << "  插入总耗时 = " << ins_total_us / 1000.0 << " ms\n";
    std::cout << "  平均单插入 = "
              << (N_INS ? ins_total_us / static_cast<double>(N_INS) : 0.0) << " us\n";
    std::cout << "  min/max   = " << ins_min_us << " us / " << ins_max_us << " us\n";

    // ---- Phase 3: save_to_file ----
    std::cout << "\n=== Phase 3: 序列化到 " << out_dir << "/ ===\n";
    fs::create_directories(out_dir);
    // 命名规则：含 dataset stem + K/err/dim 参数，避免跨数据集同参数缓存冲突
    //   index_<stem>_K{K}_err{err}_dim{d}.scidx
    // 注：N 已隐含在 <stem> 内（如 uniform_20000 / skewed_20000），无需再加 _N 后缀
    const std::string out_path = out_dir + "/index_"
                               + util::dataset_stem(dataset_path)
                               + "_K"   + std::to_string(KSZ)
                               + "_err" + std::to_string(cfg.error_bound)
                               + "_dim" + std::to_string(DIM)
                               + ".scidx";
    auto st0 = std::chrono::steady_clock::now();
    try {
        idx.save_to_file(out_path);
    } catch (const std::exception& e) {
        std::cerr << "[save error] " << e.what() << "\n";
        return 4;
    }
    auto st1 = std::chrono::steady_clock::now();
    std::cout << "  写出 = " << out_path << "\n";
    std::cout << "  耗时 = "
              << std::chrono::duration<double, std::milli>(st1 - st0).count() << " ms\n";
    std::cout << "  文件大小 = " << fs::file_size(out_path) << " bytes\n";

    // ---- Phase 4: load_from_file ----
    std::cout << "\n=== Phase 4: 反序列化 ===\n";
    std::unique_ptr<CryptoContext> loaded_crypto;
    std::unique_ptr<SULCipherIndex> loaded_idx;
    auto lt0 = std::chrono::steady_clock::now();
    try {
        auto pr = SULCipherIndex::load_from_file(out_path);
        loaded_crypto = std::move(pr.first);
        loaded_idx    = std::move(pr.second);
    } catch (const std::exception& e) {
        std::cerr << "[load error] " << e.what() << "\n";
        return 5;
    }
    auto lt1 = std::chrono::steady_clock::now();
    std::cout << "  耗时 = "
              << std::chrono::duration<double, std::milli>(lt1 - lt0).count() << " ms\n";
    std::cout << "  loaded leaf=" << loaded_idx->leaf_count()
              << "  inner_layers=" << loaded_idx->inner_layer_count()
              << "  is_loaded=" << (loaded_idx->is_loaded() ? "true" : "false") << "\n";

    bool struct_ok = (idx.leaf_count()        == loaded_idx->leaf_count()
                   && idx.inner_layer_count() == loaded_idx->inner_layer_count()
                   && idx.total_points()      == loaded_idx->total_points());
    std::cout << "  结构规模一致? " << (struct_ok ? "YES" : "NO") << "\n";

    // ---- Phase 5: loaded 实例跑 query 文件，与原实例对比 ----
    std::cout << "\n=== Phase 5: loaded 索引执行查询并比对 ===\n";
    int matched = 0, mismatched = 0;
    size_t total_orig = 0, total_loaded = 0;
    double orig_ms = 0.0, loaded_ms = 0.0;

    for (size_t qi = 0; qi < qf.queries.size(); ++qi) {
        const auto& q = qf.queries[qi];

        auto o0 = std::chrono::steady_clock::now();
        auto o_res = idx.range_query(q.lo.data(), q.hi.data());
        auto o1 = std::chrono::steady_clock::now();
        orig_ms += std::chrono::duration<double, std::milli>(o1 - o0).count();
        total_orig += o_res.size();

        auto l0 = std::chrono::steady_clock::now();
        auto l_res = loaded_idx->range_query(q.lo.data(), q.hi.data());
        auto l1 = std::chrono::steady_clock::now();
        loaded_ms += std::chrono::duration<double, std::milli>(l1 - l0).count();
        total_loaded += l_res.size();

        std::set<int32_t> ids_o, ids_l;
        for (auto* p : o_res) ids_o.insert(p->orig_id);
        for (auto* p : l_res) ids_l.insert(p->orig_id);
        if (ids_o == ids_l) ++matched;
        else {
            ++mismatched;
            int only_o = 0, only_l = 0;
            for (int x : ids_o) if (!ids_l.count(x)) ++only_o;
            for (int x : ids_l) if (!ids_o.count(x)) ++only_l;
            std::cout << "  q" << qi << " MISMATCH: orig=" << o_res.size()
                      << " loaded=" << l_res.size()
                      << " orig独有=" << only_o << " loaded独有=" << only_l << "\n";
        }
    }

    const size_t NQ = qf.queries.size();
    std::cout << "  范围查询一致: " << matched << "/" << NQ
              << (mismatched ? "  [MISMATCH=" + std::to_string(mismatched) + "]" : "")
              << "\n";
    std::cout << "  平均返回点数: orig=" << (NQ ? total_orig / NQ : 0)
              << "  loaded=" << (NQ ? total_loaded / NQ : 0) << "\n";
    std::cout << "  总耗时: orig=" << orig_ms << " ms  loaded=" << loaded_ms << " ms\n";
    std::cout << "  平均单查询: orig="
              << (NQ ? orig_ms / static_cast<double>(NQ) : 0.0) << " ms/q"
              << "  loaded="
              << (NQ ? loaded_ms / static_cast<double>(NQ) : 0.0) << " ms/q\n";

    std::cout << "\n=== 总结 ===\n";
    bool all_pass = struct_ok && (mismatched == 0);
    std::cout << "  结论: "
              << (all_pass ? "PASS 序列化后查询与原索引一致"
                           : "FAIL 序列化往返存在差异")
              << "\n";
    return all_pass ? 0 : 1;
}
