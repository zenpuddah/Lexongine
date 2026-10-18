#pragma once

#include "lexongine/binding.hpp"
#include "lexongine/graph.hpp"

namespace lexongine {
class PreparedPlan;
struct IdentityLimits { std::uint64_t canonical_bytes, string_bytes, metadata_bytes, digest_scratch_bytes; };
class ArtifactIdentity {
public:
    [[nodiscard]] static Result<ArtifactIdentity> create(const PreparedPlan&, std::uint32_t logical,
        const Contract& codec, const Contract& validation, DigestFunction, std::shared_ptr<ResourceAccount>, const IdentityLimits&) noexcept;
    [[nodiscard]] std::span<const std::byte> canonical_bytes() const noexcept;
    [[nodiscard]] const Sha256& digest() const noexcept;
    [[nodiscard]] const SequenceScope& processing_scope() const noexcept;
    [[nodiscard]] std::span<const TypeContract> output_types() const noexcept;
    [[nodiscard]] const Contract& codec() const noexcept;
    [[nodiscard]] const Contract& validation() const noexcept;
    [[nodiscard]] bool matches(const PreparedPlan&, std::uint32_t logical) const noexcept;
    friend bool operator==(const ArtifactIdentity&, const ArtifactIdentity&) noexcept;
private:
    struct Data;
    explicit ArtifactIdentity(std::shared_ptr<const Data> data) noexcept : data_(std::move(data)) {}
    std::shared_ptr<const Data> data_;
};
// New-run metadata authority only. This ring retains certificates, never an
// OutcomeFrame, ancestor payload, source reader or persisted authority token.
class PublicationHistory {
public:
    PublicationHistory(PublicationHistory&&) noexcept;
    PublicationHistory& operator=(PublicationHistory&&) noexcept;
    ~PublicationHistory() noexcept;
    PublicationHistory(const PublicationHistory&) = delete;
    PublicationHistory& operator=(const PublicationHistory&) = delete;
    [[nodiscard]] static Result<std::uint64_t> resource_bytes(std::uint64_t outputs, std::uint64_t preceding) noexcept;
    [[nodiscard]] static Result<PublicationHistory> create(std::shared_ptr<ResourceAccount>, const ResultSlot&, std::uint64_t preceding) noexcept;
    [[nodiscard]] Result<void> capture(const ResultSlot&) noexcept;
private:
    friend class ProvenanceValidator;
    struct Data;
    explicit PublicationHistory(std::unique_ptr<Data>) noexcept;
    std::unique_ptr<Data> data_;
};
} // namespace lexongine
