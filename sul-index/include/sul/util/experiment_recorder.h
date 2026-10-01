#pragma once

#include <string>
#include <vector>

namespace sul::util {

// Experiment parameter tags used in output file names:
//   <kind>_<dataset_stem>_K{K}_err{err}_dim{dim}{extra}.csv
// N is already present in dataset_stem (for example, uniform_20000), so no separate
// _N suffix is appended. Callers customize extra for each experiment type, for example:
//   range  → "_sl0p25"
//   work   → "_R80W20"
//   update → "_ul0p25"
struct ExpParams {
    int K   = 0;
    int err = 0;
    int dim = 0;
    std::string dataset_stem;
    std::string extra;
};

class ExperimentRecorder {
public:
    // Return an absolute record/{kind}_<stem>_K..._err..._dim...{extra}.csv path.
    // An empty record_dir falls back to ./record/ relative to the working directory.
    static std::string build_path(const std::string& kind,
                                  const ExpParams& p,
                                  const std::string& record_dir = "");

    // Append one row, creating the file and header when necessary.
    // header and row must have equal lengths.
    static void append_row(const std::string& path,
                           const std::vector<std::string>& header,
                           const std::vector<std::string>& row);

    // ISO-8601 local timestamp, for example 2026-05-22T15:03:01.
    static std::string now_iso();

    // Convert a floating-point value without scientific notation.
    static std::string ftoa(double v);

    // Convert a decimal value to a file-name-safe tag.
    //   pct_tag(0.25) -> "0p25"
    static std::string pct_tag(double pct);
};

} // namespace sul::util
