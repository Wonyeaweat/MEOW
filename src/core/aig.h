#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

// =============================================================================
// Weight — 256-bit unsigned integer (4 × uint64_t, little-endian)
// Supports up to 256 outputs with 2^i weights without overflow.
// No saturation, no clamping. Overflow = bug → let it crash.
// =============================================================================
class Weight {
    static constexpr size_t NWORDS = 4;
    uint64_t w[NWORDS] = {};

  public:
    Weight() = default;

    explicit Weight(uint64_t val) {
        w[0] = val;
        // w[1..3] already zero
    }

    // Construct 2^bit_index
    static Weight from_bit(size_t bit_index) {
        Weight r;
        r.w[bit_index / 64] = 1ULL << (bit_index % 64);
        return r;
    }

    static Weight from_dec_string(const std::string& text) {
        if (text.empty())
            throw std::invalid_argument("Weight decimal string is empty");

        size_t pos = 0;
        if (text[pos] == '+') {
            pos++;
        } else if (text[pos] == '-') {
            throw std::invalid_argument("Weight must be non-negative: " + text);
        }
        if (pos >= text.size())
            throw std::invalid_argument("Weight decimal string has no digits: " + text);

        const size_t digits_begin = pos;
        for (size_t index = digits_begin; index < text.size(); ++index) {
            if (text[index] < '0' || text[index] > '9')
                throw std::invalid_argument("Invalid decimal digit in Weight: " + text);
        }
        while (pos < text.size() && text[pos] == '0') ++pos;
        static constexpr const char* maximum_decimal =
            "115792089237316195423570985008687907853269984665640564039457584007913129639935";
        const std::string significant =
            pos == text.size() ? "0" : text.substr(pos);
        const std::string maximum(maximum_decimal);
        if (significant.size() > maximum.size() ||
            (significant.size() == maximum.size() && significant > maximum))
            throw std::out_of_range("Weight exceeds 256 bits: " + text);

        Weight value;
        for (pos = digits_begin; pos < text.size(); ++pos) {
            char ch = text[pos];
            uint64_t digit = static_cast<uint64_t>(ch - '0');

            Weight x2 = value + value;
            Weight x4 = x2 + x2;
            Weight x8 = x4 + x4;
            value = x8 + x2;
            value += Weight(digit);
        }
        return value;
    }

    // -----------------------------------------------------------------------
    // Comparison
    // -----------------------------------------------------------------------
    int cmp(const Weight& rhs) const {
        for (int i = static_cast<int>(NWORDS) - 1; i >= 0; --i) {
            if (w[i] != rhs.w[i])
                return w[i] > rhs.w[i] ? 1 : -1;
        }
        return 0;
    }

    bool operator<(const Weight& rhs) const { return cmp(rhs) < 0; }
    bool operator>(const Weight& rhs) const { return cmp(rhs) > 0; }
    bool operator<=(const Weight& rhs) const { return cmp(rhs) <= 0; }
    bool operator>=(const Weight& rhs) const { return cmp(rhs) >= 0; }
    bool operator==(const Weight& rhs) const {
        return std::memcmp(w, rhs.w, sizeof(w)) == 0;
    }
    bool operator!=(const Weight& rhs) const { return !(*this == rhs); }
    bool is_zero() const {
        for (size_t i = 0; i < NWORDS; ++i)
            if (w[i]) return false;
        return true;
    }

    // -----------------------------------------------------------------------
    // Arithmetic (unsigned, no overflow check — 256 bit is enough)
    // -----------------------------------------------------------------------
    Weight& operator+=(const Weight& rhs) {
        uint64_t carry = 0;
        for (size_t i = 0; i < NWORDS; ++i) {
            uint64_t a = w[i];
            uint64_t b = rhs.w[i];
            uint64_t sum = a + carry;
            carry = (sum < a) ? 1 : 0;
            sum += b;
            if (sum < b) carry += 1;
            w[i] = sum;
        }
        return *this;
    }

    Weight operator+(const Weight& rhs) const {
        Weight r = *this;
        r += rhs;
        return r;
    }

    // Subtract rhs. Precondition: *this >= rhs (let it crash otherwise)
    Weight& operator-=(const Weight& rhs) {
        uint64_t borrow = 0;
        for (size_t i = 0; i < NWORDS; ++i) {
            __int128 a = static_cast<__int128>(w[i]);
            __int128 b = static_cast<__int128>(rhs.w[i]) + borrow;
            if (a < b) {
                a += static_cast<__int128>(1) << 64;
                borrow = 1;
            } else {
                borrow = 0;
            }
            w[i] = static_cast<uint64_t>(a - b);
        }
        return *this;
    }

