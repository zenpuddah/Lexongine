#include "lexongine/execution.hpp"
#include "lexongine/result_store.hpp"

namespace lexongine {
namespace {
struct ExecutionFailure {
    Diagnostic error;
};
template <class T> T checked(std::variant<T, Diagnostic> result) {
    if (const auto *error = std::get_if<Diagnostic>(&result)) {
        throw ExecutionFailure{*error};
    }
    return std::get<T>(std::move(result));
}
void checked(Result<void> result) {
    if (const auto *error = std::get_if<Diagnostic>(&result)) {
        throw ExecutionFailure{*error};
    }
}
template <class F> auto protected_call(F &&call) noexcept -> decltype(call()) {
    return guarded([&]() -> decltype(call()) {
        try {
            return call();
        } catch (const ExecutionFailure &failure) {
            return failure.error;
        }
    });
}
bool contains(const SequenceScope &scope, const UpdateKey &key) {
    return key.clock == scope.clock && key.sequence >= scope.begin && key.sequence < scope.end;
}
} // namespace
struct OutcomeView::Data {
    Reservation charge;
    std::shared_ptr<const OutcomeFrame> frame;
    std::vector<StorageKind> storage;
    std::vector<HoldLease> holders;
};
const OutcomeFrame *OutcomeView::frame() const noexcept {
    return data_ ? data_->frame.get() : nullptr;
}
StorageKind OutcomeView::storage(PortIndex port) const noexcept {
    return data_->storage[port];
}
Result<OutcomeView> OutcomeView::capture(const ResultSlot &slot,
                                         std::shared_ptr<ResourceAccount> account,
                                         std::string_view holder,
                                         std::string_view reason) noexcept {
    return protected_call([&]() -> Result<OutcomeView> {
        if (!account || !slot.current_) {
            return diagnostic(FailureCode::MissingRequired, "cannot capture an unresolved slot");
        }
        const auto bytes = checked(
            checked_add(sizeof(Data) + 256,
                        checked(checked_multiply(slot.contracts_.size(),
                                                 sizeof(HoldLease) + sizeof(StorageKind)))));
        auto charge = checked(account->reserve(ResourceKind::Staging, bytes));
        Data data{std::move(charge), slot.current_, {}, {}};
        data.storage.reserve(slot.contracts_.size());
        data.holders.reserve(slot.contracts_.size());
        for (std::size_t i = 0; i < slot.contracts_.size(); ++i) {
            data.storage.push_back(slot.contracts_[i].storage);
            if (slot.current_->outputs[i]) {
                const auto &control = slot.current_->outputs[i]->control();
                data.holders.push_back(
                    checked(control->account()->hold(control->id(), holder, reason)));
            }
        }
        return OutcomeView{std::make_shared<const Data>(std::move(data))};
    });
}
struct RunInstance::Impl {
    Reservation charge;
    std::shared_ptr<const PreparedPlan> plan;
    std::shared_ptr<ResourceAccount> account;
    RunServices services;
    std::optional<SourceCursor> source;
    std::vector<std::shared_ptr<ResultSlot>> slots;
    std::vector<std::optional<KernelInstance>> kernels;
    std::vector<std::uint32_t> observers;
    std::vector<bool> resolved, reached;
    RunReport report;
    std::vector<std::optional<ArtifactCursor>> cached{};
    std::vector<std::shared_ptr<PublicationHistory>> histories{};
    std::vector<std::optional<StagedEntry>> staged{};
    std::vector<StagedEntry> commit_entries{};
    std::vector<bool> evidence_opened{}, evidence_closed{};
    std::vector<std::uint64_t> evidence_records{};
    RunLifecycle lifecycle = RunLifecycle::Created;
    bool evidence_started = false, exhausted = false, standalone_active = false, advancing = false;
    std::size_t event_position = 0;
    std::uint64_t emitted_events = 0;
    std::int64_t decision = std::numeric_limits<std::int64_t>::min();
};
class ExecutionAccess {
  public:
    static Result<RunInstance> instantiate(std::shared_ptr<const PreparedPlan>, RunServices);
    static Result<StepReceipt> advance(RunInstance &, bool);
    static Result<RunReport> finish(RunInstance &, IntegrationCompletion);
    static Result<void> host_stop(RunInstance &, const Diagnostic &, IntegrationOutcome);
    static Result<RunReport> execute(RunInstance &);

