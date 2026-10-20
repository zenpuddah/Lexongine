#pragma once
#include "plan_projection.hpp"
namespace lexongine {
struct SnapshotLimits {
    MetadataLimits metadata;
    std::uint64_t capture_bytes, max_parents;
};
struct SnapshotSummary {
    std::string run_id, numerical, integration, acceptance, cache_status, commit_id;
    Sha256 plan_digest, host_experiment_identity;
    std::vector<RequiredEvidenceSpec> required;
    std::vector<EvidenceReference> inventory;
    std::vector<EntryReference> entries;
    std::vector<std::vector<std::byte>> computations;
};
class SnapshotEvidence {
  public:
    static Result<SnapshotEvidence> create(std::shared_ptr<const PreparedPlan>,
                                           RequiredEvidenceSpec, std::shared_ptr<ResourceAccount>,
                                           SnapshotLimits) noexcept;
    [[nodiscard]] RequiredEvidenceSink callbacks() const;

  private:
    struct Data;
    explicit SnapshotEvidence(std::shared_ptr<Data> data) : data_(std::move(data)) {}
    std::shared_ptr<Data> data_;
};
[[nodiscard]] Result<OwnedMetadata<SnapshotSummary>>
decode_snapshot(std::string_view, std::shared_ptr<ResourceAccount>,
                const MetadataLimits &) noexcept;
} // namespace lexongine
