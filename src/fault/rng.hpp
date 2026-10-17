#pragma once

#include <cstdint>

namespace ast {

// xorshift64* PRNG: fast, deterministic, seeded. Used by the fault simulator
// and the workload generator so runs are reproducible from a seed.
class Rng {
public:
    explicit Rng(uint64_t seed = 0x9E3779B97F4A7C15ULL) : state_(seed ? seed : 1) {}

    uint64_t next_u64() {
        uint64_t x = state_;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        state_ = x;
        return x * 0x2545F4914F6CDD1DULL;
    }

    uint64_t next_below(uint64_t n) { // uniform in [0, n)
        return n ? next_u64() % n : 0;
    }

    uint32_t next_u32() { return (uint32_t)next_u64(); }

    double next_double() { // uniform in [0, 1)
        return (double)(next_u64() >> 11) * (1.0 / 9007199254740992.0);
    }

private:
    uint64_t state_;
};

} // namespace ast
