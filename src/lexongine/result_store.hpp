#pragma once

#include "lexongine/reuse.hpp"
#include "lexongine/configuration.hpp"
#include "lexongine/evidence.hpp"

namespace lexongine {
struct RunReport;
enum class CacheMissReason { Absent, Incompatible, Unaccepted };
struct CacheMiss { CacheMissReason reason; };
struct ArtifactOrigin {
    std::string run_id, commit_id;
    Sha256 record_digest;
    NumericalOutcome numerical;
    IntegrationOutcome integration;
    AcceptanceStatus acceptance;
    friend bool operator==(const ArtifactOrigin&, const ArtifactOrigin&) = default;
};
// Views are valid while the owning payload callbacks remain pinned. They are
// host/preparation capabilities, never numerical inputs or serialized pointers.
struct ArtifactRecordView {
    const UpdateKey& key;
    const TemporalExtent& extent;
    std::int64_t available_at_ns;
    OutcomeKind kind;
    std::uint64_t remaining_preceding;
    std::uint32_t declared_reason;
    std::span<const ParentReference> parents;
};
struct ArtifactInspection {
    ModulePin pin;
    ArtifactIdentity identity;
    std::shared_ptr<const void> storage;
};
struct ArtifactPayload {
    ModulePin pin;
    std::shared_ptr<const void> snapshot;
    SourceCoverage coverage;
    InitializationSpec initialization;
    HistoryRequirement history;
    ArtifactOrigin origin;
    std::uint64_t outcome_count, decoded_bytes;
    std::function<Result<ArtifactRecordView>(std::uint64_t)> record;
    // Return private Published staging: complete values for a Published record,
    // empty ports for Warming. Core applies status, availability and new-run parents.
    std::function<Result<PendingOutcome>(std::uint64_t, const UpdateContext&, std::shared_ptr<ResourceAccount>)> materialize;
};
struct ArtifactWriter {
    ModulePin pin;
    std::shared_ptr<void> storage;
    std::function<Result<void>(const OutcomeFrame&)> append;
};
struct Committed { Sha256 record_digest; };
struct NotCommitted { Diagnostic cause; };
struct CommitUnconfirmed { Diagnostic cause; };
using CommitOutcome = std::variant<Committed, NotCommitted, CommitUnconfirmed>;
struct StorePublication {
    ModulePin pin;
    CachePublicationStatus status;
    std::uint64_t published_entries, bypassed_entries;
    std::string commit_id;
    std::function<CommitOutcome()> publish;
    std::vector<EntryReference> entries{};
};
using SnapshotFinalizer = std::function<Result<void>(const StorePublication&)>;
struct StoreServiceLimits {
    IdentityLimits identity;
    std::uint64_t metadata_bytes, cursor_bytes, writer_bytes, max_outcomes, max_parents;
};
struct ResultStoreCallbacks {
    ModulePin pin;
    Contract codec, validation;
    std::function<Result<bool>(const PreparedPlan&, std::uint32_t)> supports;
    std::function<Result<ResourceBounds>(const PreparedPlan&, std::uint32_t)> staging_resources;
    std::function<Result<std::variant<CacheMiss, ArtifactInspection>>(const ArtifactIdentity&)> lookup;
    std::function<Result<ArtifactPayload>(ArtifactInspection&&, const PreparedPlan&, std::uint32_t)> validate;
    std::function<Result<ArtifactWriter>(const ArtifactIdentity&, const PreparedPlan&, std::uint32_t)> stage;
    std::function<Result<StorePublication>(std::vector<ArtifactWriter>&&, const RunReport&, const SnapshotFinalizer&)> prepare_commit;
    std::function<Result<CommitOutcome>(std::string_view)> reconcile;
};
class ResultStore;
class ArtifactCandidate {
public:
    ArtifactCandidate(ArtifactCandidate&&) noexcept;
    ArtifactCandidate& operator=(ArtifactCandidate&&) noexcept;
    ~ArtifactCandidate() noexcept;
    ArtifactCandidate(const ArtifactCandidate&) = delete;
    ArtifactCandidate& operator=(const ArtifactCandidate&) = delete;
private:
    friend class ResultStore;
    struct Data;
    explicit ArtifactCandidate(std::unique_ptr<Data>) noexcept;
    std::unique_ptr<Data> data_;
};
using LookupDecision = std::variant<CacheMiss, ArtifactCandidate>;
class ArtifactCursor {
public:
    ArtifactCursor(ArtifactCursor&&) noexcept;
    ArtifactCursor& operator=(ArtifactCursor&&) noexcept;
    ~ArtifactCursor() noexcept;
    ArtifactCursor(const ArtifactCursor&) = delete;
    ArtifactCursor& operator=(const ArtifactCursor&) = delete;
    [[nodiscard]] Result<PendingOutcome> next(const UpdateContext&) noexcept;
    [[nodiscard]] bool exhausted() const noexcept;
private:
    friend class ValidatedArtifact;
    friend class ResultStore;
    struct Data;
    explicit ArtifactCursor(std::unique_ptr<Data>) noexcept;
    std::unique_ptr<Data> data_;
};
class ValidatedArtifact {
public:
    [[nodiscard]] const ArtifactOrigin& origin() const noexcept;
    [[nodiscard]] const ArtifactIdentity& identity() const noexcept;
    [[nodiscard]] std::uint64_t decoded_bytes() const noexcept;
    [[nodiscard]] Result<ArtifactCursor> cursor(std::shared_ptr<const PreparedPlan>, std::uint32_t,
        std::shared_ptr<ResourceAccount>, std::vector<BoundInput>, std::vector<std::shared_ptr<PublicationHistory>>) const noexcept;
private:
    friend class ResultStore;
    friend class ArtifactCursor;
    struct Data;
    explicit ValidatedArtifact(std::shared_ptr<const Data> data) noexcept : data_(std::move(data)) {}
    std::shared_ptr<const Data> data_;
};
class StagedEntry {
public:
    StagedEntry(StagedEntry&&) noexcept;
    StagedEntry& operator=(StagedEntry&&) noexcept;
    ~StagedEntry() noexcept;
    StagedEntry(const StagedEntry&) = delete;
    StagedEntry& operator=(const StagedEntry&) = delete;
    [[nodiscard]] Result<void> append(const OutcomeFrame&) noexcept;
private:
    friend class ResultStore;
    struct Data;
    explicit StagedEntry(std::unique_ptr<Data>) noexcept;
    std::unique_ptr<Data> data_;
};
class PreparedCommit {
public:
    PreparedCommit(PreparedCommit&&) noexcept;
    PreparedCommit& operator=(PreparedCommit&&) noexcept;
    ~PreparedCommit() noexcept;
    PreparedCommit(const PreparedCommit&) = delete;
    PreparedCommit& operator=(const PreparedCommit&) = delete;
    [[nodiscard]] CachePublicationStatus status() const noexcept;
    [[nodiscard]] std::uint64_t published_entries() const noexcept;
    [[nodiscard]] std::uint64_t bypassed_entries() const noexcept;
    [[nodiscard]] std::string_view commit_id() const noexcept;
private:
    friend class ResultStore;
    struct Data;
    explicit PreparedCommit(std::unique_ptr<Data>) noexcept;
    std::unique_ptr<Data> data_;
};
class ResultStore {
public:
    [[nodiscard]] static Result<std::shared_ptr<ResultStore>> create(LocalCacheConfiguration, StoreServiceLimits,
        ResultStoreCallbacks, std::shared_ptr<ResourceAccount>, DigestFunction) noexcept;
    [[nodiscard]] const LocalCacheConfiguration& configuration() const noexcept;
    [[nodiscard]] const StoreServiceLimits& limits() const noexcept;
    [[nodiscard]] std::shared_ptr<ResourceAccount> account() const noexcept;
    [[nodiscard]] Result<bool> supports(const PreparedPlan&, std::uint32_t) const noexcept;
    [[nodiscard]] Result<ResourceBounds> staging_resources(const PreparedPlan&, std::uint32_t) const noexcept;
    [[nodiscard]] Result<ArtifactIdentity> identity(const PreparedPlan&, std::uint32_t) const noexcept;
    [[nodiscard]] Result<LookupDecision> lookup(const ArtifactIdentity&) noexcept;
    [[nodiscard]] Result<ValidatedArtifact> validate(ArtifactCandidate&&, const ArtifactIdentity&, const PreparedPlan&, std::uint32_t) noexcept;
    [[nodiscard]] Result<StagedEntry> stage(const ArtifactIdentity&, std::shared_ptr<const PreparedPlan>, std::uint32_t) noexcept;
    [[nodiscard]] Result<PreparedCommit> prepare_commit(std::vector<StagedEntry>&&, const RunReport&, SnapshotFinalizer = {}) noexcept;
    [[nodiscard]] CommitOutcome publish(PreparedCommit&&) noexcept;
private:
    struct Data;
    explicit ResultStore(std::shared_ptr<Data> data) noexcept : data_(std::move(data)) {}
    std::shared_ptr<Data> data_;
};
} // namespace lexongine
