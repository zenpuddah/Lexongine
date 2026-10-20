#pragma once

#include "lexongine/model.hpp"
#include "quant/data/validation.hpp"

namespace lexongine::fixture {

// Versioned synthetic generator facts, not downloaded or persisted market bars.
inline constexpr std::uint64_t bar_count = 120;
inline constexpr std::int64_t interval_ns = 60000000000LL;
inline constexpr std::string_view snapshot_name = "fixture-bars-v1";
inline constexpr std::string_view clock_name = "bar-clock-v1";

[[nodiscard]] inline Result<quant::data::Bar> complete_bar(std::uint64_t index) noexcept {
    if (index >= bar_count) {
        return diagnostic(FailureCode::SourceCoverage, "fixture key outside [0,120)");
    }
    using namespace quant::data;
    const auto begin = static_cast<std::int64_t>(index) * interval_ns;
    const auto end = begin + interval_ns;
    const auto price =
        Price::from_raw(100000000000LL + 10000000LL * static_cast<std::int64_t>(index));
    return Bar{{InstrumentId{1}, VenueId{1}, Timestamp::from_unix_nanos(end), std::nullopt, index,
                std::nullopt, 0, SourceId{1}},
               Timestamp::from_unix_nanos(begin),
               Timestamp::from_unix_nanos(end),
               price,
               price,
               price,
               price,
               Quantity::from_raw(1000000)};
}

} // namespace lexongine::fixture
