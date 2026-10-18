#pragma once

#include "lexongine/registry.hpp"
#include "quant/data/value_types.hpp"

namespace lexongine {
struct PriceOutcomeRecord {
    UpdateKey key;
    TemporalExtent extent;
    std::int64_t available_at_ns;
    OutcomeKind kind;
    std::uint64_t remaining_preceding;
    std::uint32_t declared_reason;
    std::vector<quant::data::Price> values;
    std::vector<ParentReference> parents;
};
// Borrowed only during encoding/decoding. Source/identity/origin validation remains
// the caller's responsibility; decoding is not authority to publish cached values.
struct PricePayloadContract {
    SequenceScope scope;
    std::span<const OutputContract> outputs;
    std::span<const UpdateKey> complete_keys;
};
struct PricePayloadLimits {
    std::uint64_t xml_bytes, xml_elements, string_bytes, outcomes, parents_per_outcome, decoded_bytes, scratch_bytes;
};
class DecodedPricePayload {
public:
    [[nodiscard]] const SequenceScope& scope() const noexcept;
    [[nodiscard]] std::span<const PriceOutcomeRecord> outcomes() const noexcept;
private:
    struct Data;
    explicit DecodedPricePayload(std::shared_ptr<const Data> data) noexcept : data_(std::move(data)) {}
    friend Result<DecodedPricePayload> decode_price_payload(std::string_view, const PricePayloadContract&,
        std::shared_ptr<ResourceAccount>, const PricePayloadLimits&) noexcept;
    std::shared_ptr<const Data> data_;
};
class EncodedPricePayload {
public:
    [[nodiscard]] std::string_view bytes() const noexcept { return bytes_; }
private:
    EncodedPricePayload(Reservation charge, std::string bytes) noexcept : charge_(std::move(charge)), bytes_(std::move(bytes)) {}
    friend Result<EncodedPricePayload> encode_price_payload(std::span<const PriceOutcomeRecord>, const PricePayloadContract&,
        std::shared_ptr<ResourceAccount>, const PricePayloadLimits&) noexcept;
    Reservation charge_;
    std::string bytes_;
};
[[nodiscard]] Result<DecodedPricePayload> decode_price_payload(std::string_view, const PricePayloadContract&,
    std::shared_ptr<ResourceAccount>, const PricePayloadLimits&) noexcept;
[[nodiscard]] Result<EncodedPricePayload> encode_price_payload(std::span<const PriceOutcomeRecord>, const PricePayloadContract&,
    std::shared_ptr<ResourceAccount>, const PricePayloadLimits&) noexcept;
// Counts a validated document without allocating encoded bytes. The result may
// exceed xml_bytes so a store can select configured capacity bypass before I/O.
[[nodiscard]] Result<std::uint64_t> measure_price_payload(std::span<const PriceOutcomeRecord>, const PricePayloadContract&,
    std::shared_ptr<ResourceAccount>, const PricePayloadLimits&) noexcept;
} // namespace lexongine
