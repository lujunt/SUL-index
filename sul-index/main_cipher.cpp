// SUL-cipher-index demo（数据集 + 查询文件 双 CSV 驱动）
// 用法：./sul_cipher_demo <dataset_csv> <query_csv> [paillier_key=1024]
//
// 流程：
//   1) 读 dataset 建密文索引
//   2) 读 query 文件，批量跑范围查询，输出延迟统计
//   3) 额外做 1 次点查询 + 1 次插入演示

#include "sul/cipher/sul_cipher_index.h"
#include "sul/util/csv_loader.h"
#include "sul/util/query_loader.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace sul;
using namespace sul::cipher;

namespace {
void println(const char* label, double ms) {
    std::cout << "  " << label << ": " << ms << " ms\n";
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0]
                  << " <dataset_csv> <query_csv> [paillier_key=1024]\n";
        return 1;
    }
    const std::string dataset_path = argv[1];
    const std::string query_path   = argv[2];
    const int32_t KSZ = (argc > 3) ? std::atoi(argv[3]) : 1024;

    std::cout << "=== SUL-cipher-index demo (dataset+query CSV) ===\n";
    std::cout << "  dataset = " << dataset_path << "\n";
    std::cout << "  query   = " << query_path << "\n";
    std::cout << "  paillier_key = " << KSZ << "\n";

    auto t0 = std::chrono::steady_clock::now();
    util::CsvLoadResult ds;
    util::QueryFile     qf;
    try {
        ds = util::load_csv(dataset_path);
        qf = util::load_query_file(query_path);
    } catch (const std::exception& e) {
        std::cerr << "[error] " << e.what() << "\n";
        return 2;
    }
    auto t1 = std::chrono::steady_clock::now();
    println("CSV load (dataset + query)",
            std::chrono::duration<double, std::milli>(t1 - t0).count());

    const int32_t N   = static_cast<int32_t>(ds.data.size());
    const int32_t DIM = ds.dim_count;
    if (DIM != qf.dim_count) {
        std::cerr << "[error] dataset dim=" << DIM
                  << " query dim=" << qf.dim_count << " 不匹配\n";
        return 3;
    }
    std::cout << "  dataset: N=" << N << "  dim=" << DIM << "\n";
    std::cout << "  query:   " << qf.queries.size() << " 条范围查询\n";

    t0 = std::chrono::steady_clock::now();
    CryptoContext crypto(KSZ);
    t1 = std::chrono::steady_clock::now();
    println("Paillier keygen",
            std::chrono::duration<double, std::milli>(t1 - t0).count());

    IndexConfig cfg;
    cfg.dim_count   = DIM;
    cfg.error_bound = std::max<int32_t>(8, N / 1000);
    std::cout << "  error_bound = " << cfg.error_bound
              << "  key_len = " << cfg.key_len() << " bytes\n";

    // 抓样本点供点查询/插入演示
    int32_t sample_mid[MAX_DIMS] = {};
    for (int32_t d = 0; d < DIM; ++d) sample_mid[d] = ds.data[N / 2].dimensions[d];

    SULCipherIndex idx(cfg, crypto);
    t0 = std::chrono::steady_clock::now();
    idx.bulk_load(std::move(ds.data));
    t1 = std::chrono::steady_clock::now();
    println("bulk_load (含加密)",
            std::chrono::duration<double, std::milli>(t1 - t0).count());
    std::cout << "  leaf_count = " << idx.leaf_count()
              << "  inner_layers = " << idx.inner_layer_count()
              << "  learning_filled = " << idx.learning_layer_filled()
              << "  art_points = " << idx.art_layer_points() << "\n";

    // ---- 点查询（1 次演示） ----
    {
        t0 = std::chrono::steady_clock::now();
        EncDataPoint* hit = idx.point_query(sample_mid);
        t1 = std::chrono::steady_clock::now();
        println("point_query (SQQP+SARTQ)",
                std::chrono::duration<double, std::milli>(t1 - t0).count());
        std::cout << "  hit? " << (hit ? "yes" : "no") << "\n";
    }

    // ---- 批量范围查询 ----
    std::cout << "\n--- 批量范围查询 ---\n";
    double total_ms = 0.0;
    size_t total_returned = 0;
    size_t min_returned = SIZE_MAX, max_returned = 0;
    for (const auto& q : qf.queries) {
        auto rt0 = std::chrono::steady_clock::now();
        auto res = idx.range_query(q.lo.data(), q.hi.data());
        auto rt1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(rt1 - rt0).count();
        total_ms += ms;
        total_returned += res.size();
        if (res.size() < min_returned) min_returned = res.size();
        if (res.size() > max_returned) max_returned = res.size();
    }
    const size_t NQ = qf.queries.size();
    std::cout << "  共 " << NQ << " 条范围查询\n";
    std::cout << "  返回点数: 平均=" << (NQ ? total_returned / NQ : 0)
              << "  min=" << (NQ ? min_returned : 0)
              << "  max=" << max_returned << "\n";
    std::cout << "  总耗时 = " << total_ms << " ms\n";
    std::cout << "  平均单查询 = "
              << (NQ ? total_ms / static_cast<double>(NQ) : 0.0) << " ms/q\n";

    // ---- 插入演示 ----
    {
        int32_t new_pt[MAX_DIMS] = {};
        for (int32_t d = 0; d < DIM; ++d) new_pt[d] = sample_mid[d] + 7;
        t0 = std::chrono::steady_clock::now();
        InsertResult r = idx.insert(new_pt);
        t1 = std::chrono::steady_clock::now();
        println("insert (SQQP+定位)",
                std::chrono::duration<double, std::milli>(t1 - t0).count());
        const char* layer = (r == InsertResult::LearningLayer) ? "Learning"
                          : (r == InsertResult::ARTLayer)      ? "ART"
                          : "Failed";
        std::cout << "  layer = " << layer << "\n";
    }

    std::cout << "done.\n";
    return 0;
}
