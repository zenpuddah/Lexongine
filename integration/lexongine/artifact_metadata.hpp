#pragma once
#include "metadata_xml.hpp"
#include "lexongine/result_store.hpp"
namespace lexongine {
struct MetadataLimits { wire::Limits xml; std::uint64_t owned_bytes; };
template<class T> struct OwnedMetadata { Reservation charge; T value; };
struct ArtifactManifest {
    std::string artifact_id, origin_run_id;
    Sha256 identity_digest, payload_digest, coverage_digest;
    std::uint64_t outcome_count, payload_bytes;
    Contract codec, validation;
    std::vector<std::byte> canonical_identity;
    SequenceScope processing_scope;
    std::vector<TypeContract> output_types;
    SourceCoverage coverage;
    InitializationSpec initialization;
    CountHistory history;
};
struct AcceptedRunRecord {
    std::string run_id, commit_id;
    CachePublicationStatus cache_status;
    Sha256 acceptance_policy_digest, build_identity_digest;
    EvidenceReference snapshot;
    std::vector<EvidenceReference> inventory;
    std::vector<EntryReference> entries;
};
[[nodiscard]] Result<wire::EncodedXml> encode_manifest(const ArtifactManifest&, std::shared_ptr<ResourceAccount>, const MetadataLimits&) noexcept;
[[nodiscard]] Result<OwnedMetadata<ArtifactManifest>> decode_manifest(std::string_view, std::shared_ptr<ResourceAccount>, const MetadataLimits&) noexcept;
[[nodiscard]] Result<wire::EncodedXml> encode_accepted_run(const AcceptedRunRecord&, std::shared_ptr<ResourceAccount>, const MetadataLimits&) noexcept;
[[nodiscard]] Result<OwnedMetadata<AcceptedRunRecord>> decode_accepted_run(std::string_view, std::shared_ptr<ResourceAccount>, const MetadataLimits&) noexcept;
[[nodiscard]] Result<Sha256> coverage_digest(const SourceCoverage&, std::shared_ptr<ResourceAccount>, const MetadataLimits&) noexcept;
wire::Element* project_entry(wire::Document&, const EntryReference&);
EntryReference decode_entry(const wire::Element&);
wire::Element* project_spec(wire::Document&, const RequiredEvidenceSpec&);
RequiredEvidenceSpec decode_spec(const wire::Element&);
[[nodiscard]] bool safe_identifier(std::string_view) noexcept;
// Checks the domain, kind and every top-level frame before returning the borrowed
// computation body. The caller keeps the manifest bytes alive.
[[nodiscard]] std::span<const std::byte> artifact_computation(const ArtifactManifest&);
[[nodiscard]] std::span<const std::byte> computation_body(std::span<const std::byte>);
}
