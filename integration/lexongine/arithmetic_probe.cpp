// Test-only numerical transport for the independent Fraction oracle. This is not
// the product CLI, a graph executor, source provider or financial backtester.
#include "quant/lexongine/sma.hpp"
#include "quant/lexongine/lag_price.hpp"

#include <iostream>

namespace {
using namespace lexongine;
namespace numeric = quant::lexongine;
template<class T> T take(std::variant<T, Diagnostic> result) {
    if (const auto* error = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(std::string{error->text()}); }
    return std::get<T>(std::move(result));
}
}
int main(int argc, char** argv) {
    try {
        if (argc < 2) { throw std::invalid_argument("expected explicit probe mode"); }
        const std::string_view mode{argv[1]};
        std::uint64_t index = 0;
        if (mode == "round") {
            if (argc != 2) { throw std::invalid_argument("round mode takes sum/divisor pairs on stdin"); }
            std::string sum_token, count_token;
            while (std::cin >> sum_token) {
                if (!(std::cin >> count_token)) { throw std::invalid_argument("incomplete sum/divisor pair"); }
                const auto sum = take(parse_integer<std::int64_t>(sum_token));
                const auto count = take(parse_integer<std::uint64_t>(count_token));
                if (index == 4096) { throw std::invalid_argument("test probe input limit exceeded"); }
                std::cout << index++ << ' ' << take(numeric::round_ties_even(sum, count)) << '\n';
            }
        } else {
            if ((mode != "sma" && mode != "lag") || (mode == "sma" && argc != 4) || (mode == "lag" && argc != 3)) {
                throw std::invalid_argument("expected sma N B or lag D");
            }
            const auto count = take(parse_integer<std::uint64_t>(argv[2]));
            auto resources = take(ResourceAccount::create({{1048576, 1048576, 1048576, 1048576, 1048576, 1048576, 1048576}, 64, 64, 128, 128}));
            StateAllocator allocator{resources};
            auto process = [&](auto& state) {
                std::int64_t raw;
                while (std::cin >> raw) {
                    if (index == 4096) { throw std::invalid_argument("test probe input limit exceeded"); }
                    const auto result = take(state.advance(quant::data::Price::from_raw(raw)));
                    std::cout << index++ << ' ';
                    if (result) { std::cout << "published " << result->raw(); }
                    else { std::cout << "warming"; }
                    std::cout << '\n';
                }
            };
            if (mode == "sma") {
                const auto bound = take(parse_integer<std::int64_t>(argv[3]));
                auto state = take(numeric::SmaWindow::make({count, bound, numeric::SumWidth::Signed64,
                    numeric::Rounding::NearestTiesEven, numeric::Overflow::Fail}, allocator));
                process(state);
            } else {
                auto state = take(numeric::LagWindow::make({count, CountBasis::CompleteBarUpdates}, allocator));
                process(state);
            }
        }
        if (!std::cin.eof() || index == 0 || !std::cout) { throw std::invalid_argument("incomplete probe transport"); }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "arithmetic_probe: " << error.what() << '\n';
        return 1;
    }
}
