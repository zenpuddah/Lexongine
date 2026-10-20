#include "lexongine_test_support.hpp"
#include "quant/lexongine/sma.hpp"
#include "quant/lexongine/lag_price.hpp"
#include "quant/lexongine/price_difference.hpp"
#include "quant/lexongine/bar_close.hpp"
#include "../integration/lexongine/fixture.hpp"

#include <limits>

namespace {
using namespace lexongine;
using namespace lexongine::test;
namespace numeric = quant::lexongine;
using Price = quant::data::Price;
std::shared_ptr<ResourceAccount> account(std::uint64_t state_bytes = 65536) {
    return take(ResourceAccount::create(
        {{65536, state_bytes, 65536, 65536, 65536, 65536, 65536}, 64, 64, 256, 128}));
}
numeric::SmaParameters parameters(std::uint64_t count, std::int64_t bound) {
    return {count, bound, numeric::SumWidth::Signed64, numeric::Rounding::NearestTiesEven,
            numeric::Overflow::Fail};
}
void ties_and_extremes() {
    for (const auto &[sum, expected] : std::array<std::pair<std::int64_t, std::int64_t>, 6>{
             {{1, 0}, {3, 2}, {-1, 0}, {-3, -2}, {5, 2}, {-5, -2}}}) {
        require(take(numeric::round_ties_even(sum, 2)) == expected, "signed half tie mismatch");
    }
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    require(take(numeric::round_ties_even(minimum, 1)) == minimum, "signed minimum exact division");
    require(take(numeric::round_ties_even(minimum, 3)) == -3074457345618258603LL,
            "signed minimum rounding");
    require(take(numeric::round_ties_even(maximum, 2)) == 4611686018427387904LL,
            "large positive tie");
    require(take(numeric::round_ties_even(minimum, std::numeric_limits<std::uint64_t>::max())) ==
                -1,
            "large unsigned denominator");
    rejects(numeric::round_ties_even(1, 0), FailureCode::InvalidConfig);
}
void parameters_and_capacity() {
    auto resources = account(64);
    StateAllocator allocator{resources};
    rejects(numeric::SmaWindow::make(parameters(0, 1), allocator), FailureCode::InvalidConfig);
    rejects(numeric::SmaWindow::make(parameters(2, -1), allocator), FailureCode::InvalidConfig);
    rejects(numeric::SmaWindow::make(parameters(2, std::numeric_limits<std::int64_t>::max()),
                                     allocator),
            FailureCode::InvalidConfig);
    rejects(numeric::SmaWindow::make(
                parameters(static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()), 1),
                allocator),
            FailureCode::ResourcePlanExceeded);
    rejects(numeric::SmaWindow::make(parameters(100, 1), allocator), FailureCode::ResourceExceeded);
    auto wrong = parameters(2, 1);
    wrong.rounding = static_cast<numeric::Rounding>(99);
    rejects(numeric::SmaWindow::make(wrong, allocator), FailureCode::UnsupportedContract);
    require(resources->current(ResourceKind::State) == 0,
            "failed setup retained a state allocation");
}
void exact_state_and_equal_updates() {
    StateAllocator allocator{account()};
    auto state = take(numeric::SmaWindow::make(parameters(3, 8), allocator));
    require(!take(state.advance(Price::from_raw(2))), "SMA published before enough inputs");
    require(!take(state.advance(Price::from_raw(2))), "equal-valued update lost readiness");
    require(take(state.advance(Price::from_raw(2)))->raw() == 2,
            "third equal-valued update was erased");
    require(take(state.advance(Price::from_raw(8)))->raw() == 4,
            "rounded output fed state instead of exact inputs");
    require(state.advances() == 4 && state.count() == 3 && state.sum_raw() == 12,
            "state counters or exact sum mismatch");
    rejects(state.advance(Price::from_raw(9)), FailureCode::NumericBound);
    rejects(state.advance(Price::from_raw(std::numeric_limits<std::int64_t>::min())),
            FailureCode::NumericBound);
    require(state.advances() == 4 && state.sum_raw() == 12, "invalid input changed state");
    auto one = take(numeric::SmaWindow::make(parameters(1, 9), allocator));
    require(take(one.advance(Price::from_raw(-7)))->raw() == -7, "window one must be exact");
}
void subtract_before_add() {
    StateAllocator allocator{account()};
    constexpr auto bound = std::numeric_limits<std::int64_t>::max() / 2;
    auto state = take(numeric::SmaWindow::make(parameters(2, bound), allocator));
    require(!take(state.advance(Price::from_raw(bound))), "window two published first input");
    require(take(state.advance(Price::from_raw(bound)))->raw() == bound,
            "full legal window failed");
    require(take(state.advance(Price::from_raw(bound)))->raw() == bound,
            "append-first transient overflow");
    rejects(numeric::price_difference(Price::from_raw(std::numeric_limits<std::int64_t>::max()),
                                      Price::from_raw(-1), numeric::Overflow::Fail),
            FailureCode::NumericOverflow);
    rejects(numeric::price_difference(Price::from_raw(std::numeric_limits<std::int64_t>::min()),
                                      Price::from_raw(1), numeric::Overflow::Fail),
            FailureCode::NumericOverflow);
}
void lag_values() {
    StateAllocator allocator{account()};
    auto lag = take(numeric::LagWindow::make({2, CountBasis::CompleteBarUpdates}, allocator));
    require(!take(lag.advance(Price::from_raw(1))), "lag emitted without history");
    require(!take(lag.advance(Price::from_raw(1))), "equal lag input was ignored");
    require(take(lag.advance(Price::from_raw(3)))->raw() == 1, "lag selected wrong prior input");
    require(take(lag.advance(Price::from_raw(5)))->raw() == 1 && lag.advances() == 4,
            "lag compressed repeated values");
    rejects(numeric::LagWindow::make({0, CountBasis::CompleteBarUpdates}, allocator),
            FailureCode::InvalidConfig);
}
void canonical_fixture_arithmetic() {
    StateAllocator allocator{account()};
    auto fast = take(numeric::SmaWindow::make(parameters(20, 102000000000LL), allocator));
    auto slow = take(numeric::SmaWindow::make(parameters(50, 102000000000LL), allocator));
    auto lag = take(numeric::LagWindow::make({1, CountBasis::CompleteBarUpdates}, allocator));
    std::size_t fast_count = 0, slow_count = 0, lag_count = 0;
    for (std::uint64_t key = 0; key < 120; ++key) {
        const auto bar = take(lexongine::fixture::complete_bar(key));
        require(quant::data::validate(bar).empty(), "fixture violates canonical Bar validation");
        require(bar.header.instrument_id.value() == 1 && bar.header.venue_id.value() == 1 &&
                    bar.header.source_id.value() == 1,
                "fixture stream identity mismatch");
        require(!bar.header.source_receive_time && !bar.header.channel_id &&
                    bar.header.source_flags == 0 && bar.header.sequence == key,
                "fixture optional source fields mismatch");
        require(bar.header.event_time == bar.interval_end && bar.volume.raw() == 1000000,
                "fixture time or volume mismatch");
        const auto close = numeric::bar_close(bar);
        auto a = take(fast.advance(close));
        auto b = take(slow.advance(close));
        if (a) {
            ++fast_count;
        }
        if (!b) {
            continue;
        }
        ++slow_count;
        require(a && take(numeric::price_difference(*a, *b, numeric::Overflow::Fail)).raw() ==
                         150000000,
                "fixture spread mismatch");
        if (key == 49) {
            require(a->raw() == 100395000000LL && b->raw() == 100245000000LL,
                    "first evaluated means mismatch");
        }
        if (key == 119) {
            require(a->raw() == 101095000000LL && b->raw() == 100945000000LL,
                    "last evaluated means mismatch");
        }
        const auto prior = take(lag.advance(*b));
        if (prior) {
            ++lag_count;
            require(take(numeric::price_difference(*a, *prior, numeric::Overflow::Fail)).raw() ==
                        160000000,
                    "lagged fixture spread mismatch");
        }
    }
    require(fast_count == 101 && slow_count == 71 && lag_count == 70,
            "fixture warmup or output counts mismatch");
    rejects(lexongine::fixture::complete_bar(120), FailureCode::SourceCoverage);
}
} // namespace
int main() {
    const Case cases[]{{"signed rounding and extreme bounds", ties_and_extremes},
                       {"explicit parameters and allocation admission", parameters_and_capacity},
                       {"exact state and equal-valued updates", exact_state_and_equal_updates},
                       {"legal transient and subtraction overflow", subtract_before_add},
                       {"fixed-count lag values", lag_values},
                       {"canonical fixture arithmetic", canonical_fixture_arithmetic}};
    return lexongine::test::run(cases);
}
