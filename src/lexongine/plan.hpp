#pragma once

#include "lexongine/configuration.hpp"
#include "lexongine/source.hpp"
#include "lexongine/evidence.hpp"

namespace lexongine {
struct EffectScope {
    LogicalId node;
    SequenceScope evaluation;
};
struct PrepareOptions {
    std::string run_id;
    SourceRequest source;
    SourceValidationServices source_services;
    AccountLimits runtime_limits;
    ResourceBounds kernel_limits;
    std::uint64_t metadata_bytes, identity_bytes;
    DigestFunction digest;
    std::vector<EffectScope> effects;
    std::vector<RequiredEvidenceSpec> evidence{};
    std::shared_ptr<ResourceAccount> evidence_account{};
    ResourceBounds evidence_resources{};
};
struct RequestRejection {
    RequestId request;
    Diagnostic cause;
    std::vector<LogicalId> dependencies;
};
struct PreparedObservation {
    ObservationRequest request;
    std::uint32_t producer;
    PortIndex port;
};
class ResultStore;
class ArtifactIdentity;
class ValidatedArtifact;
class PreparedPlan {
  public:
    [[nodiscard]] const CompositionDeclaration &configuration() const noexcept;
    [[nodiscard]] const FrozenGraph &graph() const noexcept;
    [[nodiscard]] std::span<const GraphNode> nodes() const noexcept;
    [[nodiscard]] const SourceSnapshot &source() const noexcept;
    [[nodiscard]] const PreparedKernel *kernel(std::uint32_t logical) const noexcept;
    [[nodiscard]] std::span<const ValueContract> outputs(std::uint32_t logical) const;
    [[nodiscard]] std::span<const PreparedObservation> observations() const noexcept;
    [[nodiscard]] std::span<const RequestRejection> rejections() const noexcept;
    [[nodiscard]] bool needed(std::uint32_t logical) const;
    [[nodiscard]] std::uint64_t retention(std::uint32_t logical) const;
    [[nodiscard]] std::uint64_t preceding(std::uint32_t logical) const;
    [[nodiscard]] const Sha256 &identity(std::uint32_t logical) const;
    [[nodiscard]] const AccountLimits &runtime_limits() const noexcept;
    [[nodiscard]] const ResourceBounds &required_resources() const noexcept;
    [[nodiscard]] const RetentionLogConfiguration &evidence_specification() const noexcept;
    [[nodiscard]] std::uint64_t metadata_bytes() const noexcept;
    [[nodiscard]] const std::optional<SequenceScope> &effect_scope(std::uint32_t logical) const;
    [[nodiscard]] bool admitted() const noexcept;
    [[nodiscard]] Result<void> admit_instance() const noexcept;
    [[nodiscard]] std::shared_ptr<ResultStore> store() const noexcept;
    [[nodiscard]] const ArtifactIdentity *artifact_identity(std::uint32_t logical) const;
    [[nodiscard]] const ValidatedArtifact *artifact(std::uint32_t logical) const;
    [[nodiscard]] std::uint64_t certificate_history(std::uint32_t logical) const;
    [[nodiscard]] std::span<const RequiredEvidenceSpec> required_evidence() const noexcept;
    [[nodiscard]] std::shared_ptr<ResourceAccount> evidence_account() const noexcept;
    [[nodiscard]] std::uint64_t report_bytes() const noexcept;
    [[nodiscard]] const ResourceBounds &evidence_resource_bounds() const noexcept;
    [[nodiscard]] std::string_view reuse_decision(std::uint32_t logical) const;

  private:
    friend class PreparationAccess;
    struct Data;
    explicit PreparedPlan(std::shared_ptr<const Data> data) noexcept : data_(std::move(data)) {}
    std::shared_ptr<const Data> data_;
};
struct PrepareResult {
    Reservation charge;
    std::shared_ptr<const PreparedPlan> plan;
    std::vector<RequestRejection> rejected;
};
[[nodiscard]] Result<PrepareResult> prepare(const Composition &, const Registry &, SourceResolver &,
                                            std::shared_ptr<ResultStore>,
                                            const PrepareOptions &) noexcept;
} // namespace lexongine
