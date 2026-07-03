#pragma once
#include <cstdint>
#include <array>
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// LatencyHistogram
//
// Records nanosecond-scale latency samples into power-of-two buckets
// (bucket i covers [2^i, 2^(i+1)) nanoseconds), similar in spirit to
// HdrHistogram. This is the right tool for a matching engine benchmark:
// throughput (orders/sec) tells you the average, but what actually matters
// for HFT-style systems is the TAIL -- p99 / p99.9 / max -- because a single
// slow order can mean a missed fill or a stale quote. A sorted vector of all
// samples would also work but costs O(N log N) to summarize and O(N) memory
// growth; this is O(1) per recorded sample and O(num_buckets) to query any
// percentile.
// ---------------------------------------------------------------------------
class LatencyHistogram {
public:
    // 48 buckets covers ~1ns up to ~140 years in nanoseconds -- comfortably
    // covers anything from a cache hit (~1ns) to a multi-second GC pause.
    static constexpr int kBuckets = 48;

    void record(uint64_t nanos) {
        int bucket = nanos == 0 ? 0 : std::min(kBuckets - 1, 63 - __builtin_clzll(nanos));
        counts_[bucket]++;
        total_++;
        sum_ += nanos;
        if (nanos < min_) min_ = nanos;
        if (nanos > max_) max_ = nanos;
    }

    uint64_t count() const { return total_; }
    uint64_t minNs() const { return total_ ? min_ : 0; }
    uint64_t maxNs() const { return max_; }
    double meanNs() const { return total_ ? static_cast<double>(sum_) / total_ : 0.0; }

    // Approximate percentile: returns the upper bound of the bucket that
    // contains the requested percentile rank. Accurate to within the
    // bucket's power-of-two range, which is the standard tradeoff for
    // O(1)-recording histograms.
    uint64_t percentile(double p) const {
        if (total_ == 0) return 0;
        uint64_t target = static_cast<uint64_t>(std::ceil(p * static_cast<double>(total_)));
        uint64_t cumulative = 0;
        for (int i = 0; i < kBuckets; ++i) {
            cumulative += counts_[i];
            if (cumulative >= target) {
                return (i + 1 == kBuckets) ? max_ : (1ull << (i + 1));
            }
        }
        return max_;
    }

private:
    std::array<uint64_t, kBuckets> counts_{};
    uint64_t total_ = 0;
    uint64_t sum_ = 0;
    uint64_t min_ = UINT64_MAX;
    uint64_t max_ = 0;
};