    Weight operator-(const Weight& rhs) const {
        Weight r = *this;
        r -= rhs;
        return r;
    }

    // |*this - rhs|
    Weight abs_diff(const Weight& rhs) const {
        int c = cmp(rhs);
        if (c == 0) return Weight{};
        if (c > 0) return *this - rhs;
        return rhs - *this;
    }

    // -----------------------------------------------------------------------
    // Conversion
    // -----------------------------------------------------------------------
    explicit operator double() const {
        double result = 0.0;
        double base = 1.0;
        for (size_t i = 0; i < NWORDS; ++i) {
            result += static_cast<double>(w[i]) * base;
            base *= 18446744073709551616.0; // 2^64
        }
        return result;
    }

    explicit operator uint64_t() const { return w[0]; }

    // -----------------------------------------------------------------------
    // Access for CNF encoding and serialization
    // -----------------------------------------------------------------------
    const uint64_t* data() const { return w; }
    static constexpr size_t nwords() { return NWORDS; }

    /// Full-precision decimal string (base 10).  Never truncates.
    std::string to_dec_string() const {
        if (is_zero()) return "0";

        // Copy to temporary buffer for repeated divide-by-10
        uint64_t tmp[NWORDS];
        std::memcpy(tmp, w, sizeof(w));

        auto is_zero_buf = [](const uint64_t* buf) {
            for (size_t i = 0; i < NWORDS; ++i)
                if (buf[i]) return false;
            return true;
        };

        auto div10 = [](uint64_t* buf) -> uint64_t {
            uint64_t rem = 0;
            for (int i = static_cast<int>(NWORDS) - 1; i >= 0; --i) {
                __int128 val = (static_cast<__int128>(rem) << 64) | buf[i];
                buf[i] = static_cast<uint64_t>(val / 10);
                rem = static_cast<uint64_t>(val % 10);
            }
            return rem;
        };

        // Collect remainders (decimal digits in reverse order)
        std::string result;
        result.reserve(78); // 2^256 ≈ 1.16e77 → ≤ 78 decimal digits

        while (!is_zero_buf(tmp))
            result.push_back('0' + static_cast<char>(div10(tmp)));

        std::reverse(result.begin(), result.end());
        return result;
    }
};

// =============================================================================
// SignedWeight — sign-magnitude signed integer backed by Weight
// =============================================================================
class SignedWeight {
    bool neg = false;
    Weight mag;

  public:
    SignedWeight() = default;

    // Construct from int64_t (backward compatibility for small values)
    explicit SignedWeight(int64_t val) {
        if (val < 0) {
            neg = true;
            // This cast is safe: INT64_MIN = -2^63, magnitude = 2^63 fits in uint64_t
            uint64_t abs_val = (val == std::numeric_limits<int64_t>::min())
                                   ? static_cast<uint64_t>(1) << 63
                                   : static_cast<uint64_t>(-val);
            mag = Weight(abs_val);
        } else {
            neg = false;
            mag = Weight(static_cast<uint64_t>(val));
        }
    }

    static SignedWeight positive(const Weight& w) {
        SignedWeight s;
        s.neg = false;
        s.mag = w;
        return s;
    }

    static SignedWeight negative(const Weight& w) {
        SignedWeight s;
        s.neg = true;
        s.mag = w;
        return s;
    }

    static SignedWeight zero() { return SignedWeight{}; }

    // -----------------------------------------------------------------------
    // Queries
    // -----------------------------------------------------------------------
    bool is_zero() const { return mag.is_zero(); }
    bool is_positive() const { return !neg && !mag.is_zero(); }
    bool is_negative() const { return neg && !mag.is_zero(); }
    bool sign() const { return neg; }

    // -----------------------------------------------------------------------
    // Comparison
    // -----------------------------------------------------------------------
    int cmp(const SignedWeight& rhs) const {
        if (neg != rhs.neg)
            return neg ? -1 : 1; // negative < positive
        if (neg)
            return rhs.mag.cmp(mag); // both negative: larger magnitude = smaller
        return mag.cmp(rhs.mag);     // both positive
    }

    bool operator<(const SignedWeight& rhs) const { return cmp(rhs) < 0; }
    bool operator>(const SignedWeight& rhs) const { return cmp(rhs) > 0; }
    bool operator<=(const SignedWeight& rhs) const { return cmp(rhs) <= 0; }
    bool operator>=(const SignedWeight& rhs) const { return cmp(rhs) >= 0; }
    bool operator==(const SignedWeight& rhs) const {
        if (neg != rhs.neg) return false;
        if (mag.is_zero() && rhs.mag.is_zero()) return true; // -0 == +0
        return mag == rhs.mag;
    }
    bool operator!=(const SignedWeight& rhs) const { return !(*this == rhs); }

