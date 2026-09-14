// 选择率自适应重训练：检测集 m=ceil(1/s)，评估集固定 100 条。
#include "sul/cipher/sul_cipher_index.h"
#include "sul/util/csv_loader.h"
#include "sul/util/query_loader.h"
#include "sul/util/retrain_policy.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace sul;
using namespace sul::cipher;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {
double ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
template<class T> std::string str(T value) {
    std::ostringstream out;
    out << std::setprecision(std::numeric_limits<long double>::max_digits10) << value;
    return out.str();
}
std::string quote(const std::string& value) {
    std::string out = "\"";
    for (char c : value) { out += c; if (c == '"') out += c; }
    return out + '"';
}
struct Table {
    std::ofstream out;
    size_t width;
    Table(const fs::path& path, const std::vector<std::string>& header)
        : out(path), width(header.size()) {
        if (!out) throw std::runtime_error("cannot create " + path.string());
        row(header);
    }
    void row(const std::vector<std::string>& cells) {
        if (cells.size() != width) throw std::logic_error("CSV column mismatch");
        for (size_t i = 0; i < cells.size(); ++i) {
            if (i) out << ',';
            out << quote(cells[i]);
        }
        out << '\n'; out.flush();
        if (!out) throw std::runtime_error("CSV write failed");
    }
};
long double number(const std::string& s) {
    size_t used = 0;
    auto n = std::stold(s, &used);
    if (used != s.size() || !std::isfinite(n)) throw std::invalid_argument("invalid number: " + s);
    return n;
}
size_t integer(const std::string& s, bool zero = false) {
    auto n = number(s);
    if (n < (zero ? 0 : 1) || std::floor(n) != n || n > INT32_MAX)
        throw std::invalid_argument("invalid integer: " + s);
    return static_cast<size_t>(n);
}
std::vector<long double> percentages(const std::string& value) {
    std::vector<long double> result;
    std::stringstream input(value);
    std::string part;
    while (std::getline(input, part, ',')) {
        if (part.empty()) throw std::invalid_argument("empty ul checkpoint");
        result.push_back(number(part));
    }
    if (result.empty()) throw std::invalid_argument("empty ul checkpoints");
    return result;
}
std::string join(const std::vector<long double>& values) {
    std::string result;
    for (auto value : values) {
        if (!result.empty()) result += ',';
        result += str(value);
    }
    return result;
}
std::string fingerprint(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot fingerprint " + path.string());
    uint64_t hash = 14695981039346656037ULL;
    char c;
    while (in.get(c)) { hash ^= static_cast<unsigned char>(c); hash *= 1099511628211ULL; }
    if (!in.eof()) throw std::runtime_error("fingerprint read failed");
    std::ostringstream out; out << std::hex << hash;
    return out.str();
}
void usage() {
    std::cout << "Usage: sul_update BASE INSERT EVAL_100 [K=1024] [err=4] [ul_pct=20]\n"
        "  --monitor-query FILE --sl-pct PCT [--beta 1] [--check-every N]\n"
        "  [--ul-checkpoints 20,25,...,70]\n"
        "  [--coarse-check-every N --fine-threshold-ratio 0.8]\n"
        "  [--run-dir DIR] [--distribution matched|hotspot|unspecified]\n"
        "  [--repeat-id N] [--warmup-rounds 0] [--eval-repeats 1]\n"
        "  [--monitor-warmup-rounds 0] [--monitor-repeats 1]\n"
        "  [--max-rebuilds 0] (0=run to ul; pilot runs may use 1)\n"
        "  [--pilot-timing-only 0|1] (skip evaluation queries and snapshots)\n"
        "  [--phase both|control|adaptive]\n"
        "  [--dataset-label NAME]\n"
        "ul_pct is the maximum insertion percentage; ul checkpoints are recorded within the same continuous run.\n"
        "Checks are candidate-only; SPI is timed at baselines and triggers. Coarse checking is opt-in.\n"
        "Each phase uses the same monitor set and 100 evaluation queries and always runs to ul_pct.\n"
        "Legacy positional theta is rejected. Existing run directories are never overwritten.\n";
}
struct Options {
    std::string base, insert, eval, monitor, label, distribution = "unspecified", phase = "both";
    fs::path dir;
    int K = 1024, err = 4;
    long double ul = 20, sl = 0, beta = 1;
    size_t check_every = 0, repeat_id = 1, warmup = 0, repeats = 1;
    size_t monitor_warmup = 0, monitor_repeats = 1;
    size_t max_rebuilds = 0;
    bool pilot_timing_only = false;
    size_t coarse_check_every = 0;
    long double fine_threshold_ratio = 0.8;
    std::vector<long double> ul_checkpoints;
};
Options options(int argc, char** argv) {
    if (argc < 4) throw std::invalid_argument("need BASE INSERT EVAL_100; use --help");
    Options o; o.base = argv[1]; o.insert = argv[2]; o.eval = argv[3];
    int i = 4, positional = 0;
    while (i < argc && std::string(argv[i]).rfind("--", 0) != 0) {
        if (positional == 0) o.K = static_cast<int>(integer(argv[i]));
        else if (positional == 1) {
            const auto e = number(argv[i]);
            if (std::floor(e) != e || e < INT32_MIN || e > INT32_MAX)
                throw std::invalid_argument("invalid err");
            o.err = e <= 0 ? 4 : static_cast<int>(e);
        }
        else if (positional == 2) o.ul = number(argv[i]);
        else throw std::invalid_argument("positional theta removed; use --beta");
        ++i; ++positional;
    }
    for (; i < argc; ++i) {
        std::string key = argv[i];
        if (++i >= argc) throw std::invalid_argument("missing value for " + key);
        std::string value = argv[i];
        if (key == "--monitor-query") o.monitor = value;
        else if (key == "--sl-pct") o.sl = number(value);
        else if (key == "--beta") o.beta = number(value);
        else if (key == "--check-every") o.check_every = integer(value);
        else if (key == "--coarse-check-every") o.coarse_check_every = integer(value, true);
        else if (key == "--fine-threshold-ratio") o.fine_threshold_ratio = number(value);
        else if (key == "--ul-checkpoints") o.ul_checkpoints = percentages(value);
        else if (key == "--run-dir") o.dir = value;
        else if (key == "--distribution") o.distribution = value;
        else if (key == "--dataset-label") o.label = value;
        else if (key == "--repeat-id") o.repeat_id = integer(value);
        else if (key == "--warmup-rounds") o.warmup = integer(value, true);
        else if (key == "--eval-repeats") o.repeats = integer(value);
        else if (key == "--monitor-warmup-rounds") o.monitor_warmup = integer(value, true);
        else if (key == "--monitor-repeats") o.monitor_repeats = integer(value);
        else if (key == "--max-rebuilds") o.max_rebuilds = integer(value, true);
        else if (key == "--pilot-timing-only") {
            const auto enabled = integer(value, true);
            if (enabled > 1) throw std::invalid_argument("pilot-timing-only must be 0 or 1");
            o.pilot_timing_only = enabled == 1;
        }
        else if (key == "--phase") o.phase = value;
        else throw std::invalid_argument("unknown option: " + key);
    }
    if (o.monitor.empty() || o.sl <= 0 || o.beta <= 0 || o.ul <= 0)
        throw std::invalid_argument("require --monitor-query, positive --sl-pct, beta and ul_pct");
    if (o.K != 1024 && o.K != 2048 && o.K != 3072 && o.K != 4096)
        throw std::invalid_argument("K must be 1024/2048/3072/4096");
    if (o.warmup > 10 || o.repeats > 100 || o.monitor_warmup > 10 || o.monitor_repeats > 100)
        throw std::invalid_argument("too many query rounds");
    if (o.fine_threshold_ratio <= 0 || o.fine_threshold_ratio > 1)
        throw std::invalid_argument("fine-threshold-ratio must be in (0,1]");
    if (o.pilot_timing_only && o.max_rebuilds != 1)
        throw std::invalid_argument("pilot-timing-only requires --max-rebuilds 1");
    if (o.distribution != "matched" && o.distribution != "hotspot" && o.distribution != "unspecified")
        throw std::invalid_argument("invalid distribution label");
    if (o.phase != "both" && o.phase != "control" && o.phase != "adaptive")
        throw std::invalid_argument("phase must be both, control or adaptive");
    if (o.label.empty()) o.label = fs::path(o.base).stem().string();
    if (o.ul_checkpoints.empty()) o.ul_checkpoints.push_back(o.ul);
    long double previous = 0;
    for (auto checkpoint : o.ul_checkpoints) {
        if (checkpoint <= previous || checkpoint > o.ul)
            throw std::invalid_argument("ul checkpoints must be positive, strictly increasing and <= ul_pct");
        previous = checkpoint;
    }
    if (o.dir.empty()) {
        auto tick = std::chrono::system_clock::now().time_since_epoch().count();
        o.dir = fs::path("record/retrain_v2") / str(tick);
    }
    return o;
}
struct Measurement {
    bool full_query = true;
    int64_t W = 0;
    size_t hits = 0;
    double query_ms = 0, wall_ms = 0;
    double learning_ms = 0, collect_ms = 0, spi_setup_ms = 0, spi_filter_ms = 0;
    std::vector<double> round_query_ms; // 每轮 m 条查询调用的墙钟时间之和，排除预热与校验。
    std::vector<double> round_learning_ms, round_collect_ms, round_spi_setup_ms, round_spi_filter_ms;
    double avg(size_t count) const { return query_ms / count; }
    double selectivity(size_t count, size_t n) const { return 100.0 * hits / count / n; }
};
Measurement measure(SULCipherIndex& idx, const util::QueryFile& queries,
                    const std::vector<DataPoint>& records, size_t warmup = 0, size_t repeats = 1) {
    auto start = Clock::now();
    for (size_t r = 0; r < warmup; ++r)
        for (const auto& q : queries.queries) idx.range_query(q.lo.data(), q.hi.data());
    Measurement m;
    for (size_t round = 0; round < repeats; ++round) {
        int64_t round_W = 0;
        double round_ms = 0, learning_ms = 0, collect_ms = 0, setup_ms = 0, filter_ms = 0;
        for (size_t qi = 0; qi < queries.queries.size(); ++qi) {
            const auto& q = queries.queries[qi];
            SULCipherIndex::QueryStats stats{};
            auto begin = Clock::now();
            auto result = idx.range_query_with_stats(q.lo.data(), q.hi.data(), &stats);
            round_ms += ms(begin);
            learning_ms += stats.learning_us / 1000;
            collect_ms += stats.collect_us / 1000;
            setup_ms += stats.spi_setup_us / 1000;
            filter_ms += stats.spi_filter_us / 1000;
            if (stats.candidates_total > static_cast<uint64_t>(INT64_MAX - round_W))
                throw std::overflow_error("candidate sum overflow");
            round_W += static_cast<int64_t>(stats.candidates_total);
            // 独立坐标扫描：保留不同 ID 的同坐标记录，不以 plain 查询作真值。
            std::vector<int32_t> expected, actual;
            for (const auto& dp : records) {
                bool inside = true;
                for (int d = 0; d < dp.dim_count; ++d)
                    if (dp.dimensions[d] < q.lo[d] || dp.dimensions[d] > q.hi[d]) { inside = false; break; }
                if (inside) expected.push_back(dp.orig_id);
            }
            for (auto* p : result) {
                if (!p) throw std::runtime_error("null query result");
                actual.push_back(p->orig_id);
            }
            std::sort(expected.begin(), expected.end()); std::sort(actual.begin(), actual.end());
            if (expected != actual)
                throw std::runtime_error("range query differs from brute force at query " + str(qi)
                                         + " expected=" + str(expected.size()) + " actual=" + str(actual.size()));
            if (round == 0) m.hits += expected.size();
        }
        if (round == 0) m.W = round_W;
        else if (m.W != round_W) throw std::runtime_error("candidate count changed without updates");
        m.round_query_ms.push_back(round_ms);
        m.round_learning_ms.push_back(learning_ms);
        m.round_collect_ms.push_back(collect_ms);
        m.round_spi_setup_ms.push_back(setup_ms);
        m.round_spi_filter_ms.push_back(filter_ms);
        m.query_ms += round_ms;
        m.learning_ms += learning_ms;
        m.collect_ms += collect_ms;
        m.spi_setup_ms += setup_ms;
        m.spi_filter_ms += filter_ms;
    }
    m.query_ms /= repeats; m.learning_ms /= repeats; m.collect_ms /= repeats;
    m.spi_setup_ms /= repeats; m.spi_filter_ms /= repeats;
    m.wall_ms = ms(start);
    return m;
}
Measurement count_candidates(SULCipherIndex& idx, const util::QueryFile& queries) {
    auto start = Clock::now();
    Measurement m;
    m.full_query = false;
    for (const auto& q : queries.queries) {
        const auto n = idx.count_range_candidates(q.lo.data(), q.hi.data());
        if (n > static_cast<uint64_t>(INT64_MAX - m.W))
            throw std::overflow_error("candidate sum overflow");
        m.W += static_cast<int64_t>(n);
    }
    m.wall_ms = ms(start);
    return m;
}
struct Snapshot { uint64_t bytes, kl1; double wall_ms; };
Snapshot snapshot(SULCipherIndex& idx, const fs::path& path, const Options& o,
                  const util::QueryFile& eval, const std::vector<DataPoint>& records,
                  const Measurement& expected) {
    auto start = Clock::now();
    idx.save_to_file(path.string());
    auto loaded = SULCipherIndex::load_from_file(path.string());
    if (loaded.second->total_points() != records.size()) throw std::runtime_error("snapshot point count mismatch");
    const auto check = measure(*loaded.second, eval, records);
    if (check.W != expected.W) throw std::runtime_error("snapshot candidate mismatch");
    uint64_t bytes = fs::file_size(path);
    uint64_t reduction = static_cast<uint64_t>(idx.config().key_len() - 1) * records.size()
                         * (4 + (2ULL * o.K + 7) / 8);
    return {bytes, bytes > reduction ? bytes - reduction : 0, ms(start)};
}
const std::vector<std::string> event_header = {
    "event_id","configured_ul_pct","successful_updates","trigger_insert_pct","Nt",
    "W0_before","Wt_before","delta_W_before",
    "beta","rho_before","W_after","rho_after_old_baseline","pre_eval_query_avg_ms","post_eval_query_avg_ms",
    "eval_latency_speedup","pre_eval_recall","post_eval_recall","pre_eval_precision","post_eval_precision",
    "pre_eval_actual_sl_pct","post_eval_actual_sl_pct","rebuild_build_ms","rebuild_event_wall_ms",
    "rebaseline_ms","snapshot_path","snapshot_bytes","snapshot_bytes_kl1",
    "epoch_before","baseline_successful_updates","m","monitor_repeats",
    "monitor_baseline_total_ms","monitor_pre_total_ms","monitor_pre_query_avg_ms","monitor_delta_total_ms",
    "monitor_post_total_ms","monitor_post_query_avg_ms","monitor_saved_total_ms","beta_rebuild_ms",
    "monitor_delta_over_rebuild","time_condition_met",
    "spi_baseline_total_ms","spi_pre_total_ms","spi_delta_total_ms","spi_delta_avg_ms",
    "spi_post_total_ms","spi_saved_total_ms",
    "spi_delta_over_rebuild","spi_time_condition_met"};
size_t phase(const Options& o, const util::CsvLoadResult& base, const util::CsvLoadResult& inserts,
             const util::QueryFile& monitor, const util::QueryFile& eval, size_t target,
             const std::map<size_t,long double>& ul_checkpoints,
             bool adaptive, Table& summary, Table& events) {
    const std::string mode = adaptive ? "adaptive" : "control";
    std::cout << "[phase] " << mode << std::endl;
    auto phase_start = Clock::now();
    CryptoContext crypto(o.K);
    IndexConfig config; config.dim_count = base.dim_count; config.error_bound = o.err;
    auto idx = std::make_unique<SULCipherIndex>(config, crypto);
    std::vector<DataPoint> records = base.data;
    idx->bulk_load(records);
    Table trace(o.dir / (mode + "_trace.csv"), {"mode","configured_ul_pct","stage","epoch","event_id","attempted_updates",
        "successful_updates","fail_count","insert_pct","Nt","sl_pct","m","beta","W0","Wt","delta_W","rho",
        "would_trigger","triggered","monitor_query_avg_ms","monitor_actual_sl_pct","monitor_recall",
        "monitor_precision","probe_ms","baseline_successful_updates","monitor_repeats",
        "monitor_baseline_total_ms","monitor_query_total_ms","monitor_delta_total_ms",
        "monitor_measurement","candidate_probe_ms","spi_baseline_ms","spi_filter_ms","spi_delta_ms",
        "check_reason","check_interval"});
    Table samples(o.dir / (mode + "_monitor_samples.csv"), {"mode","stage","epoch","event_id",
        "successful_updates","Nt","m","Wt","round","query_total_ms","learning_ms","collect_ms",
        "spi_setup_ms","spi_filter_ms"});
    Table eval_samples(o.dir / (mode + "_eval_samples.csv"), {"mode","stage","epoch","event_id",
        "successful_updates","Nt","eval_count","candidates_total","round","query_total_ms","query_avg_ms"});
    Table checkpoint_rows(o.dir / (mode + "_ul_checkpoints.csv"), {"mode","checkpoint_ul_pct",
        "successful_updates","Nt","epoch_before","event_id_before","event_id_after","rebuild_count",
        "W0_before","Wt","delta_W","rho","triggered","check_reason","check_interval",
        "candidate_probe_ms"});
    size_t success = 0, attempted = 0, learn = 0, art = 0, count = 0;
    size_t baseline_updates = 0;
    auto write_eval_samples = [&](const char* stage, const Measurement& m, size_t epoch, size_t event) {
        for (size_t r = 0; r < m.round_query_ms.size(); ++r)
            eval_samples.row({mode,stage,str(epoch),str(event),str(success),str(records.size()),
                str(eval.queries.size()),str(m.W),str(r+1),str(m.round_query_ms[r]),
                str(m.round_query_ms[r]/eval.queries.size())});
    };
    double insert_ms = 0, monitor_ms = 0, rebuild_ms = 0, rebaseline_ms = 0, eval_ms = 0, snapshot_ms = 0;
    auto baseline = measure(*idx, monitor, records, o.monitor_warmup, o.monitor_repeats);
    const double baseline_ms = baseline.wall_ms;
    int64_t W0 = baseline.W;
    double T0 = baseline.query_ms;
    double F0 = baseline.spi_filter_ms;
    auto write_trace = [&](const char* stage, const Measurement& m, bool trigger, double candidate_ms = 0,
                           const std::string& check_reason = "none", size_t check_interval = 0) {
        auto d = util::retrain_decision(m.W, W0, records.size(), o.beta);
        trace.row({mode,str(o.ul),stage,str(count),str(trigger ? count + 1 : count),str(attempted),str(success),"0",
            str(100.0L * success / base.data.size()),str(records.size()),str(o.sl),str(monitor.queries.size()),str(o.beta),
            str(W0),str(m.W),str(d.delta),str(d.rho),str(d.triggered),str(trigger),
            m.full_query ? str(m.avg(monitor.queries.size())) : "",
            m.full_query ? str(m.selectivity(monitor.queries.size(), records.size())) : "",
            m.full_query ? "1" : "",m.full_query ? "1" : "",str(m.wall_ms),
            str(baseline_updates),str(m.round_query_ms.size()),str(T0),
            m.full_query ? str(m.query_ms) : "",m.full_query ? str(m.query_ms-T0) : "",
            m.full_query ? "full_query" : "candidate_only",str(candidate_ms),str(F0),
            m.full_query ? str(m.spi_filter_ms) : "",m.full_query ? str(m.spi_filter_ms-F0) : "",
            check_reason,str(check_interval)});
        for (size_t r = 0; r < m.round_query_ms.size(); ++r)
            samples.row({mode,stage,str(count),str(trigger ? count + 1 : count),str(success),
                str(records.size()),str(monitor.queries.size()),str(m.W),str(r+1),str(m.round_query_ms[r]),
                str(m.round_learning_ms[r]),str(m.round_collect_ms[r]),str(m.round_spi_setup_ms[r]),
                str(m.round_spi_filter_ms[r])});
    };
    write_trace("baseline", baseline, false, 0, "baseline", 0);
    Measurement initial_eval;
    if (!o.pilot_timing_only) {
        initial_eval = measure(*idx, eval, records, o.warmup, o.repeats);
        eval_ms += initial_eval.wall_ms;
        write_eval_samples("initial", initial_eval, count, 0);
    }
    auto update_start = Clock::now();
    double update_eval_ms = 0;
    Measurement final_eval; size_t final_eval_at = std::numeric_limits<size_t>::max();
    bool stopped_after_rebuild = false;
    const bool hybrid_checks = o.coarse_check_every > 0;
    const size_t coarse_interval = hybrid_checks ? o.coarse_check_every : o.check_every;
    bool fine_mode = false;
    size_t next_scheduled_check = coarse_interval;
    size_t last_check_updates = 0;
    for (size_t i = 0; i < target; ++i) {
        DataPoint dp = inserts.data[i];
        dp.orig_id = static_cast<int32_t>(records.size());
        auto begin = Clock::now();
        auto result = idx->insert(dp.dimensions);
        insert_ms += ms(begin); ++attempted;
        if (result == InsertResult::Failed) throw std::runtime_error("insert failed at input row " + str(i + 1));
        ++success; if (result == InsertResult::LearningLayer) ++learn; else ++art;
        records.push_back(dp);
        if (idx->total_points() != records.size()) throw std::runtime_error("insert point count mismatch");
        const auto checkpoint = ul_checkpoints.find(success);
        const bool checkpoint_due = checkpoint != ul_checkpoints.end();
        const bool scheduled_due = success >= next_scheduled_check;
        const bool final_due = i + 1 == target;
        if (!scheduled_due && !checkpoint_due && !final_due) continue;
        std::string check_reason;
        if (scheduled_due) check_reason = hybrid_checks ? (fine_mode ? "fine" : "coarse") : "fixed";
        if (checkpoint_due) check_reason += (check_reason.empty() ? "checkpoint" : "+checkpoint");
        if (final_due && !checkpoint_due) check_reason += (check_reason.empty() ? "final" : "+final");
        const size_t actual_check_interval = success - last_check_updates;
        Measurement pre = count_candidates(*idx, monitor);
        monitor_ms += pre.wall_ms;
        const double candidate_ms = pre.wall_ms;
        const auto decision = util::retrain_decision(pre.W, W0, records.size(), o.beta);
        const bool trigger = adaptive && decision.triggered;
        std::cout << "[check] " << mode << " updates=" << success << " Nt=" << records.size()
                  << " rho=" << str(decision.rho) << " trigger=" << trigger << std::endl;
        if (trigger) {
            auto timed = measure(*idx, monitor, records, o.monitor_warmup, o.monitor_repeats);
            if (timed.W != pre.W) throw std::runtime_error("monitor candidate count changed during trigger measurement");
            monitor_ms += timed.wall_ms;
            pre = std::move(timed);
        }
        const size_t event_before = count;
        const int64_t checkpoint_W0 = W0;
        write_trace("check", pre, trigger, candidate_ms, check_reason, actual_check_interval);
        last_check_updates = success;
        if (!trigger) {
            if (hybrid_checks && !fine_mode && decision.rho >= o.fine_threshold_ratio * o.beta)
                fine_mode = true;
            if (scheduled_due || fine_mode)
                next_scheduled_check = success + (fine_mode ? o.check_every : coarse_interval);
            if (checkpoint_due)
                checkpoint_rows.row({mode,str(checkpoint->second),str(success),str(records.size()),str(event_before),
                    str(event_before),str(event_before),str(count),str(checkpoint_W0),str(pre.W),
                    str(decision.delta),str(decision.rho),"0",check_reason,str(actual_check_interval),str(candidate_ms)});
            continue;
        }
        Measurement eval_pre;
        if (!o.pilot_timing_only) {
            eval_pre = measure(*idx, eval, records, o.warmup, o.repeats);
            eval_ms += eval_pre.wall_ms; update_eval_ms += eval_pre.wall_ms;
            write_eval_samples("pre_rebuild", eval_pre, count, count+1);
        }
        auto event_start = Clock::now();
        auto next = std::make_unique<SULCipherIndex>(config, crypto);
        auto merged = records;
        auto build_start = Clock::now(); next->bulk_load(std::move(merged));
        const double build_ms = ms(build_start); rebuild_ms += build_ms;
        if (next->total_points() != records.size()) throw std::runtime_error("rebuild point count mismatch");
        auto post = measure(*next, monitor, records, o.monitor_warmup, o.monitor_repeats); rebaseline_ms += post.wall_ms;
        Measurement eval_post;
        if (!o.pilot_timing_only) {
            eval_post = measure(*next, eval, records, o.warmup, o.repeats);
            eval_ms += eval_post.wall_ms; update_eval_ms += eval_post.wall_ms;
            write_eval_samples("post_rebuild", eval_post, count+1, count+1);
        }
        const std::string filename = "event" + str(count + 1) + ".scidx";
        Snapshot event_snap{0,0,0};
        if (!o.pilot_timing_only) {
            event_snap = snapshot(*next, o.dir / filename, o, eval, records, eval_post);
            snapshot_ms += event_snap.wall_ms;
        }
        idx.swap(next); next.reset(); // 仅在构建、查询及快照往返校验均成功后替换。
        ++count;
        events.row({str(count),str(o.ul),str(success),str(100.0L * success / base.data.size()),str(records.size()),
            str(W0),str(pre.W),str(decision.delta),str(o.beta),str(decision.rho),str(post.W),
            str(util::retrain_decision(post.W, W0, records.size(), o.beta).rho),
            o.pilot_timing_only ? "" : str(eval_pre.avg(100)),
            o.pilot_timing_only ? "" : str(eval_post.avg(100)),
            (!o.pilot_timing_only && eval_post.query_ms > 0) ? str(eval_pre.query_ms/eval_post.query_ms) : "",
            o.pilot_timing_only ? "" : "1",o.pilot_timing_only ? "" : "1",
            o.pilot_timing_only ? "" : "1",o.pilot_timing_only ? "" : "1",
            o.pilot_timing_only ? "" : str(eval_pre.selectivity(100, records.size())),
            o.pilot_timing_only ? "" : str(eval_post.selectivity(100, records.size())),
            str(build_ms),str(ms(event_start)),str(post.wall_ms),o.pilot_timing_only ? "" : filename,
            o.pilot_timing_only ? "" : str(event_snap.bytes),o.pilot_timing_only ? "" : str(event_snap.kl1),
            str(count-1),str(baseline_updates),str(monitor.queries.size()),str(o.monitor_repeats),
            str(T0),str(pre.query_ms),str(pre.avg(monitor.queries.size())),str(pre.query_ms-T0),
            str(post.query_ms),str(post.avg(monitor.queries.size())),str(pre.query_ms-post.query_ms),
            str(o.beta*build_ms),build_ms > 0 ? str((pre.query_ms-T0)/build_ms) : "",
            build_ms > 0 ? str(pre.query_ms-T0 >= o.beta*build_ms) : "",
            str(F0),str(pre.spi_filter_ms),str(pre.spi_filter_ms-F0),
            str((pre.spi_filter_ms-F0)/monitor.queries.size()),str(post.spi_filter_ms),
            str(pre.spi_filter_ms-post.spi_filter_ms),
            build_ms > 0 ? str((pre.spi_filter_ms-F0)/build_ms) : "",
            build_ms > 0 ? str(pre.spi_filter_ms-F0 >= o.beta*build_ms) : ""});
        W0 = post.W; T0 = post.query_ms; F0 = post.spi_filter_ms; baseline_updates = success;
        fine_mode = false;
        next_scheduled_check = success + coarse_interval;
        write_trace("post_reset", post, false, 0, "post_reset", 0);
        if (checkpoint_due)
            checkpoint_rows.row({mode,str(checkpoint->second),str(success),str(records.size()),str(event_before),
                str(event_before),str(count),str(count),str(checkpoint_W0),str(pre.W),str(decision.delta),
                str(decision.rho),"1",check_reason,str(actual_check_interval),str(candidate_ms)});
        final_eval = eval_post; final_eval_at = success;
        std::cout << "[rebuild] event=" << count << " W_after=" << W0 << " build_ms=" << build_ms << std::endl;
        if (o.max_rebuilds && count >= o.max_rebuilds) {
            stopped_after_rebuild = true;
            break;
        }
    }
    const double update_wall_ms = ms(update_start);
    Snapshot final_snap{0,0,0};
    Measurement loaded_eval;
    if (!o.pilot_timing_only) {
        if (final_eval_at != success) {
            final_eval = measure(*idx, eval, records, o.warmup, o.repeats); eval_ms += final_eval.wall_ms;
            write_eval_samples("final", final_eval, count, count);
        }
        final_snap = snapshot(*idx, o.dir / (mode + "_final.scidx"), o, eval, records, final_eval);
        snapshot_ms += final_snap.wall_ms;
        auto loaded = SULCipherIndex::load_from_file((o.dir / (mode + "_final.scidx")).string());
        loaded_eval = measure(*loaded.second, eval, records, o.warmup, o.repeats); eval_ms += loaded_eval.wall_ms;
        write_eval_samples("loaded_final", loaded_eval, count, count);
    }
    summary.row({mode,str(base.data.size()),str(records.size()),str(attempted),str(success),"0",str(learn),str(art),
        str(count),str(o.beta),str(o.sl),str(monitor.queries.size()),"100",str(o.check_every),str(o.ul),
        str(success == target),stopped_after_rebuild ? "max_rebuilds" : "target_reached",
        str(100.0L*success/base.data.size()),str(insert_ms),str(insert_ms/attempted),str(baseline_ms),str(monitor_ms),
        str(rebaseline_ms),str(rebuild_ms),str(eval_ms),str(snapshot_ms),str(update_wall_ms),
        str(update_wall_ms-update_eval_ms),str(ms(phase_start)),
        o.pilot_timing_only ? "" : str(initial_eval.avg(100)),
        o.pilot_timing_only ? "" : str(final_eval.avg(100)),
        o.pilot_timing_only ? "" : str(loaded_eval.avg(100)),
        o.pilot_timing_only ? "" : "1",o.pilot_timing_only ? "" : "1",
        o.pilot_timing_only ? "" : str(final_eval.W/100.0),
        o.pilot_timing_only ? "" : str(final_eval.selectivity(100, records.size())),
        o.pilot_timing_only ? "" : str(final_eval.learning_ms/100),
        o.pilot_timing_only ? "" : str(final_eval.collect_ms/100),
        o.pilot_timing_only ? "" : str(final_eval.spi_setup_ms/100),
        o.pilot_timing_only ? "" : str(final_eval.spi_filter_ms/100),
        o.pilot_timing_only ? "" : str(final_snap.bytes),o.pilot_timing_only ? "" : str(final_snap.kl1),
        stopped_after_rebuild ? "complete_pilot" : "complete"});
    return count;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") { usage(); return 0; }
    fs::path active_dir;
    try {
        auto o = options(argc, argv);
        auto base = util::load_csv(o.base);
        auto inserts = util::load_csv(o.insert, base.normalization);
        auto eval = util::load_query_file(o.eval), monitor = util::load_query_file(o.monitor);
        const auto m = util::monitor_query_count(o.sl);
        if (eval.queries.size() != 100) throw std::invalid_argument("evaluation file must contain exactly 100 queries");
        if (monitor.queries.size() < m) throw std::invalid_argument("insufficient monitor queries: need " + str(m));
        if (base.dim_count != eval.dim_count || base.dim_count != monitor.dim_count)
            throw std::invalid_argument("query dimension mismatch");
        monitor.queries.resize(m);
        if (fs::equivalent(o.eval, o.monitor)) throw std::invalid_argument("monitor and evaluation files must be separate");
        auto wanted = std::ceil(base.data.size() * o.ul / 100);
        if (wanted < 1 || wanted > inserts.data.size()) throw std::invalid_argument("insert file too short for requested ul_pct (no truncation)");
        if (base.data.size() + wanted > INT32_MAX) throw std::invalid_argument("record ID capacity exceeded");
        const size_t target = static_cast<size_t>(wanted);
        if (!o.check_every) o.check_every = std::max<size_t>(1, (base.data.size()+99)/100);
        if (o.coarse_check_every && o.coarse_check_every < o.check_every)
            throw std::invalid_argument("coarse-check-every must be >= check-every");
        std::map<size_t,long double> ul_checkpoints;
        for (auto pct : o.ul_checkpoints) {
            const auto checkpoint = std::ceil(base.data.size() * pct / 100);
            if (checkpoint < 1 || checkpoint > target)
                throw std::invalid_argument("ul checkpoint outside insertion target");
            if (!ul_checkpoints.emplace(static_cast<size_t>(checkpoint), pct).second)
                throw std::invalid_argument("ul checkpoints map to duplicate insertion counts");
        }
        // Existing duplicate coordinates are valid records. New workload points must be new keys.
        std::set<std::vector<int32_t>> coordinates;
        std::vector<int32_t> base_source_ids;
        for (size_t i = 0; i < base.data.size(); ++i) {
            base_source_ids.push_back(base.data[i].orig_id);
            base.data[i].orig_id = static_cast<int32_t>(i);
            coordinates.emplace(base.data[i].dimensions, base.data[i].dimensions + base.dim_count);
        }
        const size_t base_duplicates = base.data.size() - coordinates.size();
        for (size_t i = 0; i < target; ++i)
            if (!coordinates.emplace(inserts.data[i].dimensions, inserts.data[i].dimensions + base.dim_count).second)
                throw std::invalid_argument("insert overlaps base/earlier insert after quantization at row " + str(i+1));
        if (fs::exists(o.dir)) throw std::invalid_argument("run directory exists: " + o.dir.string());
        fs::create_directories(o.dir.parent_path().empty() ? fs::path(".") : o.dir.parent_path());
        if (!fs::create_directory(o.dir)) throw std::runtime_error("cannot reserve run directory");
        active_dir = o.dir;
        if (fs::exists(o.insert + ".json"))
            fs::copy_file(o.insert + ".json", o.dir / "insert_manifest.json");
        Table ids(o.dir / "record_ids.csv", {"kind","source_row","source_id","internal_id"});
        for (size_t i = 0; i < base.data.size(); ++i)
            ids.row({"base",str(i+1),str(base_source_ids[i]),str(i)});
        for (size_t i = 0; i < target; ++i)
            ids.row({"insert",str(i+1),str(inserts.data[i].orig_id),str(base.data.size()+i)});
        Table meta(o.dir / "metadata.csv", {"key","value"});
        for (auto kv : std::vector<std::pair<std::string,std::string>>{
            {"schema","retrain_v2"},{"dataset",o.label},{"distribution",o.distribution},
            {"base",fs::absolute(o.base).string()},{"insert",fs::absolute(o.insert).string()},
            {"eval",fs::absolute(o.eval).string()},{"monitor",fs::absolute(o.monitor).string()},
            {"base_fnv1a64",fingerprint(o.base)},{"insert_fnv1a64",fingerprint(o.insert)},
            {"eval_fnv1a64",fingerprint(o.eval)},{"monitor_fnv1a64",fingerprint(o.monitor)},
            {"N_init",str(base.data.size())},{"base_duplicate_coordinates",str(base_duplicates)},
            {"K",str(o.K)},{"err",str(o.err)},{"dim",str(base.dim_count)},{"sl_pct",str(o.sl)},
            {"m",str(m)},{"eval_count","100"},{"beta",str(o.beta)},{"check_every",str(o.check_every)},
            {"coarse_check_every",str(o.coarse_check_every)},
            {"fine_threshold_ratio",str(o.fine_threshold_ratio)},
            {"check_schedule",o.coarse_check_every ? "hybrid" : "fixed"},
            {"target_updates",str(target)},{"configured_ul_pct",str(o.ul)},{"ul_pct",str(o.ul)},
            {"max_rebuilds",str(o.max_rebuilds)},
            {"pilot_timing_only",str(o.pilot_timing_only)},
            {"ul_mode",o.ul_checkpoints.size() > 1 ? "continuous_checkpoints" : "single_target"},
            {"ul_checkpoints",join(o.ul_checkpoints)},
            {"repeat_id",str(o.repeat_id)},{"phase",o.phase},
            {"warmup_rounds",str(o.warmup)},{"eval_repeats",str(o.repeats)},
            {"time_measurement_schema","5"},{"monitor_warmup_rounds",str(o.monitor_warmup)},
            {"monitor_repeats",str(o.monitor_repeats)},
            {"monitor_schedule","candidate-only checks; full queries at baseline/trigger/post-reset"},
            {"query_time_scope","sum of query-call wall times per round; mean across repeats; excludes warmup/validation/CSV"},
            {"spi_time_scope","sum of SPI filtering times for m monitor queries; excludes learning, collection and query encryption"},
            {"rebuild_time_scope","bulk_load only; includes plain/cipher index build; excludes input copy/keygen/probes/snapshots"},
            {"query_parallelism","serial queries; paired SIC via main + std::thread; OpenMP-enabled ophelib"},
            {"normalization","base min/max; scale=65536; domain rejection"}}) meta.row({kv.first,kv.second});
        for (const char* key : {"OMP_NUM_THREADS", "OMP_DYNAMIC", "OMP_THREAD_LIMIT", "OMP_PROC_BIND", "OMP_PLACES"}) {
            const char* value = std::getenv(key);
            meta.row({key,value ? value : "unset"});
        }
        for (int d = 0; d < base.dim_count; ++d) {
            meta.row({"coordinate_low_"+str(d),str(base.normalization.low[d])});
            meta.row({"coordinate_high_"+str(d),str(base.normalization.high[d])});
        }
        Table summary(o.dir / "summary.csv", {"mode","N_init","Nt","attempted_updates","successful_updates","fail_count",
            "learn_cnt","art_cnt","rebuild_count","beta","sl_pct","m","eval_count","check_every","configured_ul_pct",
            "target_reached","stop_reason",
            "insert_pct","insert_total_ms","insert_avg_ms","baseline_probe_ms","monitor_total_ms","rebaseline_total_ms",
            "rebuild_total_ms","eval_total_ms","snapshot_total_ms","update_wall_ms","update_maintenance_ms","run_wall_ms",
            "initial_eval_query_avg_ms","final_eval_query_avg_ms","loaded_eval_query_avg_ms","eval_recall","eval_precision",
            "eval_candidates_avg","eval_actual_sl_pct","learning_ms_avg","collect_ms_avg","spi_setup_ms_avg","spi_filter_ms_avg",
            "file_bytes","file_bytes_kl1","status"});
        Table events(o.dir / "events.csv", event_header);
        std::cout << "[run] " << o.dir << " m=" << m << " eval=100 check_every=" << o.check_every
                  << " base_duplicate_records=" << base_duplicates << std::endl;
        if (o.phase != "adaptive") phase(o, base, inserts, monitor, eval, target, ul_checkpoints, false, summary, events);
        if (o.phase != "control") phase(o, base, inserts, monitor, eval, target, ul_checkpoints, true, summary, events);
        std::ofstream(o.dir / "status.txt") << "complete\n";
        std::cout << "[complete] results=" << o.dir << std::endl;
        return 0;
    } catch (const std::exception& e) {
        if (!active_dir.empty()) std::ofstream(active_dir / "status.txt") << "failed: " << e.what() << '\n';
        std::cerr << "[error] " << e.what() << '\n';
        return 2;
    }
}
