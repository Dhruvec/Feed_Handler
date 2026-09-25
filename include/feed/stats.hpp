#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace feed {

// Bounded-memory latency accumulator. Keeps an exact reservoir sample (every
// value until the cap is reached, then uniform random replacement), so
// percentiles are exact for the common case of a single pass over a feed and
// statistically sound beyond it. The max is tracked exactly regardless.
class LatencyStats {
public:
  static constexpr std::size_t kCapacity = std::size_t{1} << 20; // 1M samples

  LatencyStats() { samples_.reserve(kCapacity); }

  void record(uint64_t ns) noexcept {
    if (total_ == 0 || ns < min_) min_ = ns;
    if (ns > max_) max_ = ns;
    sum_ += ns;
    ++total_;

    if (samples_.size() < kCapacity) {
      samples_.push_back(ns);
    } else {
      const uint64_t j = next_random() % total_;
      if (j < kCapacity) samples_[static_cast<std::size_t>(j)] = ns;
    }
  }

  std::size_t total() const noexcept { return total_; }
  std::size_t stored() const noexcept { return samples_.size(); }
  uint64_t min() const noexcept { return total_ ? min_ : 0; }
  uint64_t max() const noexcept { return total_ ? max_ : 0; }
  double mean() const noexcept {
    return total_ ? static_cast<double>(sum_) / static_cast<double>(total_) : 0.0;
  }

  // Nearest-rank percentile in [0, 100].
  uint64_t percentile(double p) const {
    if (samples_.empty()) return 0;
    std::vector<uint64_t> v(samples_.begin(), samples_.end());
    std::sort(v.begin(), v.end());
    if (p <= 0.0) return v.front();
    if (p >= 100.0) return v.back();

    const double rank = (p / 100.0) * static_cast<double>(v.size());
    std::size_t idx = static_cast<std::size_t>(std::ceil(rank));
    if (idx > 0) --idx;
    if (idx >= v.size()) idx = v.size() - 1;
    return v[idx];
  }

private:
  uint64_t next_random() noexcept {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 7;
    rng_ ^= rng_ << 17;
    return rng_;
  }

  std::vector<uint64_t> samples_;
  std::size_t           total_ = 0;
  uint64_t              min_ = 0;
  uint64_t              max_ = 0;
  uint64_t              sum_ = 0;
  uint64_t              rng_ = 0x9E3779B97F4A7C15ull;
};

} // namespace feed
