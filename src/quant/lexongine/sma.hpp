#pragma once

#include "lexongine/registry.hpp"
#include "quant/lexongine/numerical.hpp"

namespace quant::lexongine {

struct SmaParameters {
    std::uint64_t window;
    std::int64_t max_abs_input_raw;
    SumWidth sum_width;
    Rounding rounding;
    Overflow overflow;
};

[[nodiscard]] inline ::lexongine::Result<void> validate(const SmaParameters& parameters) noexcept {
    if (parameters.sum_width != SumWidth::Signed64 || parameters.rounding != Rounding::NearestTiesEven ||
        parameters.overflow != Overflow::Fail) {
        return ::lexongine::diagnostic(::lexongine::FailureCode::UnsupportedContract, "unsupported SMA numerical policy");
    }
    if (parameters.window == 0 || parameters.max_abs_input_raw < 0 ||
        static_cast<std::uint64_t>(parameters.max_abs_input_raw) >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / parameters.window) {
        return ::lexongine::diagnostic(::lexongine::FailureCode::InvalidConfig, "SMA requires N>0, B>=0 and B<=INT64_MAX/N");
    }
    return ::lexongine::ok();
}

// Mathematical working state only. The registered component/host layer owns
// publication provenance, source validation, original keys and gap scheduling.
class SmaWindow {
public:
    [[nodiscard]] static ::lexongine::Result<std::uint64_t> state_bytes(const SmaParameters& parameters) noexcept {
        if (auto result = validate(parameters); std::holds_alternative<::lexongine::Diagnostic>(result)) {
            return std::get<::lexongine::Diagnostic>(result);
        }
        const auto bytes = ::lexongine::checked_multiply(parameters.window, sizeof(std::int64_t));
        if (const auto* error = std::get_if<::lexongine::Diagnostic>(&bytes)) { return *error; }
        if (parameters.window > std::vector<std::int64_t>{}.max_size()) {
            return ::lexongine::diagnostic(::lexongine::FailureCode::ResourcePlanExceeded, "SMA ring exceeds vector representation");
        }
        return bytes;
    }
    [[nodiscard]] static ::lexongine::Result<SmaWindow> make(SmaParameters parameters, ::lexongine::StateAllocator& allocator) noexcept {
        return ::lexongine::guarded([&]() -> ::lexongine::Result<SmaWindow> {
            auto bytes = state_bytes(parameters);
            if (const auto* error = std::get_if<::lexongine::Diagnostic>(&bytes)) { return *error; }
            auto reservation = allocator.reserve(std::get<std::uint64_t>(bytes));
            if (const auto* error = std::get_if<::lexongine::Diagnostic>(&reservation)) { return *error; }
            return SmaWindow{parameters, std::get<::lexongine::Reservation>(std::move(reservation)),
                             std::vector<std::int64_t>(static_cast<std::size_t>(parameters.window))};
        });
    }
    [[nodiscard]] ::lexongine::Result<std::optional<data::Price>> advance(data::Price input) noexcept {
        return ::lexongine::guarded([&]() -> ::lexongine::Result<std::optional<data::Price>> {
            if (ring_.empty()) { return ::lexongine::diagnostic(::lexongine::FailureCode::InvalidConfig, "moved-from SMA state"); }
            const auto value = input.raw();
            if (value < -parameters_.max_abs_input_raw || value > parameters_.max_abs_input_raw) {
                return ::lexongine::diagnostic(::lexongine::FailureCode::NumericBound, "SMA input exceeds declared B");
            }
            if (advances_ == std::numeric_limits<std::uint64_t>::max()) {
                return ::lexongine::diagnostic(::lexongine::FailureCode::UpdateProtocol, "SMA advance counter exhausted");
            }
            std::int64_t next_sum;
            try {
                auto candidate = data::Price::from_raw(sum_);
                if (count_ == parameters_.window) { candidate = candidate - data::Price::from_raw(ring_[head_]); }
                next_sum = (candidate + input).raw(); // Evict first: a legal full window must not transiently overflow.
            } catch (const std::overflow_error&) {
                return ::lexongine::diagnostic(::lexongine::FailureCode::NumericOverflow, "SMA checked sum overflow");
            }
            ring_[head_] = value;
            head_ = (head_ + 1) % ring_.size();
            sum_ = next_sum;
            if (count_ < parameters_.window) { ++count_; }
            ++advances_;
            if (count_ != parameters_.window) { return std::optional<data::Price>{}; }
            auto rounded = round_ties_even(sum_, parameters_.window);
            if (const auto* error = std::get_if<::lexongine::Diagnostic>(&rounded)) { return *error; }
            return std::optional<data::Price>{data::Price::from_raw(std::get<std::int64_t>(rounded))};
        });
    }
    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    [[nodiscard]] std::uint64_t advances() const noexcept { return advances_; }
    [[nodiscard]] std::int64_t sum_raw() const noexcept { return sum_; }
private:
    SmaWindow(SmaParameters parameters, ::lexongine::Reservation charge, std::vector<std::int64_t> ring)
        : parameters_(parameters), charge_(std::move(charge)), ring_(std::move(ring)) {}
    SmaParameters parameters_;
    ::lexongine::Reservation charge_;
    std::vector<std::int64_t> ring_;
    std::size_t head_ = 0;
    std::uint64_t count_ = 0, advances_ = 0;
    std::int64_t sum_ = 0;
};

} // namespace quant::lexongine