    // -----------------------------------------------------------------------
    // Arithmetic
    // -----------------------------------------------------------------------
    SignedWeight operator-() const {
        if (mag.is_zero()) return SignedWeight{};
        SignedWeight r = *this;
        r.neg = !neg;
        return r;
    }

    SignedWeight& operator+=(const SignedWeight& rhs) {
        if (neg == rhs.neg) {
            // same sign: magnitudes add
            mag += rhs.mag;
        } else {
            if (mag.is_zero()) {
                if (rhs.mag.is_zero()) {
                    neg = false;
                    return *this;
                }
                neg = rhs.neg;
                mag = rhs.mag;
                return *this;
            }
            if (rhs.mag.is_zero())
                return *this;

            // different signs: subtract smaller magnitude from larger
            int c = mag.cmp(rhs.mag);
            if (c == 0) {
                mag = Weight{};
                neg = false;
            } else if (c > 0) {
                mag -= rhs.mag;
                // sign unchanged (this->neg)
            } else {
                Weight tmp = rhs.mag;
                tmp -= mag;
                mag = tmp;
                neg = rhs.neg;
            }
        }
        return *this;
    }

    SignedWeight operator+(const SignedWeight& rhs) const {
        SignedWeight r = *this;
        r += rhs;
        return r;
    }

    SignedWeight& operator-=(const SignedWeight& rhs) {
        return *this += (-rhs);
    }

    SignedWeight operator-(const SignedWeight& rhs) const {
        SignedWeight r = *this;
        r -= rhs;
        return r;
    }

    // -----------------------------------------------------------------------
    // Magnitude / Conversion
    // -----------------------------------------------------------------------
    Weight abs() const { return mag; }

    explicit operator double() const {
        double d = static_cast<double>(mag);
        return neg ? -d : d;
    }

    // -----------------------------------------------------------------------
    // Serialization helpers
    // -----------------------------------------------------------------------
    const Weight& magnitude() const { return mag; }
    bool negative() const { return neg; }

    /// Full-precision signed decimal string.  Never truncates.
    std::string to_dec_string() const {
        if (mag.is_zero()) return "0";
        return neg ? ("-" + mag.to_dec_string()) : mag.to_dec_string();
    }
};

// =============================================================================
// WeightedInterval — replaces the old Interval (int64_t-based)
// =============================================================================
struct WeightedInterval {
    SignedWeight l;
    SignedWeight u;

    WeightedInterval() = default;
    WeightedInterval(SignedWeight lower, SignedWeight upper) : l(lower), u(upper) {}

    // max(|l|, |u|)
    Weight abs_bound() const {
        Weight al = l.abs();
        Weight au = u.abs();
        return al > au ? al : au;
    }

    // u - l as unsigned Weight
    Weight width_bound() const {
        if (l.is_negative() && u.is_negative()) {
            // both negative: u - l = |l| - |u| (since |l| >= |u| for negative range)
            return l.abs().abs_diff(u.abs());
        }
        if (l.is_negative()) {
            // l negative, u non-negative
            return l.abs() + u.abs();
        }
        // both non-negative
        return u.abs() - l.abs();
    }

    bool is_valid() const { return l.cmp(u) <= 0; }
};

// =============================================================================
// Free-standing helpers (replace old signed_output_weight etc.)
// =============================================================================

// Default per-output weight: 2^i
inline SignedWeight signed_output_weight(size_t output_index) {
    return SignedWeight::positive(Weight::from_bit(output_index));
}

// Maximum possible |signed error| for output_count outputs with 2^i weights.
// This is 2^{output_count} - 1, which is the sum of all weights.
inline SignedWeight signed_error_cap(size_t output_count) {
    if (output_count == 0)
        return SignedWeight::zero();
    // sum_{i=0}^{output_count-1} 2^i = 2^{output_count} - 1
    Weight sum;
    for (size_t i = 0; i < output_count; ++i)
        sum += Weight::from_bit(i);
    return SignedWeight::positive(sum);
}

// -----------------------------------------------------------------------
// WeightedInterval helpers
// -----------------------------------------------------------------------
inline Weight interval_abs_bound(const WeightedInterval& iv) {
    return iv.abs_bound();
}

inline Weight interval_width_bound(const WeightedInterval& iv) {
    return iv.width_bound();
}

