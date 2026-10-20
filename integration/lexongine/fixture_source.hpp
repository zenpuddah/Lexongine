#pragma once

#include "lexongine/configuration.hpp"
#include "lexongine/source.hpp"
#include "quant/lexongine/components.hpp"

namespace lexongine::fixture {
struct SourceLimits {
    std::uint64_t max_records, decoded_bytes, metadata_bytes, fingerprint_bytes;
};
[[nodiscard]] Result<SourceRequest> source_request(const SourceDeclaration &) noexcept;
[[nodiscard]] Result<void> register_source(Registry &, SourceResolver &,
                                           RegisteredType<quant::data::Bar>,
                                           const quant::lexongine::ComponentRegistration &,
                                           SourceLimits) noexcept;
} // namespace lexongine::fixture
