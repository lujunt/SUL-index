#include "sul/cipher/sul_cipher_index.h"
#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace sul;
using namespace sul::cipher;

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

void check(SULCipherIndex& idx, const std::vector<DataPoint>& records) {
    const std::vector<std::array<int32_t,4>> windows = {
        {0,0,65535,65535}, {0,25000,60000,25100}, {50,4900,150,5100},
        {60000,60000,65000,65000}, {1000,10000,25000,40000}};
    size_t middle = 0, pruned = 0;
    for (const auto& q : windows) {
        SULCipherIndex::QueryStats full{}, counted{};
        auto hits = idx.range_query_with_stats(q.data(),q.data()+2,&full);
        const auto n = idx.count_range_candidates(q.data(),q.data()+2,&counted);
        require(n == full.candidates_total && n == counted.candidates_total, "candidate count mismatch");
        require(full.middle_leaves_total == counted.middle_leaves_total &&
                full.middle_leaves_pruned == counted.middle_leaves_pruned, "bbox pruning mismatch");
        require(counted.spi_filter_us == 0 && counted.candidates_kept == 0, "count executed SPI or reported hits");
        require(idx.count_range_candidates(q.data(),q.data()+2) == n, "count without stats mismatch");
        middle += full.middle_leaves_total; pruned += full.middle_leaves_pruned;
        std::vector<int32_t> expected, actual;
        for (const auto& p : records)
            if (p.dimensions[0] >= q[0] && p.dimensions[1] >= q[1] &&
                p.dimensions[0] <= q[2] && p.dimensions[1] <= q[3]) expected.push_back(p.orig_id);
        for (auto* p : hits) actual.push_back(p->orig_id);
        std::sort(expected.begin(),expected.end()); std::sort(actual.begin(),actual.end());
        require(expected == actual, "full query differs from coordinate scan");
    }
    if (!records.empty()) require(middle > 0 && pruned > 0, "fixture did not cover bbox pruning");
}

int main(int argc, char** argv) {
    try {
        require(argc == 2,"need snapshot path");
        CryptoContext crypto(1024);
        IndexConfig config; config.dim_count = 2; config.error_bound = 1;
        SULCipherIndex idx(config,crypto);
        std::vector<DataPoint> records;
        check(idx,records);
        auto add = [&](int32_t x, int32_t y) {
            DataPoint p{}; p.dim_count = 2; p.orig_id = static_cast<int32_t>(records.size());
            p.dimensions[0] = x; p.dimensions[1] = y; records.push_back(p);
        };
        for (int x = 1; x <= 20; ++x)
            for (int y = 1; y <= 10; ++y) add(x*x*100,y*5000);
        add(100,5000); add(100,5000);
        idx.bulk_load(records); check(idx,records);
        for (const auto& xy : std::vector<std::array<int32_t,2>>{{100,5000},{500,25050},{45000,55000}}) {
            require(idx.insert(xy.data()) != InsertResult::Failed,"insert failed");
            add(xy[0],xy[1]);
        }
        check(idx,records);
        std::filesystem::create_directories(std::filesystem::path(argv[1]).parent_path());
        idx.save_to_file(argv[1]);
        auto loaded = SULCipherIndex::load_from_file(argv[1]); check(*loaded.second,records);
        SULCipherIndex rebuilt(config,crypto); rebuilt.bulk_load(records); check(rebuilt,records);
        std::cout << "candidate counts, SPI bypass, bbox pruning, duplicate IDs, insert/rebuild/snapshot passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
