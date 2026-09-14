#include "sul/util/retrain_policy.h"
#include "sul/util/csv_loader.h"
#include "sul/sul_index.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class F> void rejects(F fn) {
    bool rejected = false;
    try { fn(); } catch (const std::exception&) { rejected = true; }
    require(rejected, "expected rejection");
}
int main(int argc, char** argv) {
    try {
        using namespace sul;
        using namespace sul::util;
        require(monitor_query_count(.25L)==400 && monitor_query_count(.5L)==200 &&
                monitor_query_count(1)==100 && monitor_query_count(2)==50 && monitor_query_count(4)==25,
                "selectivity counts");
        rejects([]{monitor_query_count(0);}); rejects([]{monitor_query_count(101);});
        require(!retrain_decision(120000,120000,100000,1).triggered,"large baseline must not trigger");
        require(retrain_decision(220000,120000,100000,1).triggered,"threshold equality");
        require(!retrain_decision(219999,120000,100000,1).triggered,"below threshold");
        require(!retrain_decision(220000,120000,100001,1).triggered,"use current N");
        require(retrain_decision(2,10,100,1).delta==-8,"signed difference");
        require(!retrain_decision(50,50,10,1).triggered,"rebaseline");
        if (argc != 2) throw std::runtime_error("test temp directory required");
        std::filesystem::path dir=argv[1]; std::filesystem::create_directories(dir);
        std::ofstream(dir/"base.csv") << "10,20,100\n20,40,101\n";
        std::ofstream(dir/"insert.csv") << "12,24,500\n13,26,501\n";
        std::ofstream(dir/"outside.csv") << "9,25,502\n";
        auto base=load_csv((dir/"base.csv").string());
        auto inserted=load_csv((dir/"insert.csv").string(),base.normalization);
        require(inserted.data[0].dimensions[0]==13107 && inserted.data[1].dimensions[0]==19660,
                "hotspot must retain base normalization");
        rejects([&]{load_csv((dir/"outside.csv").string(),base.normalization);});
        IndexConfig cfg;cfg.error_bound=4;cfg.dim_count=2;
        SULPlainIndex idx(cfg);std::vector<DataPoint> points;
        for (int i=0;i<100;++i) {
            DataPoint p{};p.dim_count=2;p.orig_id=i;
            p.dimensions[0]=(i%10)*500;p.dimensions[1]=(i%5)*700;
            points.push_back(p);
        }
        idx.bulk_load(points);
        int32_t lo[2]={0,0},hi[2]={65535,65535};
        auto hits=idx.range_query(lo,hi);
        std::set<int> ids;for(auto* p:hits) ids.insert(p->orig_id);
        require(hits.size()==100 && ids.size()==100,"duplicate records lost in plain ART");
        require(idx.learning_layer_filled()+idx.art_layer_points()==100,"layer record count mismatch");
        std::cout << "retrain unit checks passed\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