inline WeightedInterval intersect_weighted_interval(const WeightedInterval& lhs,
                                                    const WeightedInterval& rhs) {
    return WeightedInterval(
        lhs.l.cmp(rhs.l) > 0 ? lhs.l : rhs.l, // max(lower)
        lhs.u.cmp(rhs.u) < 0 ? lhs.u : rhs.u  // min(upper)
    );
}

inline void expand_weighted_interval(WeightedInterval& target, const WeightedInterval& update) {
    if (update.l.cmp(target.l) < 0)
        target.l = update.l;
    if (update.u.cmp(target.u) > 0)
        target.u = update.u;
}

// An "open" interval (-∞, +∞). We use sentinel values instead of infinity.
inline WeightedInterval open_weighted_interval(size_t output_count) {
    SignedWeight cap = signed_error_cap(output_count);
    return WeightedInterval(-cap, cap);
}

// =============================================================================
// ForcedValueIntervals — SA0 / SA1 pair of WeightedInterval
// =============================================================================
struct ForcedValueIntervals {
    WeightedInterval sa0;
    WeightedInterval sa1;
};

inline ForcedValueIntervals open_forced_value_intervals(size_t output_count) {
    return ForcedValueIntervals{open_weighted_interval(output_count),
                                open_weighted_interval(output_count)};
}

// =============================================================================
// Stream operators — full-precision decimal output
// =============================================================================
inline std::ostream& operator<<(std::ostream& os, const Weight& v) {
    return os << v.to_dec_string();
}

inline std::ostream& operator<<(std::ostream& os, const SignedWeight& v) {
    return os << v.to_dec_string();
}

class BitPattern {
  private:
    std::vector<uint64_t> words;
    size_t n_bits;

  public:
    explicit BitPattern(size_t size = 0) : n_bits(size) { words.resize((size + 63) / 64, 0); }

    size_t size() const { return n_bits; }

    BitPattern& operator|=(const BitPattern& other) {
        for (size_t i = 0; i < words.size(); ++i)
            words[i] |= other.words[i];
        return *this;
    }

    BitPattern& operator&=(const BitPattern& other) {
        for (size_t i = 0; i < words.size(); ++i)
            words[i] &= other.words[i];
        return *this;
    }

    bool is_zero() const {
        for (auto w : words)
            if (w) return false;
        return true;
    }

    size_t popcount() const {
        size_t total = 0;
        for (auto w : words)
            total += static_cast<size_t>(__builtin_popcountll(w));
        return total;
    }

    // Iterate over set bits: call `fn(bit_index)` for each 1-bit
    template <typename F>
    void for_each_bit(F&& fn) const {
        for (size_t wi = 0; wi < words.size(); ++wi) {
            uint64_t w = words[wi];
            while (w) {
                int bit = __builtin_ctzll(w);
                fn(wi * 64 + static_cast<size_t>(bit));
                w &= w - 1;
            }
        }
    }

    // Construct with a single bit set (for fwd_po_mask initialization)
    static BitPattern from_bit(size_t bit_index, size_t total_bits) {
        BitPattern bp(total_bits);
        bp.words[bit_index / 64] |= uint64_t{1} << (bit_index % 64);
        return bp;
    }

};

#include <string>
#include <vector>

namespace als {

enum class NodeType { CONST0, CONST1, INPUT, AND, OUTPUT };

struct Node {
    int id = 0;
    NodeType type = NodeType::CONST0;
    int fanin0 = 0;
    int fanin1 = 0;
    std::vector<int> fanouts;
    int level = -1;
};

constexpr int lit_to_var(int literal) { return literal >> 1; }
constexpr bool lit_is_inv(int literal) { return (literal & 1) != 0; }

class AigNetlist {
  public:
    int max_var = 0;
    std::vector<Node> nodes;
    std::vector<int> inputs;
    std::vector<int> outputs;
    // Optional AIGER symbols aligned with inputs/outputs. Empty entries mean
    // that the source file did not provide a symbol for that port.
    std::vector<std::string> input_names;
    std::vector<std::string> output_names;

    // Lazily-computed topology caches. They describe the circuit only; analyzer
    // results and pruning state deliberately live outside AigNetlist.
    std::vector<BitPattern> forward_output_masks;
    bool forward_output_reachability_valid = false;

    void compute_forward_output_reachability();
    void clear_topology_caches();
    void rebuild_fanouts();

};

int count_and_gates(const AigNetlist& net);

} // namespace als

namespace als {

AigNetlist load_aiger(const std::string& path);
void write_aiger(const AigNetlist& net, const std::string& path);

} // namespace als
