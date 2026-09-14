#pragma once

#include <cstddef>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace sul::util {

inline size_t monitor_query_count(long double sl_pct) {
    if (!std::isfinite(sl_pct) || sl_pct <= 0 || sl_pct > 100)
        throw std::invalid_argument("sl-pct must be in (0,100]");
    long double count = 100 / sl_pct;
    const auto nearest = std::round(count);
    if (std::fabs(count - nearest) <= 16 * std::numeric_limits<long double>::epsilon() * count)
        count = nearest;
    count = std::ceil(count);
    // Explicit resource limit, never truncate m and change the trigger semantics.
    if (count > 1000000) throw std::invalid_argument("monitor workload exceeds 1000000 queries");
    return static_cast<size_t>(count);
}

struct RetrainDecision {
    int64_t delta;
    long double rho;
    bool triggered;
};

inline RetrainDecision retrain_decision(int64_t current, int64_t baseline,
                                        size_t n, long double beta) {
    if (current < 0 || baseline < 0 || n == 0 || !std::isfinite(beta) || beta <= 0)
        throw std::invalid_argument("invalid retrain state");
    const int64_t delta = current - baseline; // Both operands nonnegative: no signed overflow.
    return {delta, static_cast<long double>(delta) / n,
            static_cast<long double>(delta) >= beta * n};
}

} // namespace sul::util
