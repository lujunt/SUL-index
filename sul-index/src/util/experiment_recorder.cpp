#include "sul/util/experiment_recorder.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/types.h>

namespace sul::util {

namespace {

bool file_exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

void ensure_dir(const std::string& dir) {
    if (dir.empty()) return;
    struct stat st{};
    if (::stat(dir.c_str(), &st) == 0) return;
    ::mkdir(dir.c_str(), 0755);
}

std::string join_row(const std::vector<std::string>& cells) {
    std::ostringstream os;
    for (size_t i = 0; i < cells.size(); ++i) {
        if (i) os << ',';
        const auto& s = cells[i];
        bool needs_quote = s.find_first_of(",\"\n") != std::string::npos;
        if (needs_quote) {
            os << '"';
            for (char c : s) {
                if (c == '"') os << "\"\"";
                else os << c;
            }
            os << '"';
        } else {
            os << s;
        }
    }
    return os.str();
}

} // namespace

std::string ExperimentRecorder::build_path(const std::string& kind,
                                           const ExpParams& p,
                                           const std::string& record_dir) {
    std::string dir = record_dir.empty() ? "record" : record_dir;
    ensure_dir(dir);
    std::ostringstream os;
    os << dir << '/' << kind
       << "_K" << p.K
       << "_err" << p.err
       << "_dim" << p.dim
       << "_N" << p.N
       << p.extra
       << ".csv";
    return os.str();
}

void ExperimentRecorder::append_row(const std::string& path,
                                    const std::vector<std::string>& header,
                                    const std::vector<std::string>& row) {
    if (header.size() != row.size()) {
        throw std::invalid_argument(
            "ExperimentRecorder::append_row header/row size mismatch");
    }
    bool first_time = !file_exists(path);
    std::ofstream out(path, std::ios::out | std::ios::app);
    if (!out) {
        throw std::runtime_error("ExperimentRecorder: cannot open " + path);
    }
    if (first_time) {
        out << join_row(header) << '\n';
    }
    out << join_row(row) << '\n';
}

std::string ExperimentRecorder::now_iso() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return std::string(buf);
}

std::string ExperimentRecorder::ftoa(double v) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(6) << v;
    std::string s = os.str();
    if (s.find('.') != std::string::npos) {
        auto last = s.find_last_not_of('0');
        if (last != std::string::npos && s[last] == '.') --last;
        s.erase(last + 1);
    }
    return s;
}

std::string ExperimentRecorder::pct_tag(double pct) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", pct);
    std::string s = buf;
    for (auto& c : s) {
        if (c == '.') c = 'p';
    }
    return s;
}

} // namespace sul::util
