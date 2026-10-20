#pragma once

#include "lexongine/registry.hpp"
#include "quant/data/value_types.hpp"

namespace quant::lexongine {

struct LagParameters {
    std::uint64_t lag_steps;
    ::lexongine::CountBasis count_basis;
};

// Pure value history. Gap policy, key/timing and selected-parent metadata belong
// to the registered typed envelope and the host timeline, not a hidden clock here.
class LagWindow {
  public:
    [[nodiscard]] static ::lexongine::Result<std::uint64_t>
    state_bytes(const LagParameters &parameters) noexcept {
        if (parameters.lag_steps == 0) {
            return ::lexongine::diagnostic(::lexongine::FailureCode::InvalidConfig,
                                           "lag_steps must be positive");
        }
        if (parameters.count_basis != ::lexongine::CountBasis::CompleteBarUpdates) {
            return ::lexongine::diagnostic(::lexongine::FailureCode::UnsupportedContract,
                                           "lag count basis is unsupported");
        }
        const auto bytes =
            ::lexongine::checked_multiply(parameters.lag_steps, sizeof(std::int64_t));
        if (const auto *error = std::get_if<::lexongine::Diagnostic>(&bytes)) {
            return *error;
        }
        if (parameters.lag_steps > std::vector<std::int64_t>{}.max_size()) {
            return ::lexongine::diagnostic(::lexongine::FailureCode::ResourcePlanExceeded,
                                           "lag ring exceeds vector representation");
        }
        return bytes;
    }
    [[nodiscard]] static ::lexongine::Result<LagWindow>
    make(LagParameters parameters, ::lexongine::StateAllocator &allocator) noexcept {
        return ::lexongine::guarded([&]() -> ::lexongine::Result<LagWindow> {
            auto bytes = state_bytes(parameters);
            if (const auto *error = std::get_if<::lexongine::Diagnostic>(&bytes)) {
                return *error;
            }
            auto charge = allocator.reserve(std::get<std::uint64_t>(bytes));
            if (const auto *error = std::get_if<::lexongine::Diagnostic>(&charge)) {
                return *error;
            }
            return LagWindow{
                std::get<::lexongine::Reservation>(std::move(charge)),
                std::vector<std::int64_t>(static_cast<std::size_t>(parameters.lag_steps))};
        });
    }
    [[nodiscard]] ::lexongine::Result<std::optional<data::Price>>
    advance(data::Price input) noexcept {
        if (ring_.empty()) {
            return ::lexongine::diagnostic(::lexongine::FailureCode::InvalidConfig,
                                           "moved-from lag state");
        }
        if (advances_ == std::numeric_limits<std::uint64_t>::max()) {
            return ::lexongine::diagnostic(::lexongine::FailureCode::UpdateProtocol,
                                           "lag advance counter exhausted");
        }
        std::optional<data::Price> result;
        if (count_ == ring_.size()) {
            result = data::Price::from_raw(ring_[head_]);
        }
        ring_[head_] = input.raw();
        head_ = (head_ + 1) % ring_.size();
        if (count_ < ring_.size()) {
            ++count_;
        }
        ++advances_;
        return result;
    }
    [[nodiscard]] std::uint64_t advances() const noexcept {
        return advances_;
    }

  private:
    LagWindow(::lexongine::Reservation charge, std::vector<std::int64_t> ring)
        : charge_(std::move(charge)), ring_(std::move(ring)) {}
    ::lexongine::Reservation charge_;
    std::vector<std::int64_t> ring_;
    std::size_t head_ = 0, count_ = 0;
    std::uint64_t advances_ = 0;
};

} // namespace quant::lexongine
