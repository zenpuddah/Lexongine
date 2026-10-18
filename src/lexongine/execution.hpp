#pragma once

#include "lexongine/plan.hpp"

namespace lexongine {
class OutcomeView {
public:
    OutcomeView() = default;
    OutcomeView(OutcomeView&&) noexcept = default;
    OutcomeView& operator=(OutcomeView&&) noexcept = default;
    OutcomeView(const OutcomeView&) = delete;
    OutcomeView& operator=(const OutcomeView&) = delete;
    [[nodiscard]] static Result<OutcomeView> capture(const ResultSlot&, std::shared_ptr<ResourceAccount>, std::string_view holder, std::string_view reason) noexcept;
    [[nodiscard]] const OutcomeFrame* frame() const noexcept;
    template<class T> Result<ReadValue<T>> read(PortIndex port, std::int64_t decision_time) const noexcept {
        return guarded([&]() -> Result<ReadValue<T>> {
            const auto* current = frame();
            if (!current || current->kind != OutcomeKind::Published || port >= current->outputs.size()) { return diagnostic(FailureCode::MissingRequired, "receipt has no requested publication"); }
            if (current->available_at_ns > decision_time) { return diagnostic(FailureCode::FutureRead, "receipt publication is not available"); }
            auto value = Immutable<T>::from_control(current->outputs[port]->control(), "receipt_reader", "published_view");
            if (const auto* error = std::get_if<Diagnostic>(&value)) { return *error; }
            return ReadValue<T>{std::get<Immutable<T>>(std::move(value)), storage(port),
                current->certificates[port], std::nullopt, {}};
        });
    }
private:
    struct Data;
    explicit OutcomeView(std::shared_ptr<const Data> data) noexcept : data_(std::move(data)) {}
    [[nodiscard]] StorageKind storage(PortIndex) const noexcept;
    std::shared_ptr<const Data> data_;
};
struct ObservationDelivery { RequestContext context; PortIndex port; OutcomeView value; bool delivered = false; };
struct LogicalReach {
    std::uint32_t logical = 0;
    bool reached = false, advanced = false, published = false, alias = false;
    OutcomeKind outcome = OutcomeKind::MissingRequired;
};
struct StepReceipt {
    Reservation charge;
    StepStatus status;
    std::optional<UpdateKey> key;
    std::optional<TemporalExtent> extent;
    std::optional<std::int64_t> available_at_ns;
    std::optional<SourceGap> gap;
    std::optional<SequenceScope> completed_scope;
    std::optional<OutcomeView> source;
    std::vector<ObservationDelivery> observations;
    std::vector<LogicalReach> reach;
    std::optional<Diagnostic> cause;
};
struct ProducerStatistics {
    LogicalId id;
    std::uint32_t physical;
    std::uint64_t advances = 0, publications = 0, aliases = 0, waiting = 0, cached_deliveries = 0;
};
struct ObservationStatistics { RequestId id; std::uint64_t delivered = 0; };
struct RunReport {
    Reservation charge;
    std::string run_id;
    NumericalOutcome numerical = NumericalOutcome::Aborted;
    IntegrationOutcome integration = IntegrationOutcome::NotFinished;
    AcceptanceStatus acceptance = AcceptanceStatus::Pending;
    CachePublicationStatus cache = CachePublicationStatus::Disabled;
    std::optional<Diagnostic> cause;
    std::optional<Diagnostic> secondary_cause;
    std::vector<ProducerStatistics> producers;
    std::vector<ObservationStatistics> observations;
    std::vector<RequestRejection> rejected;
    std::uint64_t complete_updates = 0, gap_events = 0, missing_intervals = 0;
    std::vector<RetentionEvent> outstanding_holds;
    ResourceBounds current{}, peak{};
    std::string host_contract_id;
    std::uint32_t host_contract_version = 0;
    Sha256 host_experiment_identity{};
    std::vector<EvidenceReference> evidence;
    std::string cache_commit_id;
    Sha256 cache_record_digest{};
    std::uint64_t cache_published_entries = 0, cache_bypassed_entries = 0;
    ResourceBounds store_current{}, store_peak{};
    std::uint64_t cache_staged_entries = 0;
    std::vector<EntryReference> cache_entries;
    std::vector<RequiredEvidenceSpec> required_evidence;
    std::vector<LogicalReach> last_reach;
};
struct ObservationAdapter {
    ModulePin pin;
    RequestId request;
    ValueContract expected;
    std::function<Result<void>(const RequestContext&, const ResultSlot&, PortIndex, const UpdateContext&)> deliver;
};
template<class T, class Callback> ObservationAdapter make_observer(RequestId request, const RegisteredType<T>& type, Callback callback, ModulePin pin = {}) {
    return {pin, std::move(request), {type.policy.contract, typeid(T), type.policy.storage, type.policy.pin},
        [callback = std::move(callback), pin](const RequestContext& request, const ResultSlot& slot, PortIndex port, const UpdateContext& context) mutable -> Result<void> {
            auto value = slot.template read<T>(port, context, std::nullopt, pin);
            if (const auto* error = std::get_if<Diagnostic>(&value)) { return *error; }
            return callback(std::get<ReadValue<T>>(value), request);
        }};
}
// Typed test sinks and file adapters implement the same required open/write/close
// gate. The integration adapter owns encoding, byte limits and actual I/O.
struct ExecutionEvidence {
    ModulePin pin;
    RetentionLogConfiguration specification;
    std::function<Result<void>()> open;
    std::function<Result<void>(const RetentionEvent&)> write;
    std::function<Result<void>(const RunReport&)> finalize;
    std::function<Result<EvidenceReference>()> reference{};
};
struct RequiredEvidenceSink {
    ModulePin pin;
    RequiredEvidenceSpec specification;
    std::function<Result<void>()> open;
    std::function<Result<void>(std::uint32_t, const OutcomeFrame&)> outcome;
    std::function<Result<void>(const SourceGap&)> gap;
    std::function<Result<EvidenceReference>(const RunReport&)> finalize;
};
struct RunServices { std::vector<ObservationAdapter> observers; ExecutionEvidence evidence; IntegrationCompletion standalone; std::vector<RequiredEvidenceSink> required{}; };
class RunInstance {
public:
    RunInstance(RunInstance&&) noexcept;
    RunInstance& operator=(RunInstance&&) noexcept;
    ~RunInstance() noexcept;
    RunInstance(const RunInstance&) = delete;
    RunInstance& operator=(const RunInstance&) = delete;
    [[nodiscard]] RunLifecycle lifecycle() const noexcept;
private:
    friend class ExecutionAccess;
    struct Impl;
    explicit RunInstance(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
};
[[nodiscard]] Result<RunInstance> instantiate(std::shared_ptr<const PreparedPlan>, RunServices) noexcept;
[[nodiscard]] Result<StepReceipt> advance_one(RunInstance&) noexcept;
[[nodiscard]] Result<void> fail_host(RunInstance&, const Diagnostic&) noexcept;
[[nodiscard]] Result<void> abort_host(RunInstance&, const Diagnostic&) noexcept;
[[nodiscard]] Result<RunReport> finish(RunInstance&, IntegrationCompletion) noexcept;
[[nodiscard]] Result<RunReport> execute(RunInstance&) noexcept;
} // namespace lexongine
