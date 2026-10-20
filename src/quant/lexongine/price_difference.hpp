#pragma once

#include "quant/lexongine/numerical.hpp"

namespace quant::lexongine {
[[nodiscard]] inline ::lexongine::Result<data::Price>
price_difference(data::Price left, data::Price right, Overflow overflow) noexcept {
    return ::lexongine::guarded([&]() -> ::lexongine::Result<data::Price> {
        if (overflow != Overflow::Fail) {
            return ::lexongine::diagnostic(::lexongine::FailureCode::UnsupportedContract,
                                           "unsupported difference overflow policy");
        }
        try {
            return left - right;
        } catch (const std::overflow_error &) {
            return ::lexongine::diagnostic(::lexongine::FailureCode::NumericOverflow,
                                           "Price subtraction overflow");
        }
    });
}
} // namespace quant::lexongine
