#include "difficulty.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace probe {

namespace {
double ndtr(double x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); }
}  // namespace

double hitProbability(double windowMs, double hz, double sigmaMs, int phases) {
    if (!std::isfinite(windowMs)) return std::numeric_limits<double>::quiet_NaN();
    const double half = windowMs / 2.0;
    if (!std::isfinite(hz)) return ndtr(half / sigmaMs) - ndtr(-half / sigmaMs);
    const double frame = 1000.0 / hz;
    const int maxK = static_cast<int>(std::ceil(windowMs / frame)) + 2;
    const double aim = -frame / 2.0;
    double total = 0.0;
    for (int i = 0; i < phases; ++i) {
        const double phi = (i + 0.5) / phases * frame;
        double p = 0.0;
        for (int k = -maxK; k <= maxK; ++k) {
            const double g = phi + k * frame;
            if (g > -half && g <= half) p += ndtr((g - aim) / sigmaMs) - ndtr((g - frame - aim) / sigmaMs);
        }
        total += p;
    }
    return total / phases;
}

std::vector<DifficultyRow> levelDifficulty(const std::vector<double>& windowsMs, const std::vector<double>& hzList,
                                           double sigmaMs) {
    std::vector<DifficultyRow> rows;
    for (double hz : hzList) {
        DifficultyRow row;
        row.hz = hz;
        double luck = 1.0;
        for (double w : windowsMs) {
            if (!std::isfinite(w)) continue;
            const double p = std::clamp(hitProbability(w, hz, sigmaMs), 1e-300, 1.0);
            row.nats += -std::log(p);
            row.worstP = std::min(row.worstP, p);
            luck *= std::isfinite(hz) ? std::min(1.0, w * hz / 1000.0) : 1.0;
        }
        row.log10Attempts = row.nats / std::log(10.0);
        row.impossibleShare = 1.0 - luck;
        rows.push_back(row);
    }
    return rows;
}

}  // namespace probe
