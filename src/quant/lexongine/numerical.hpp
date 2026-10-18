#pragma once

#include "lexongine/model.hpp"
#include "quant/data/value_types.hpp"

namespace quant::lexongine {

enum class SumWidth { Signed64 };
enum class Rounding { NearestTiesEven };
enum class Overflow { Fail };

// This helper deliberately also accepts sums outside the bounded SMA domain.
// Unsigned magnitude avoids negating INT64_MIN and supports a uint64 divisor.
[[nodiscard]] inline ::lexongine::Result<std::int64_t> round_ties_even(std::int64_t sum, std::uint64_t count) noexcept {
    if (count == 0) { return ::lexongine::diagnostic(::lexongine::FailureCode::InvalidConfig, "rounding divisor must be positive"); }
    const bool negative = sum < 0;
    const auto magnitude = negative ? std::uint64_t{0} - static_cast<std::uint64_t>(sum) : static_cast<std::uint64_t>(sum);
    auto quotient = magnitude / count;
    const auto remainder = magnitude % count;
    const auto distance_up = count - remainder;
    if (remainder > distance_up || (remainder == distance_up && (quotient & 1U) != 0)) { ++quotient; }
    if (negative && quotient == (std::uint64_t{1} << 63)) { return std::numeric_limits<std::int64_t>::min(); }
    const auto result = static_cast<std::int64_t>(quotient);
    return negative ? -result : result;
}

} // namespace quant::lexongine
