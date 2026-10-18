#pragma once

#include "quant/data/observations.hpp"

namespace quant::lexongine {
// The source/typed host contract must establish validity, completeness and current
// availability before invoking this numerical adapter. A Bar alone is no certificate.
[[nodiscard]] inline data::Price bar_close(const data::Bar& bar) noexcept { return bar.close; }
} // namespace quant::lexongine
