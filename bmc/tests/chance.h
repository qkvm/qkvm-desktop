#pragma once

#include <cstdint>

namespace qkvm::bmc::tests {

// Chance that is the same at every run, on every computer (SplitMix64), for the fuzz tests.
class Chance {
public:
    explicit Chance(std::uint64_t seed) : m_state(seed) {}
    std::uint64_t next()
    {
        std::uint64_t z = (m_state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    int below(int limit) { return static_cast<int>(next() % static_cast<std::uint64_t>(limit)); }
    int between(int low, int high) { return low + below(high - low + 1); }
    bool oneIn(int many) { return below(many) == 0; }

private:
    std::uint64_t m_state;
};

} // namespace qkvm::bmc::tests
