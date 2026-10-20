#pragma once

#include "lexongine/configuration.hpp"

namespace lexongine::cli {
enum class Command { List, Describe, Validate, Plan, Run };
struct Options {
    Reservation charge;
    Command command;
    std::string config, operation, implementation, plan, before_dot, after_dot, report;
    std::uint32_t version = 0;
    CompositionOverrides overrides;
};
// Versioned tool bounds, separate from numerical/source policies.
inline constexpr std::uint64_t max_arguments = 256, max_argument_bytes = 1024;
[[nodiscard]] Result<Options> parse_options(std::span<const std::string_view>,
                                            std::shared_ptr<ResourceAccount>) noexcept;
} // namespace lexongine::cli
