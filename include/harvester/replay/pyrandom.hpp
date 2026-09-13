// CPython's ``random.Random``, bit for bit.
//
// The generated market is seeded, and a seed is only worth something if
// the same seed gives the same tape in both implementations: then a
// replay here can be checked number for number against the Python
// reference.  That needs CPython's exact generator -- MT19937 seeded with
// ``init_by_array``, 53-bit doubles from two draws, ``randint`` through
// rejection sampling on ``getrandbits`` -- which ``std::mt19937`` does
// not provide (it seeds differently).  This is that generator, and the
// tests pin it against values produced by the interpreter.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace harvester {

class PyRandom {
public:
    explicit PyRandom(std::uint64_t seed) { seed_from(seed); }

    // ``random.seed(n)`` for a non-negative int.
    void seed_from(std::uint64_t seed) {
        std::array<std::uint32_t, 2> key{static_cast<std::uint32_t>(seed & 0xffffffffU),
                                         static_cast<std::uint32_t>(seed >> 32)};
        init_by_array(key.data(), key[1] != 0 ? 2 : 1);
    }

    std::uint32_t genrand_uint32() {
        static constexpr std::uint32_t kMatrixA = 0x9908b0dfU;
        static constexpr std::uint32_t kUpper = 0x80000000U;
        static constexpr std::uint32_t kLower = 0x7fffffffU;
        if (mti_ >= N) {
            int kk = 0;
            for (; kk < N - M; ++kk) {
                const std::uint32_t y = (mt_[kk] & kUpper) | (mt_[kk + 1] & kLower);
                mt_[kk] = mt_[kk + M] ^ (y >> 1) ^ ((y & 1U) ? kMatrixA : 0U);
            }
            for (; kk < N - 1; ++kk) {
                const std::uint32_t y = (mt_[kk] & kUpper) | (mt_[kk + 1] & kLower);
                mt_[kk] = mt_[kk + (M - N)] ^ (y >> 1) ^ ((y & 1U) ? kMatrixA : 0U);
            }
            const std::uint32_t y = (mt_[N - 1] & kUpper) | (mt_[0] & kLower);
            mt_[N - 1] = mt_[M - 1] ^ (y >> 1) ^ ((y & 1U) ? kMatrixA : 0U);
            mti_ = 0;
        }
        std::uint32_t y = mt_[mti_++];
        y ^= (y >> 11);
        y ^= (y << 7) & 0x9d2c5680U;
        y ^= (y << 15) & 0xefc60000U;
        y ^= (y >> 18);
        return y;
    }

    // ``random.random()``: a double in [0, 1) with 53 bits of randomness.
    double random() {
        const std::uint32_t a = genrand_uint32() >> 5;
        const std::uint32_t b = genrand_uint32() >> 6;
        return (a * 67108864.0 + b) * (1.0 / 9007199254740992.0);
    }

    // ``random.getrandbits(k)`` for 0 < k <= 64.
    std::uint64_t getrandbits(int k) {
        if (k <= 32) return genrand_uint32() >> (32 - k);
        std::uint64_t out = 0;
        int shift = 0;
        for (int remaining = k; remaining > 0; remaining -= 32, shift += 32) {
            std::uint32_t r = genrand_uint32();
            if (remaining < 32) r >>= (32 - remaining);
            out |= static_cast<std::uint64_t>(r) << shift;
        }
        return out;
    }

    // ``Random._randbelow(n)``: an int in [0, n).
    std::uint64_t randbelow(std::uint64_t n) {
        int k = 0;
        for (std::uint64_t v = n; v != 0; v >>= 1) ++k;
        std::uint64_t r = getrandbits(k);
        while (r >= n) r = getrandbits(k);
        return r;
    }

    // ``random.randint(a, b)``: an int in [a, b].
    std::int64_t randint(std::int64_t a, std::int64_t b) {
        return a + static_cast<std::int64_t>(randbelow(static_cast<std::uint64_t>(b - a + 1)));
    }

    // ``random.choice(seq)``.
    template <typename Seq>
    auto choice(const Seq& seq) -> decltype(seq[0]) {
        return seq[randbelow(seq.size())];
    }

    // ``random.expovariate(lambd)``.
    double expovariate(double lambd) { return -std::log(1.0 - random()) / lambd; }

private:
    static constexpr int N = 624;
    static constexpr int M = 397;

    void init_genrand(std::uint32_t s) {
        mt_[0] = s;
        for (mti_ = 1; mti_ < N; ++mti_) {
            mt_[mti_] = (1812433253U * (mt_[mti_ - 1] ^ (mt_[mti_ - 1] >> 30)) + static_cast<std::uint32_t>(mti_));
        }
    }

    void init_by_array(const std::uint32_t* key, std::size_t key_length) {
        init_genrand(19650218U);
        std::size_t i = 1;
        std::size_t j = 0;
        std::size_t k = N > key_length ? N : key_length;
        for (; k; --k) {
            mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1664525U)) + key[j] + static_cast<std::uint32_t>(j);
            ++i;
            ++j;
            if (i >= N) {
                mt_[0] = mt_[N - 1];
                i = 1;
            }
            if (j >= key_length) j = 0;
        }
        for (k = N - 1; k; --k) {
            mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1566083941U)) - static_cast<std::uint32_t>(i);
            ++i;
            if (i >= N) {
                mt_[0] = mt_[N - 1];
                i = 1;
            }
        }
        mt_[0] = 0x80000000U;
        mti_ = N;
    }

    std::array<std::uint32_t, N> mt_{};
    int mti_ = N + 1;
};

}  // namespace harvester
