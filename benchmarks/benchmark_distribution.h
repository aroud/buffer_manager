#ifndef BUFFER_MANAGER_BENCHMARKS_BENCHMARK_DISTRIBUTION_H_
#define BUFFER_MANAGER_BENCHMARKS_BENCHMARK_DISTRIBUTION_H_

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <stdexcept>
#include <vector>

namespace buffer_manager::main_benchmark {

class ZipfGenerator final {
 public:
  ZipfGenerator(std::uint64_t item_count, double theta)
      : n_(item_count - 1),
        theta_(theta),
        alpha_(1.0 / (1.0 - theta)),
        zeta_n_(CalcZeta(item_count - 1, theta)),
        eta_((1.0 - std::pow(2.0 / static_cast<double>(item_count - 1),
                             1.0 - theta)) /
             (1.0 - (CalcZeta(2, theta) / zeta_n_))) {
    if (item_count <= 1) {
      throw std::invalid_argument("Zipf item_count must be greater than one");
    }
  }

  [[nodiscard]] std::uint64_t Rand(std::mt19937_64& rng) const {
    std::uniform_real_distribution<double> distribution(0.0, 1.0);

    const double rand_float = distribution(rng);
    const double uz = rand_float * zeta_n_;

    if (uz < 1.0) {
      return 1;
    }

    if (uz < 1.0 + std::pow(0.5, theta_)) {
      return 2;
    }

    return 1 + static_cast<std::uint64_t>(
                   static_cast<double>(n_) *
                   std::pow((eta_ * rand_float) - eta_ + 1.0, alpha_));
  }

 private:
  [[nodiscard]] static double CalcZeta(std::uint64_t n, double theta) {
    double result = 0.0;

    for (std::uint64_t i = 1; i <= n; ++i) {
      result += std::pow(1.0 / static_cast<double>(i), theta);
    }

    return result;
  }

  std::uint64_t n_;
  double theta_;
  double alpha_;
  double zeta_n_;
  double eta_;
};

class ScrambledZipfGenerator final {
 public:
  ScrambledZipfGenerator(std::uint64_t min, std::uint64_t max, double theta)
      : min_(min), n_(max - min), zipf_(max - min, theta) {
    if (max <= min) {
      throw std::invalid_argument("Zipf max must be greater than min");
    }
  }

  [[nodiscard]] std::uint64_t Rand(std::mt19937_64& rng) const {
    const std::uint64_t zipf_value = zipf_.Rand(rng);
    return min_ + (Fnv1a64(zipf_value) % n_);
  }

 private:
  [[nodiscard]] static std::uint64_t Fnv1a64(std::uint64_t value) {
    constexpr std::uint64_t kOffsetBasis = 14695981039346656037ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;

    std::uint64_t hash = kOffsetBasis;

    std::array<unsigned char, sizeof(value)> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(value));

    for (unsigned char byte : bytes) {
      hash ^= static_cast<std::uint64_t>(byte);
      hash *= kPrime;
    }

    return hash;
  }

  std::uint64_t min_;
  std::uint64_t n_;
  ZipfGenerator zipf_;
};

[[nodiscard]] inline std::vector<std::uint64_t> GenScrambledZipfSequence(
    std::uint64_t min, std::uint64_t max, double theta, std::mt19937_64& rng,
    std::uint64_t sequence_length) {
  std::vector<std::uint64_t> sequence;
  sequence.reserve(static_cast<std::size_t>(sequence_length));

  if (min == max) {
    sequence.assign(static_cast<std::size_t>(sequence_length), min);
    return sequence;
  }

  ScrambledZipfGenerator generator(min, max, theta);

  for (std::uint64_t i = 0; i < sequence_length; ++i) {
    sequence.push_back(generator.Rand(rng));
  }

  return sequence;
}

}  // namespace buffer_manager::main_benchmark

#endif  // BUFFER_MANAGER_BENCHMARKS_BENCHMARK_DISTRIBUTION_H_
