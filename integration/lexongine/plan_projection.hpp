#pragma once
#include "artifact_metadata.hpp"
namespace lexongine {
wire::Element* project_plan_identity(wire::Document&, const PreparedPlan&);
wire::Element* project_resources(wire::Document&, const ResourceBounds&);
wire::Element* project_observation(wire::Document&, const ObservationRequest&);
wire::Element* project_diagnostic(wire::Document&, const Diagnostic&);
wire::Element* project_parent(wire::Document&, const ParentReference&);
[[nodiscard]] Result<Sha256> plan_digest(const PreparedPlan&, std::shared_ptr<ResourceAccount>, const MetadataLimits&) noexcept;
[[nodiscard]] Result<Sha256> acceptance_digest(std::span<const RequiredEvidenceSpec>, std::shared_ptr<ResourceAccount>, const MetadataLimits&) noexcept;
[[nodiscard]] Result<Sha256> implementation_digest(const ImplementationIdentity&, std::shared_ptr<ResourceAccount>, const MetadataLimits&) noexcept;
std::string_view name(NumericalOutcome);
std::string_view name(IntegrationOutcome);
std::string_view name(AcceptanceStatus);
std::string_view name(CachePublicationStatus);
std::string_view name(OutcomeKind);
}