  private:
    static void cause(RunInstance::Impl &run, Diagnostic error) noexcept {
        if (!run.report.cause) {
            run.report.cause = error;
        } else if (!run.report.secondary_cause) {
            run.report.secondary_cause = error;
        }
    }
    static void stop(RunInstance::Impl &run, Diagnostic error) noexcept {
        cause(run, error);
        if (run.report.integration != IntegrationOutcome::Failed &&
            run.report.integration != IntegrationOutcome::Aborted) {
            run.report.numerical = NumericalOutcome::Failed;
        }
        run.lifecycle = RunLifecycle::AwaitingCompletion;
    }
    static Result<void> flush(RunInstance::Impl &run) {
        return guarded([&]() -> Result<void> {
            const auto events = run.account->events();
            while (run.event_position < events.size()) {
                const auto &event = events[run.event_position++];
                // Retention evidence describes payload ownership; other reserved
                // categories remain visible in current/peak resource accounting.
                if (event.resource_kind != ResourceKind::Retention) {
                    continue;
                }
                if (run.emitted_events == run.services.evidence.specification.max_events) {
                    return diagnostic(FailureCode::EvidenceQuota,
                                      "required retention record limit exceeded");
                }
                auto written = run.services.evidence.write(event);
                if (const auto *error = std::get_if<Diagnostic>(&written)) {
                    return *error;
                }
                ++run.emitted_events;
            }
            if (run.account->failure()) {
                return *run.account->failure();
            }
            return ok();
        });
    }
};
RunInstance::RunInstance(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
RunInstance::RunInstance(RunInstance &&) noexcept = default;
RunInstance &RunInstance::operator=(RunInstance &&) noexcept = default;
RunInstance::~RunInstance() noexcept = default;
RunLifecycle RunInstance::lifecycle() const noexcept {
    return impl_ ? impl_->lifecycle : RunLifecycle::Terminal;
}
Result<RunInstance> instantiate(std::shared_ptr<const PreparedPlan> plan,
                                RunServices services) noexcept {
    return protected_call(
        [&]() { return ExecutionAccess::instantiate(std::move(plan), std::move(services)); });
}
Result<RunInstance> ExecutionAccess::instantiate(std::shared_ptr<const PreparedPlan> plan,
                                                 RunServices services) {
    if (!plan || !services.evidence.open || !services.evidence.write ||
        !services.evidence.finalize ||
        !(services.evidence.specification == plan->evidence_specification()) ||
        services.standalone.host_contract_id.empty() ||
        services.standalone.host_contract_version == 0 ||
        services.standalone.host_experiment_identity == Sha256{} ||
        services.observers.size() != plan->observations().size()) {
        return diagnostic(FailureCode::InvalidConfig, "run services do not match the frozen plan");
    }
    if (services.required.size() != plan->required_evidence().size()) {
        return diagnostic(FailureCode::InvalidConfig,
                          "required evidence services differ from frozen plan");
    }
    for (std::size_t i = 0; i < services.required.size(); ++i) {
        if (services.required[i].specification != plan->required_evidence()[i] ||
            !services.required[i].open || !services.required[i].finalize) {
            return diagnostic(FailureCode::InvalidConfig,
                              "required evidence binding is incomplete or changed");
        }
    }
    checked(plan->admit_instance());
    auto account = checked(ResourceAccount::create(plan->runtime_limits()));
    auto charge = checked(account->reserve(ResourceKind::Graph, plan->metadata_bytes()));
    const auto report_bytes = plan->report_bytes();
    auto report_charge = checked(account->reserve(ResourceKind::Evidence, report_bytes));
    RunReport report;
    report.charge = std::move(report_charge);
    report.run_id = plan->configuration().run.id;
    report.producers.reserve(plan->nodes().size());
    report.observations.reserve(plan->observations().size());
    report.rejected.assign(plan->rejections().begin(), plan->rejections().end());
    report.outstanding_holds.reserve(plan->runtime_limits().max_holders);
    report.evidence.reserve(1 + services.required.size());
    report.cache_entries.reserve(plan->nodes().size());
    report.last_reach.resize(plan->nodes().size());
    const auto &retention = plan->evidence_specification();
    report.required_evidence.push_back({"retention", retention.format, retention.path,
                                        retention.open, retention.max_bytes, retention.max_events,
                                        retention.flush, retention.failure});
    report.required_evidence.insert(report.required_evidence.end(),
                                    plan->required_evidence().begin(),
                                    plan->required_evidence().end());
    if (plan->store()) {
        report.cache = CachePublicationStatus::NoNewEntries;
        report.cache_commit_id.reserve(
            static_cast<std::size_t>(plan->store()->limits().identity.string_bytes));
    }
    report.host_contract_id = services.standalone.host_contract_id;
    report.host_contract_version = services.standalone.host_contract_version;
    report.host_experiment_identity = services.standalone.host_experiment_identity;
    const auto count = plan->nodes().size();
    auto run = std::make_unique<RunInstance::Impl>(
        RunInstance::Impl{std::move(charge),
                          plan,
                          account,
                          std::move(services),
                          {},
                          std::vector<std::shared_ptr<ResultSlot>>(count),
                          std::vector<std::optional<KernelInstance>>(count),
                          {},
                          std::vector<bool>(count),
                          std::vector<bool>(count),
                          std::move(report)});
    run->evidence_opened.resize(run->services.required.size());
    run->evidence_closed.resize(run->services.required.size());
    run->evidence_records.resize(run->services.required.size());
    for (std::uint32_t i = 0; i < count; ++i) {
        run->report.producers.push_back(
            {plan->nodes()[i].spec.id, plan->graph().physical_index(i)});
    }
    for (const auto &observation : plan->observations()) {
        const auto found = std::find_if(
            run->services.observers.begin(), run->services.observers.end(),
            [&](const auto &observer) { return observer.request == observation.request.id; });
        if (found == run->services.observers.end() || !found->deliver) {
            return diagnostic(FailureCode::InvalidConfig,
                              "required observation adapter is missing");
        }
        const auto &expected = plan->outputs(observation.producer)[observation.port];
        if (!(found->expected.semantic == expected.semantic) ||
            found->expected.witness != expected.witness ||
            found->expected.storage != expected.storage) {
            return diagnostic(FailureCode::TypeMismatch,
                              "observation adapter type differs from its requested output");
        }
        const auto index = static_cast<std::uint32_t>(found - run->services.observers.begin());
        if (std::find(run->observers.begin(), run->observers.end(), index) !=
            run->observers.end()) {
            return diagnostic(FailureCode::InvalidConfig, "duplicate observation adapter");
        }
        run->observers.push_back(index);
        run->report.observations.push_back({observation.request.id});
    }
    for (const auto i : plan->graph().physical_order()) {
        if (!plan->needed(i)) {
            continue;
        }
        run->slots[i] = checked(ResultSlot::create(
            account, {plan->outputs(i).begin(), plan->outputs(i).end()}, plan->retention(i),
            plan->identity(i),
            i == 0 ? std::optional<Sha256>{plan->source().metadata().source_reference_digest}
                   : std::nullopt));
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        if (plan->needed(i)) {
            run->slots[i] = run->slots[plan->graph().representative(i)];
        }
    }
    if (plan->store()) {
        run->cached.resize(count);
        run->histories.resize(count);
        run->staged.resize(count);
        run->commit_entries.reserve(count);
        for (const auto i : plan->graph().physical_order()) {
            if (plan->needed(i)) {
                run->histories[i] =
                    std::make_shared<PublicationHistory>(checked(PublicationHistory::create(
                        account, *run->slots[i], plan->certificate_history(i))));
            }
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            if (plan->needed(i)) {
                run->histories[i] = run->histories[plan->graph().representative(i)];
            }
        }
    }
    PublicationAllocator allocator{account, 0};
    run->source.emplace(checked(plan->source().cursor(allocator)));
    for (const auto i : plan->graph().physical_order()) {
        if (!plan->needed(i) || i == 0) {
            continue;
        }
        InstanceServices instance{account, {}};
        for (std::size_t p = 0; p < plan->graph().inputs(i).size(); ++p) {
            const auto edge = plan->graph().inputs(i)[p];
            instance.inputs.push_back({run->slots[edge.producer], edge.output,
                                       plan->nodes()[i].spec.inputs[p].prior_outputs});
        }
        if (const auto *artifact = plan->artifact(i)) {
            std::vector<std::shared_ptr<PublicationHistory>> histories;
            histories.reserve(instance.inputs.size());
            for (const auto edge : plan->graph().inputs(i)) {
                histories.push_back(run->histories[edge.producer]);
            }
            run->cached[i].emplace(checked(artifact->cursor(
                plan, i, account, std::move(instance.inputs), std::move(histories))));
        } else {
            run->kernels[i].emplace(checked(plan->kernel(i)->instantiate(instance)));
            if (const auto *identity = plan->artifact_identity(i)) {
                run->staged[i].emplace(checked(plan->store()->stage(*identity, plan, i)));
                run->report.cache = CachePublicationStatus::Staged;
            }
        }
    }
    return RunInstance{std::move(run)};
}
Result<StepReceipt> advance_one(RunInstance &run) noexcept {
    return protected_call([&]() { return ExecutionAccess::advance(run, false); });
}
Result<StepReceipt> ExecutionAccess::advance(RunInstance &value, bool standalone) {
    if (!value.impl_ || value.impl_->lifecycle == RunLifecycle::Terminal) {
        return diagnostic(FailureCode::AlreadyExecuted, "run is terminal");
    }
    auto &run = *value.impl_;
    const auto &plan = *run.plan;
    if ((plan.configuration().run.driver == Driver::Standalone) != standalone || run.advancing ||
        run.lifecycle == RunLifecycle::AwaitingCompletion ||
        run.lifecycle == RunLifecycle::Finalizing) {
        return diagnostic(FailureCode::UpdateProtocol,
                          "driver or run lifecycle forbids advancement");
    }
    struct AdvanceGuard {
        bool &active;
        ResourceAccount &account;
        ~AdvanceGuard() {
            account.set_owner(std::nullopt);
            active = false;
        }
    } guard{run.advancing, *run.account};
    run.advancing = true;
    auto reserved = run.account->reserve(ResourceKind::Staging, plan.metadata_bytes());
    if (const auto *error = std::get_if<Diagnostic>(&reserved)) {
        stop(run, *error);
        StepReceipt stopped;
        stopped.status = StepStatus::Stopped;
        stopped.cause = *error;
        return stopped;
    }
    StepReceipt receipt;
    receipt.charge = std::get<Reservation>(std::move(reserved));
    receipt.status = StepStatus::Processed;
    auto result = protected_call([&]() -> Result<void> {
        if (run.lifecycle == RunLifecycle::Created) {
            run.evidence_started = true;
            checked(run.services.evidence.open());
            for (std::size_t i = 0; i < run.services.required.size(); ++i) {
                run.evidence_opened[i] = true;
                checked(run.services.required[i].open());
            }
            run.lifecycle = RunLifecycle::Running;
            checked(flush(run));
        }
        const auto *next = run.source->descriptor();
        if (!next) {
            const auto end = checked(run.source->finish());
            receipt.completed_scope = end.completed_scope;
            receipt.status = StepStatus::Exhausted;
            run.exhausted = true;
            run.report.numerical = NumericalOutcome::Completed;
            run.lifecycle = RunLifecycle::AwaitingCompletion;
            checked(flush(run));
            return ok();
        }
        run.decision = std::max(run.decision, next->available_at_ns);
        UpdateContext context{next->key, next->extent, run.decision, next->available_at_ns};
        receipt.key = context.key;
        receipt.extent = context.extent;
        receipt.available_at_ns = context.activation_available_at_ns;
        receipt.observations.reserve(plan.observations().size());
        receipt.reach.resize(plan.nodes().size());
        for (std::uint32_t i = 0; i < receipt.reach.size(); ++i) {
            receipt.reach[i].logical = i;
        }
        std::vector<std::size_t> delivery_index(plan.observations().size(),
                                                std::numeric_limits<std::size_t>::max());
        for (std::size_t i = 0; i < plan.observations().size(); ++i) {
            const auto &request = plan.observations()[i].request;
            if (contains(request.evaluation, context.key)) {
                delivery_index[i] = receipt.observations.size();
                receipt.observations.push_back(
                    {{request.id, request.evaluation, Phase::Evaluation, context.key},
                     plan.observations()[i].port,
                     {},
                     false});
            }
        }
        std::fill(run.resolved.begin(), run.resolved.end(), false);
        std::fill(run.reached.begin(), run.reached.end(), false);
        checked(run.account->advance_age(context.key.sequence));
        run.account->set_owner(AllocationOwner{0, context.key.sequence});
        auto source_event = checked(run.source->next(context));
        if (const auto *gap = std::get_if<SourceGap>(&source_event)) {
            receipt.status = StepStatus::Gap;
            receipt.gap = *gap;
            receipt.observations.clear();
            ++run.report.gap_events;
            for (std::size_t i = 0; i < run.services.required.size(); ++i) {
                auto &sink = run.services.required[i];
                if (sink.gap) {
                    if (run.evidence_records[i] == sink.specification.max_records) {
                        return diagnostic(FailureCode::EvidenceQuota,
                                          "required event evidence limit exceeded");
                    }
                    ++run.evidence_records[i];
                    checked(sink.gap(*gap));
                }
            }
            run.report.missing_intervals += gap->missing_scope.end - gap->missing_scope.begin;
            checked(flush(run));
            return ok();
        }
        run.slots[0]->commit(checked(validate_pending(
            std::get<PendingOutcome>(std::move(source_event)), *run.slots[0], context)));
        if (plan.store()) {
            checked(run.histories[0]->capture(*run.slots[0]));
        }
        run.resolved[0] = true;
        ++run.report.complete_updates;
        ++run.report.producers[0].advances;
        ++run.report.producers[0].publications;
        receipt.source.emplace(
            checked(OutcomeView::capture(*run.slots[0], run.account, "host", "current_source")));
        checked(walk_logical(
            plan.graph(), [&](std::uint32_t logical, std::uint32_t, bool first) -> Result<void> {
                return protected_call([&]() -> Result<void> {
                    if (!plan.needed(logical)) {
                        return ok();
                    }
                    const auto representative = plan.graph().representative(logical);
                    if (first && logical != 0) {
                        if (run.resolved[representative]) {
                            return diagnostic(FailureCode::UpdateProtocol,
                                              "producer resolved twice at one update");
                        }
                        bool waiting = false;
                        auto available = context.activation_available_at_ns;
                        for (const auto edge : plan.graph().inputs(logical)) {
                            const auto *frame = run.slots[edge.producer]->current();
                            if (!frame || frame->context.key != context.key ||
                                frame->context.extent != context.extent) {
                                return diagnostic(FailureCode::MissingRequired,
                                                  "required input has no current matching outcome");
                            }
                            if (frame->available_at_ns > context.decision_time_ns) {
                                return diagnostic(FailureCode::FutureRead,
                                                  "required input is not yet available");
                            }
                            available = std::max(available, frame->available_at_ns);
                            if (frame->kind == OutcomeKind::Warming) {
                                waiting = true;
                            } else if (frame->kind != OutcomeKind::Published) {
                                return diagnostic(FailureCode::MissingRequired,
                                                  "required input is absent or failed");
                            }
                        }
                        if (plan.effect_scope(logical) &&
                            !contains(*plan.effect_scope(logical), context.key)) {
                            waiting = true;
                        }
                        if (plan.store() && run.cached[logical]) {
                            run.account->set_owner(AllocationOwner{logical, context.key.sequence});
                            auto pending = checked(run.cached[logical]->next(context));
                            run.slots[logical]->commit(checked(validate_pending(
                                std::move(pending), *run.slots[logical], context)));
                            ++run.report.producers[logical].cached_deliveries;
                            if (waiting) {
                                ++run.report.producers[logical].waiting;
                            }
                            if (run.slots[logical]->current()->kind == OutcomeKind::Published) {
                                ++run.report.producers[logical].publications;
                            }
                        } else if (waiting) {
                            auto pending = checked(PendingOutcome::create(
                                run.account, context, plan.outputs(logical).size()));
                            const auto ready = checked(checked_add(plan.preceding(logical), 1));
                            checked(pending.waiting(ready > run.report.complete_updates
                                                        ? ready - run.report.complete_updates
                                                        : 0,
                                                    available));
                            run.slots[logical]->commit(checked(validate_pending(
                                std::move(pending), *run.slots[logical], context)));
                            ++run.report.producers[logical].waiting;
                        } else {
                            ++run.report.producers[logical].advances;
                            receipt.reach[logical].advanced = true;
                            run.account->set_owner(AllocationOwner{logical, context.key.sequence});
                            auto pending = checked(run.kernels[logical]->step(context));
                            run.slots[logical]->commit(checked(validate_pending(
                                std::move(pending), *run.slots[logical], context)));
                            if (run.slots[logical]->current()->kind == OutcomeKind::Published) {
                                ++run.report.producers[logical].publications;
                            }
                        }
                        if (plan.store()) {
                            checked(run.histories[logical]->capture(*run.slots[logical]));
                            if (run.staged[logical]) {
                                checked(
                                    run.staged[logical]->append(*run.slots[logical]->current()));
                            }
                        }
                        run.resolved[representative] = true;
                    } else if (!first) {
                        if (!run.resolved[representative]) {
                            return diagnostic(FailureCode::UpdateProtocol,
                                              "alias reached before physical resolution");
                        }
                        ++run.report.producers[logical].aliases;
                        receipt.reach[logical].alias = true;
                    }
                    run.reached[logical] = true;
                    if (first) {
                        for (std::size_t i = 0; i < run.services.required.size(); ++i) {
                            auto &sink = run.services.required[i];
                            if (sink.outcome) {
                                if (run.evidence_records[i] == sink.specification.max_records) {
                                    return diagnostic(FailureCode::EvidenceQuota,
                                                      "required outcome evidence limit exceeded");
                                }
                                ++run.evidence_records[i];
                                checked(sink.outcome(logical, *run.slots[logical]->current()));
                            }
                        }
                    }
                    receipt.reach[logical].reached = true;
                    receipt.reach[logical].advanced =
                        receipt.reach[logical].advanced || logical == 0;
                    receipt.reach[logical].outcome = run.slots[logical]->current()->kind;
                    receipt.reach[logical].published =
                        run.slots[logical]->current()->kind == OutcomeKind::Published;
                    checked(flush(run));
                    for (std::size_t i = 0; i < plan.observations().size(); ++i) {
                        const auto &observation = plan.observations()[i];
                        if (observation.producer != logical ||
                            delivery_index[i] == std::numeric_limits<std::size_t>::max()) {
                            continue;
                        }
                        if (run.slots[logical]->current()->kind != OutcomeKind::Published) {
                            return diagnostic(
                                FailureCode::MissingRequired,
                                "required observation is not published at its evaluation key");
                        }
                        auto &delivery = receipt.observations[delivery_index[i]];
                        delivery.value = checked(OutcomeView::capture(
                            *run.slots[logical], run.account, "host", "current_observation"));
                        checked(run.services.observers[run.observers[i]].deliver(
                            delivery.context, *run.slots[logical], observation.port, context));
                        delivery.delivered = true;
                        ++run.report.observations[i].delivered;
                        checked(flush(run));
                        if (run.lifecycle != RunLifecycle::Running) {
                            return run.report.cause.value_or(
                                diagnostic(FailureCode::UpdateProtocol,
                                           "host stopped during observation delivery"));
                        }
                    }
                    return ok();
                });
            }));
        checked(flush(run));
        return ok();
    });
    if (!receipt.reach.empty()) {
        std::copy(receipt.reach.begin(), receipt.reach.end(), run.report.last_reach.begin());
    }
    if (const auto *error = std::get_if<Diagnostic>(&result)) {
        stop(run, *error);
        receipt.status = StepStatus::Stopped;
        receipt.cause = run.report.cause;
    }
    return receipt;
}
Result<void> fail_host(RunInstance &run, const Diagnostic &cause) noexcept {
    return protected_call(
        [&]() { return ExecutionAccess::host_stop(run, cause, IntegrationOutcome::Failed); });
}
Result<void> abort_host(RunInstance &run, const Diagnostic &cause) noexcept {
    return protected_call(
        [&]() { return ExecutionAccess::host_stop(run, cause, IntegrationOutcome::Aborted); });
}
Result<void> ExecutionAccess::host_stop(RunInstance &value, const Diagnostic &error,
                                        IntegrationOutcome outcome) {
    if (!value.impl_ || value.impl_->lifecycle == RunLifecycle::Terminal ||
        value.impl_->lifecycle == RunLifecycle::Finalizing) {
        return diagnostic(FailureCode::AlreadyExecuted, "host cannot change a terminal run");
    }
    auto &run = *value.impl_;
    if (run.plan->configuration().run.driver != Driver::Hosted) {
        return diagnostic(FailureCode::UpdateProtocol, "host failure requires hosted driver");
    }
    cause(run, error);
    run.report.integration = outcome;
    if (!run.exhausted && run.report.numerical != NumericalOutcome::Failed) {
        run.report.numerical = NumericalOutcome::Aborted;
    }
    run.lifecycle = RunLifecycle::AwaitingCompletion;
    return ok();
}
Result<RunReport> finish(RunInstance &run, IntegrationCompletion completion) noexcept {
    return protected_call([&]() { return ExecutionAccess::finish(run, std::move(completion)); });
}
Result<RunReport> ExecutionAccess::finish(RunInstance &value, IntegrationCompletion completion) {
    if (!value.impl_ || value.impl_->lifecycle == RunLifecycle::Terminal ||
        value.impl_->lifecycle == RunLifecycle::Finalizing) {
        return diagnostic(FailureCode::AlreadyExecuted, "run already finalized");
    }
    auto &run = *value.impl_;
    if (run.advancing) {
        return diagnostic(FailureCode::UpdateProtocol,
                          "cannot finalize inside an active update callback");
    }
    if (completion.host_contract_id != run.report.host_contract_id ||
        completion.host_contract_version != run.report.host_contract_version ||
        completion.host_experiment_identity != run.report.host_experiment_identity ||
        (completion.kind != IntegrationOutcome::Completed &&
         completion.kind != IntegrationOutcome::Failed &&
         completion.kind != IntegrationOutcome::Aborted)) {
        return diagnostic(FailureCode::InvalidConfig,
                          "completion differs from the declared integration identity");
    }
    if (completion.kind == IntegrationOutcome::Completed && !run.exhausted &&
        run.report.numerical != NumericalOutcome::Failed) {
        return diagnostic(FailureCode::SourceCoverage,
                          "completed host cannot truncate required source scope");
    }
    if (run.report.integration == IntegrationOutcome::Failed ||
        run.report.integration == IntegrationOutcome::Aborted) {
        completion.kind = run.report.integration;
    }
    run.report.integration = completion.kind;
    if (completion.kind != IntegrationOutcome::Completed) {
        cause(run,
              completion.cause.value_or(diagnostic(FailureCode::CallbackException,
                                                   "participating integration failed or aborted")));
        if (!run.exhausted && run.report.numerical != NumericalOutcome::Failed) {
            run.report.numerical = NumericalOutcome::Aborted;
        }
    }
    run.lifecycle = RunLifecycle::Finalizing;
    if (run.exhausted) {
        for (const auto &cursor : run.cached) {
            if (cursor && !cursor->exhausted()) {
                cause(run, diagnostic(FailureCode::SourceCoverage,
                                      "cached outcome stream did not reach exact closure"));
            }
        }
    }
    run.cached.clear();
    run.histories.clear();
    run.kernels.clear();
    run.slots.clear();
    run.source.reset();
    run.services.observers.clear();
    if (auto closed = run.account->close_evidence(); std::holds_alternative<Diagnostic>(closed)) {
        cause(run, std::get<Diagnostic>(closed));
    }
    for (const auto &event : run.account->events()) {
        if (event.kind == RetentionEventKind::OutstandingAtClose) {
            run.report.outstanding_holds.push_back(event);
        }
    }
    if (run.evidence_started) {
        if (auto written = flush(run); std::holds_alternative<Diagnostic>(written)) {
            cause(run, std::get<Diagnostic>(written));
        }
        auto finalized = guarded([&]() { return run.services.evidence.finalize(run.report); });
        if (const auto *error = std::get_if<Diagnostic>(&finalized)) {
            cause(run, *error);
        } else if (run.services.evidence.reference) {
            auto reference = guarded([&]() { return run.services.evidence.reference(); });
            if (const auto *error = std::get_if<Diagnostic>(&reference)) {
                cause(run, *error);
            } else {
                auto value = std::get<EvidenceReference>(std::move(reference));
                if (!value.finalized || value.name != "retention" ||
                    value.absolute_path != run.services.evidence.specification.path ||
                    value.digest == Sha256{}) {
                    cause(run, diagnostic(FailureCode::EvidenceIO,
                                          "required evidence reference is incomplete"));
                } else {
                    run.report.evidence.push_back(std::move(value));
                }
            }
        }
    } else {
        cause(run, diagnostic(FailureCode::EvidenceIO, "required evidence was never opened"));
    }
    auto current = [&](ResourceKind kind) { return run.account->current(kind); };
    auto peak = [&](ResourceKind kind) { return run.account->peak(kind); };
    run.report.current = {current(ResourceKind::Source),    current(ResourceKind::State),
                          current(ResourceKind::Scratch),   current(ResourceKind::Staging),
                          current(ResourceKind::Retention), current(ResourceKind::Graph),
                          current(ResourceKind::Evidence)};
    run.report.peak = {peak(ResourceKind::Source),    peak(ResourceKind::State),
                       peak(ResourceKind::Scratch),   peak(ResourceKind::Staging),
                       peak(ResourceKind::Retention), peak(ResourceKind::Graph),
                       peak(ResourceKind::Evidence)};
    auto finalize_sink = [&](std::size_t i) -> Result<void> {
        return protected_call([&]() -> Result<void> {
            if (run.evidence_closed[i]) {
                return diagnostic(FailureCode::UpdateProtocol, "required sink already finalized");
            }
            run.evidence_closed[i] = true;
            if (!run.evidence_opened[i]) {
                return diagnostic(FailureCode::EvidenceIO, "required sink was never opened");
            }
            auto reference = checked(run.services.required[i].finalize(run.report));
            const auto &spec = run.services.required[i].specification;
            if (reference.name != spec.name || reference.absolute_path != spec.absolute_path ||
                !reference.finalized || reference.digest == Sha256{} ||
                reference.byte_count > spec.max_bytes) {
                return diagnostic(FailureCode::EvidenceIO, "invalid required sink reference");
            }
            run.report.evidence.push_back(std::move(reference));
            return ok();
        });
    };
    for (std::size_t i = 0; i < run.services.required.size(); ++i) {
        if (run.services.required[i].specification.format != "run_snapshot_v1") {
            auto finalized = finalize_sink(i);
            if (const auto *error = std::get_if<Diagnostic>(&finalized)) {
                cause(run, *error);
            }
        }
    }
    auto finalize_snapshot = [&](const StorePublication &summary) -> Result<void> {
        return protected_call([&]() -> Result<void> {
            run.report.cache = summary.status == CachePublicationStatus::Published
                                   ? CachePublicationStatus::Staged
                                   : summary.status;
            run.report.cache_commit_id = summary.commit_id;
            run.report.cache_staged_entries = summary.published_entries;
            run.report.cache_bypassed_entries = summary.bypassed_entries;
            run.report.cache_entries = summary.entries;
            if (run.report.cause) {
                run.report.acceptance = AcceptanceStatus::Rejected;
            }
            for (std::size_t i = 0; i < run.services.required.size(); ++i) {
                if (run.services.required[i].specification.format == "run_snapshot_v1" &&
                    !run.evidence_closed[i]) {
                    checked(finalize_sink(i));
                }
            }
            return ok();
        });
    };
    const auto store = run.plan->store();
    if (store && run.exhausted && run.report.numerical == NumericalOutcome::Completed &&
        run.report.integration == IntegrationOutcome::Completed && !run.report.cause) {
        for (auto &entry : run.staged) {
            if (entry) {
                run.commit_entries.push_back(std::move(*entry));
                entry.reset();
            }
        }
        if (run.commit_entries.empty()) {
            auto finalized =
                finalize_snapshot({{}, CachePublicationStatus::NoNewEntries, 0, 0, {}, {}});
            if (const auto *error = std::get_if<Diagnostic>(&finalized)) {
                cause(run, *error);
                run.report.cache = CachePublicationStatus::NotPublished;
            }
        } else {
            auto prepared = protected_call([&]() -> Result<PreparedCommit> {
                auto commit = checked(store->prepare_commit(std::move(run.commit_entries),
                                                            run.report, finalize_snapshot));
                run.report.cache_commit_id.assign(commit.commit_id());
                run.report.cache_bypassed_entries = commit.bypassed_entries();
                return commit;
            });
            if (const auto *error = std::get_if<Diagnostic>(&prepared)) {
                cause(run, *error);
                run.report.cache = CachePublicationStatus::NotPublished;
            } else {
                auto commit = std::get<PreparedCommit>(std::move(prepared));
                const auto status = commit.status();
                const auto published = commit.published_entries();
                auto outcome = store->publish(std::move(commit));
                if (const auto *accepted = std::get_if<Committed>(&outcome)) {
                    run.report.cache = status;
                    run.report.cache_record_digest = accepted->record_digest;
                    run.report.cache_published_entries = published;
                } else if (const auto *rejected = std::get_if<NotCommitted>(&outcome)) {
                    cause(run, rejected->cause);
                    run.report.cache = CachePublicationStatus::NotPublished;
                    run.report.cache_published_entries = 0;
                } else {
                    cause(run, std::get<CommitUnconfirmed>(outcome).cause);
                    run.report.cache = CachePublicationStatus::Unconfirmed;
                    run.report.acceptance = AcceptanceStatus::Unconfirmed;
                }
            }
        }
    } else {
        auto finalized = finalize_snapshot(
            {{},
             store ? CachePublicationStatus::NotPublished : CachePublicationStatus::Disabled,
             0,
             0,
             {},
             {}});
        if (const auto *error = std::get_if<Diagnostic>(&finalized)) {
            cause(run, *error);
        }
    }
    for (std::size_t i = 0; i < run.services.required.size(); ++i) {
        if (!run.evidence_closed[i]) {
            run.report.acceptance = AcceptanceStatus::Rejected;
            auto finalized = finalize_sink(i);
            if (const auto *error = std::get_if<Diagnostic>(&finalized)) {
                cause(run, *error);
            }
        }
    }
    run.staged.clear();
    run.commit_entries.clear();
    if (store) {
        const auto resources = store->account();
        run.report.store_current = {
            resources->current(ResourceKind::Source),    resources->current(ResourceKind::State),
            resources->current(ResourceKind::Scratch),   resources->current(ResourceKind::Staging),
            resources->current(ResourceKind::Retention), resources->current(ResourceKind::Graph),
            resources->current(ResourceKind::Evidence)};
        run.report.store_peak = {
            resources->peak(ResourceKind::Source),    resources->peak(ResourceKind::State),
            resources->peak(ResourceKind::Scratch),   resources->peak(ResourceKind::Staging),
            resources->peak(ResourceKind::Retention), resources->peak(ResourceKind::Graph),
            resources->peak(ResourceKind::Evidence)};
    }
    if (run.report.acceptance != AcceptanceStatus::Unconfirmed) {
        run.report.acceptance =
            run.exhausted && run.report.numerical == NumericalOutcome::Completed &&
                    run.report.integration == IntegrationOutcome::Completed && !run.report.cause
                ? AcceptanceStatus::Accepted
                : AcceptanceStatus::Rejected;
    }
    run.lifecycle = RunLifecycle::Terminal;
    return std::move(run.report);
}
Result<RunReport> execute(RunInstance &run) noexcept {
    return protected_call([&]() { return ExecutionAccess::execute(run); });
}
Result<RunReport> ExecutionAccess::execute(RunInstance &value) {
    if (!value.impl_ || value.impl_->lifecycle == RunLifecycle::Terminal) {
        return diagnostic(FailureCode::AlreadyExecuted, "run already executed");
    }
    auto &run = *value.impl_;
    if (run.plan->configuration().run.driver != Driver::Standalone ||
        run.lifecycle != RunLifecycle::Created || run.standalone_active) {
        return diagnostic(FailureCode::UpdateProtocol, "execute requires a fresh standalone run");
    }
    run.standalone_active = true;
    for (;;) {
        auto receipt = checked(advance(value, true));
        if (receipt.status == StepStatus::Exhausted || receipt.status == StepStatus::Stopped) {
            break;
        }
    }
    auto completion = std::move(run.services.standalone);
    if (!run.exhausted) {
        completion.kind = IntegrationOutcome::Failed;
        completion.cause = run.report.cause;
    }
    return finish(value, std::move(completion));
}
} // namespace lexongine
