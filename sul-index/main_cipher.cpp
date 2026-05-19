// SUL-cipher-index demo（CSV 驱动）
// 用法：./sul_cipher_demo <csv_path> [paillier_key_size=1024]
//
// CSV 格式：每行 dim_1, dim_2, ..., dim_N, id
//   dim_i ∈ [0,1) 浮点，按 floor(v * 65536) 缩放为 int32 坐标
//   id   为整数

#include "sul/cipher/sul_cipher_index.h"
#include "sul/util/csv_loader.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace sul;
using namespace sul::cipher;

namespace {

void println(const char* label, double ms) {
    std::cout << "  " << label << ": " << ms << " ms\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0]
                  << " <csv_path> [paillier_key_size=1024]\n";
        return 1;
    }
    const std::string csv_path = argv[1];
    const int32_t KSZ = (argc > 2) ? std::atoi(argv[2]) : 1024;

    std::cout << "=== SUL-cipher-index demo (CSV) ===\n";
    std::cout << "  csv = " << csv_path << "\n";
    std::cout << "  paillier_key_size = " << KSZ << "\n";

    auto t0 = std::chrono::steady_clock::now();
    util::CsvLoadResult loaded;
    try {
        loaded = util::load_csv(csv_path);
    } catch (const std::exception& e) {
        std::cerr << "[error] " << e.what() << "\n";
        return 2;
    }
    auto t1 = std::chrono::steady_clock::now();
    println("CSV load",
            std::chrono::duration<double, std::milli>(t1 - t0).count());
    const int32_t N   = static_cast<int32_t>(loaded.data.size());
    const int32_t DIM = loaded.dim_count;
    std::cout << "  N = " << N << "  dim = " << DIM << "\n";

    t0 = std::chrono::steady_clock::now();
    CryptoContext crypto(KSZ);
    t1 = std::chrono::steady_clock::now();
    println("Paillier keygen",
            std::chrono::duration<double, std::milli>(t1 - t0).count());

    IndexConfig cfg;
    cfg.dim_count   = DIM;
    cfg.error_bound = std::max<int32_t>(8, N / 1000);
    std::cout << "  error_bound = " << cfg.error_bound
              << "  key_len = " << cfg.key_len()
              << " bytes  ART_layers = " << cfg.key_len() << "\n";

    // 在 move 之前抓取样本坐标用于后续查询
    int32_t sample_mid[MAX_DIMS] = {};
    int32_t sample_lo[MAX_DIMS]  = {};
    int32_t sample_hi[MAX_DIMS]  = {};
    {
        const auto& p_mid = loaded.data[N / 2];
        for (int32_t d = 0; d < DIM; ++d) sample_mid[d] = p_mid.dimensions[d];
        const auto& p0 = loaded.data[0];
        const auto& p1 = loaded.data[std::min(N - 1, N / 4)];
        for (int32_t d = 0; d < DIM; ++d) {
            sample_lo[d] = std::min(p0.dimensions[d], p1.dimensions[d]);
            sample_hi[d] = std::max(p0.dimensions[d], p1.dimensions[d]);
        }
    }

    SULCipherIndex idx(cfg, crypto);
    t0 = std::chrono::steady_clock::now();
    idx.bulk_load(std::move(loaded.data));
    t1 = std::chrono::steady_clock::now();
    println("bulk_load (含加密)",
            std::chrono::duration<double, std::milli>(t1 - t0).count());
    std::cout << "  leaf_count = " << idx.leaf_count()
              << "  inner_layers = " << idx.inner_layer_count()
              << "  learning_filled = " << idx.learning_layer_filled()
              << "  art_points = " << idx.art_layer_points() << "\n";

    {
        t0 = std::chrono::steady_clock::now();
        EncDataPoint* hit = idx.point_query(sample_mid);
        t1 = std::chrono::steady_clock::now();
        println("point_query (SQQP+SARTQ)",
                std::chrono::duration<double, std::milli>(t1 - t0).count());
        std::cout << "  hit? " << (hit ? "yes" : "no") << "\n";
    }

    {
        t0 = std::chrono::steady_clock::now();
        auto res = idx.range_query(sample_lo, sample_hi);
        t1 = std::chrono::steady_clock::now();
        println("range_query (SHRQ+SPI)",
                std::chrono::duration<double, std::milli>(t1 - t0).count());
        std::cout << "  result_count = " << res.size() << "\n";
    }

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
