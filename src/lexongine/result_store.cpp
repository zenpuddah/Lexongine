#include "lexongine/result_store.hpp"
#include "lexongine/execution.hpp"

namespace lexongine {
namespace {
struct StoreFailure { Diagnostic error; };
template<class T> T checked(std::variant<T, Diagnostic> result) {
    if (const auto* error = std::get_if<Diagnostic>(&result)) { throw StoreFailure{*error}; } return std::get<T>(std::move(result));
}
void checked(Result<void> result) { if (const auto* error = std::get_if<Diagnostic>(&result)) { throw StoreFailure{*error}; } }
[[noreturn]] void corrupt(std::string_view message) { throw StoreFailure{diagnostic(FailureCode::CacheCorrupt, message)}; }
template<class F> auto protect(F&& function) noexcept -> decltype(function()) {
    return guarded([&]() -> decltype(function()) { try { return function(); } catch (const StoreFailure& failure) { return failure.error; } });
}
bool same_initialization(const InitializationSpec& a, const InitializationSpec& b) {
    return a.contract == b.contract && a.actual_prefix == b.actual_prefix && a.explicit_values == b.explicit_values;
}
std::uint64_t local_history(const PreparedPlan& plan, std::uint32_t logical) {
    const auto* value = std::get_if<CountHistory>(&plan.nodes()[logical].facts.history);
    if (!value) { corrupt("artifact requires the supported count-history protocol"); } return value->preceding;
}
const TimingDescriptor& complete_descriptor(const PreparedPlan& plan, std::uint64_t index) {
    for (const auto& descriptor : plan.source().timeline()) {
        if (std::holds_alternative<CompleteRecord>(descriptor.delivery)) { if (index == 0) { return descriptor; } --index; }
    }
    corrupt("artifact references a missing complete source update");
}
std::int64_t decision_at(const PreparedPlan& plan, const UpdateKey& key) {
    auto decision = std::numeric_limits<std::int64_t>::min();
    for (const auto& descriptor : plan.source().timeline()) {
        decision = std::max(decision, descriptor.available_at_ns); if (descriptor.key == key) { return decision; }
    }
    corrupt("artifact activation is outside the validated source timeline");
}
void check_record(const PreparedPlan& plan, std::uint32_t logical, std::uint64_t index,
    const ArtifactRecordView& record, const StoreServiceLimits& limits) {
    const auto& descriptor = complete_descriptor(plan, index);
    if (record.key != descriptor.key || record.extent != descriptor.extent || record.available_at_ns < descriptor.available_at_ns ||
        record.available_at_ns > decision_at(plan, descriptor.key) || record.parents.size() > limits.max_parents) { corrupt("artifact outcome timing, coverage or metadata bound differs from the source"); }
    const auto ready = plan.preceding(logical);
    if (index < ready) {
        if (record.kind != OutcomeKind::Warming || record.remaining_preceding != ready - index || record.declared_reason != 0) { corrupt("artifact startup differs from declared history"); }
    } else if (record.kind != OutcomeKind::Published || record.remaining_preceding != 0 || record.declared_reason != 0) { corrupt("artifact contains an unsupported post-startup outcome"); }
    const auto inputs = plan.graph().inputs(logical);
    bool waiting = false;
    for (const auto& input : inputs) { waiting |= index < plan.preceding(input.producer); }
    if (waiting) {
        if (record.kind != OutcomeKind::Warming || !record.parents.empty()) { corrupt("dependency-waiting artifact fabricated input publications"); }
        return;
    }
    const auto& keys = plan.source().metadata().coverage.complete_keys;
    std::size_t cursor = 0;
    for (std::size_t input = 0; input < inputs.size(); ++input) {
        const auto first = cursor; const auto& edge = inputs[input]; std::optional<std::uint64_t> previous;
        while (cursor < record.parents.size() && record.parents[cursor].input_port == input) {
            const auto& parent = record.parents[cursor++].publication;
            const auto key = std::lower_bound(keys.begin(), keys.end(), parent.key.sequence, [](const auto& value, auto sequence) { return value.sequence < sequence; });
            if (key == keys.end() || *key != parent.key) { corrupt("artifact parent is not a complete source-clock update"); }
            const auto parent_index = static_cast<std::uint64_t>(key - keys.begin());
            const auto expected_reference = edge.producer == 0 ? std::optional<Sha256>{plan.source().metadata().source_reference_digest} : std::nullopt;
            if (parent.computation_digest != plan.identity(edge.producer) || parent.port != edge.output || parent.source_reference_digest != expected_reference ||
                parent_index > index || parent_index < plan.preceding(edge.producer) ||
                index - parent_index > std::max(local_history(plan, logical), plan.nodes()[logical].spec.inputs[input].prior_outputs) ||
                (previous && parent_index <= *previous)) { corrupt("artifact parent violates declared input identity or publication history"); }
            const auto& original = complete_descriptor(plan, parent_index);
            if (parent.extent != original.extent || parent.available_at_ns < original.available_at_ns ||
                parent.available_at_ns > decision_at(plan, parent.key) || parent.available_at_ns > record.available_at_ns) { corrupt("artifact parent temporal provenance is invalid"); }
            if (record.kind == OutcomeKind::Warming && parent.key != record.key) { corrupt("own-startup warming must retain current input provenance"); }
            previous = parent_index;
        }
        if (cursor == first || (record.kind == OutcomeKind::Warming && cursor - first != 1)) { corrupt("artifact omitted or reordered a required parent input"); }
    }
    if (cursor != record.parents.size()) { corrupt("artifact parent names an undeclared consuming input"); }
}
Diagnostic invalid_payload(Diagnostic error) {
    if (error.code != FailureCode::ResourceExceeded && error.code != FailureCode::ResourcePlanExceeded && error.code != FailureCode::EvidenceQuota && error.code != FailureCode::CallbackException) {
        error.code = FailureCode::CacheCorrupt;
    }
    return error;
}
}
struct ResultStore::Data {
    Reservation charge;
    std::shared_ptr<ResourceAccount> account;
    LocalCacheConfiguration configuration;
    StoreServiceLimits limits;
    ResultStoreCallbacks callbacks;
    DigestFunction digest;
};
struct ArtifactCandidate::Data { std::shared_ptr<const void> owner; Reservation charge; ArtifactInspection inspection; };
struct ValidatedArtifact::Data {
    std::shared_ptr<const void> owner;
    Reservation charge;
    ArtifactIdentity identity;
    StoreServiceLimits limits;
    ArtifactPayload payload;
};
struct ArtifactCursor::Data {
    Reservation charge;
    std::shared_ptr<const ValidatedArtifact::Data> artifact;
    std::shared_ptr<const PreparedPlan> plan;
    std::uint32_t logical;
    std::shared_ptr<ResourceAccount> account;
    std::vector<BoundInput> inputs;
    std::vector<std::shared_ptr<PublicationHistory>> histories;
    std::vector<const PublicationHistory*> history_views;
    std::uint64_t position = 0;
    std::optional<Diagnostic> failure;
};
struct StagedEntry::Data {
    std::shared_ptr<const void> owner;
    Reservation charge;
    std::shared_ptr<const PreparedPlan> plan;
    std::uint32_t logical;
    ArtifactWriter writer;
    std::uint64_t position = 0;
    std::optional<Diagnostic> failure;
};
struct PreparedCommit::Data { std::shared_ptr<const void> owner; Reservation charge; StorePublication publication; };
ArtifactCandidate::ArtifactCandidate(std::unique_ptr<Data> data) noexcept : data_(std::move(data)) {}
ArtifactCandidate::ArtifactCandidate(ArtifactCandidate&&) noexcept = default;
ArtifactCandidate& ArtifactCandidate::operator=(ArtifactCandidate&&) noexcept = default;
ArtifactCandidate::~ArtifactCandidate() noexcept = default;
ArtifactCursor::ArtifactCursor(std::unique_ptr<Data> data) noexcept : data_(std::move(data)) {}
ArtifactCursor::ArtifactCursor(ArtifactCursor&&) noexcept = default;
ArtifactCursor& ArtifactCursor::operator=(ArtifactCursor&&) noexcept = default;
ArtifactCursor::~ArtifactCursor() noexcept = default;
StagedEntry::StagedEntry(std::unique_ptr<Data> data) noexcept : data_(std::move(data)) {}
StagedEntry::StagedEntry(StagedEntry&&) noexcept = default;
StagedEntry& StagedEntry::operator=(StagedEntry&&) noexcept = default;
StagedEntry::~StagedEntry() noexcept = default;
PreparedCommit::PreparedCommit(std::unique_ptr<Data> data) noexcept : data_(std::move(data)) {}
PreparedCommit::PreparedCommit(PreparedCommit&&) noexcept = default;
PreparedCommit& PreparedCommit::operator=(PreparedCommit&&) noexcept = default;
PreparedCommit::~PreparedCommit() noexcept = default;
CachePublicationStatus PreparedCommit::status() const noexcept { return data_->publication.status; }
std::uint64_t PreparedCommit::published_entries() const noexcept { return data_->publication.published_entries; }
std::uint64_t PreparedCommit::bypassed_entries() const noexcept { return data_->publication.bypassed_entries; }
std::string_view PreparedCommit::commit_id() const noexcept { return data_->publication.commit_id; }
const ArtifactOrigin& ValidatedArtifact::origin() const noexcept { return data_->payload.origin; }
const ArtifactIdentity& ValidatedArtifact::identity() const noexcept { return data_->identity; }
std::uint64_t ValidatedArtifact::decoded_bytes() const noexcept { return data_->payload.decoded_bytes; }
const LocalCacheConfiguration& ResultStore::configuration() const noexcept { return data_->configuration; }
const StoreServiceLimits& ResultStore::limits() const noexcept { return data_->limits; }
std::shared_ptr<ResourceAccount> ResultStore::account() const noexcept { return data_->account; }
Result<std::shared_ptr<ResultStore>> ResultStore::create(LocalCacheConfiguration configuration, StoreServiceLimits limits,
    ResultStoreCallbacks callbacks, std::shared_ptr<ResourceAccount> account, DigestFunction digest) noexcept {
    return protect([&]() -> Result<std::shared_ptr<ResultStore>> {
        if (!account || !digest || !callbacks.supports || !callbacks.staging_resources || !callbacks.lookup || !callbacks.validate || !callbacks.stage || !callbacks.prepare_commit || !callbacks.reconcile ||
            configuration.mode != "automatic_read_write" || configuration.backend != "local_files" || !configuration.directory.starts_with('/') ||
            configuration.matching != "exact_full_scope" || configuration.trust != "same_build_local_writer" || configuration.integrity != "sha256" ||
            configuration.writer != "single_process_lock" || configuration.corruption != "fail_preparation" || configuration.capacity != "bypass_new_entries" || configuration.cleanup != "explicit_only" ||
            !configuration.max_entries || !configuration.max_store_bytes || !configuration.max_entry_bytes || !configuration.max_decoded_bytes ||
            limits.metadata_bytes < sizeof(Data) + 8192 || limits.cursor_bytes < sizeof(ArtifactCursor::Data) + 1024 || limits.writer_bytes < sizeof(StagedEntry::Data) + 1024 || !limits.max_outcomes) {
            return diagnostic(FailureCode::InvalidConfig, "incomplete explicit result-store contract or services");
        }
        checked(validate_text(configuration.directory, limits.identity.string_bytes, false));
        checked(validate_text(callbacks.codec.id, limits.identity.string_bytes, false)); checked(validate_text(callbacks.validation.id, limits.identity.string_bytes, false));
        if (!callbacks.codec.version || !callbacks.validation.version) { return diagnostic(FailureCode::InvalidConfig, "store codec/validation versions must be explicit"); }
        auto charge = checked(account->reserve(ResourceKind::Graph, limits.metadata_bytes));
        auto data = std::make_shared<Data>(Data{std::move(charge), std::move(account), std::move(configuration), limits, std::move(callbacks), digest});
        return std::shared_ptr<ResultStore>(new ResultStore{std::move(data)});
    });
}
Result<bool> ResultStore::supports(const PreparedPlan& plan, std::uint32_t logical) const noexcept {
    return protect([&]() -> Result<bool> {
        if (logical >= plan.nodes().size()) { return diagnostic(FailureCode::InvalidConfig, "store eligibility references an undeclared producer"); }
        if (logical == 0 || !plan.needed(logical) || !plan.graph().shareable(logical) || !plan.kernel(logical) || !plan.kernel(logical)->has_output_validation()) { return false; }
        const auto& definition = plan.nodes()[logical].definition->contract();
        if (!definition.sharing.cacheable || !definition.sharing.replay_safe || !definition.sharing.effect_free ||
            std::any_of(definition.outputs.begin(), definition.outputs.end(), [](const auto& output) { return !output.cacheable; })) { return false; }
        return data_->callbacks.supports(plan, logical);
    });
}
Result<ResourceBounds> ResultStore::staging_resources(const PreparedPlan& plan, std::uint32_t logical) const noexcept {
    return protect([&]() -> Result<ResourceBounds> { auto bounds = checked(data_->callbacks.staging_resources(plan, logical)); static_cast<void>(checked(bounds.total())); return bounds; });
}
Result<ArtifactIdentity> ResultStore::identity(const PreparedPlan& plan, std::uint32_t logical) const noexcept {
    return ArtifactIdentity::create(plan, logical, data_->callbacks.codec, data_->callbacks.validation, data_->digest, data_->account, data_->limits.identity);
}
Result<LookupDecision> ResultStore::lookup(const ArtifactIdentity& identity) noexcept {
    return protect([&]() -> Result<LookupDecision> {
        if (identity.codec() != data_->callbacks.codec || identity.validation() != data_->callbacks.validation) { return diagnostic(FailureCode::UnsupportedContract, "artifact identity differs from store codec contracts"); }
        auto charge = checked(data_->account->reserve(ResourceKind::Graph, data_->limits.metadata_bytes));
        auto found = checked(data_->callbacks.lookup(identity));
        if (const auto* miss = std::get_if<CacheMiss>(&found)) { return LookupDecision{*miss}; }
        auto inspection = std::get<ArtifactInspection>(std::move(found));
        if (!(inspection.identity == identity)) { return LookupDecision{CacheMiss{CacheMissReason::Incompatible}}; }
        if (!inspection.storage) { corrupt("store returned an empty artifact candidate"); }
        return LookupDecision{ArtifactCandidate{std::make_unique<ArtifactCandidate::Data>(ArtifactCandidate::Data{data_, std::move(charge), std::move(inspection)})}};
    });
}
Result<ValidatedArtifact> ResultStore::validate(ArtifactCandidate&& candidate, const ArtifactIdentity& expected,
    const PreparedPlan& plan, std::uint32_t logical) noexcept {
    return protect([&]() -> Result<ValidatedArtifact> {
        if (!candidate.data_ || candidate.data_->owner.get() != data_.get() || logical >= plan.nodes().size()) { return diagnostic(FailureCode::InvalidConfig, "artifact candidate is consumed or belongs to another store"); }
        if (!(candidate.data_->inspection.identity == expected) || !(checked(identity(plan, logical)) == expected) || !checked(supports(plan, logical))) {
            return diagnostic(FailureCode::InvalidConfig, "candidate validation differs from the eligible prepared computation");
        }
        auto owned = std::move(candidate.data_);
        auto payload = checked(data_->callbacks.validate(std::move(owned->inspection), plan, logical));
        const auto* history = std::get_if<CountHistory>(&payload.history);
        if (!payload.snapshot || !payload.record || !payload.materialize || !history || history->preceding != local_history(plan, logical) ||
            payload.coverage != plan.source().metadata().coverage || !same_initialization(payload.initialization, plan.nodes()[logical].spec.initialization) ||
            payload.outcome_count != payload.coverage.complete_keys.size() || payload.outcome_count > data_->limits.max_outcomes) { corrupt("artifact metadata, startup or complete coverage mismatch"); }
        if (payload.decoded_bytes > data_->configuration.max_decoded_bytes) { return diagnostic(FailureCode::ResourcePlanExceeded, "artifact decoded memory exceeds the configured bound"); }
        checked(validate_text(payload.origin.run_id, data_->limits.identity.string_bytes, false)); checked(validate_text(payload.origin.commit_id, data_->limits.identity.string_bytes, false));
        if (payload.origin.record_digest == Sha256{} || payload.origin.numerical != NumericalOutcome::Completed || payload.origin.integration != IntegrationOutcome::Completed ||
            payload.origin.acceptance != AcceptanceStatus::Accepted) { corrupt("artifact has no validated accepted originating run"); }
        for (std::uint64_t i = 0; i < payload.outcome_count; ++i) {
            const auto record = checked(payload.record(i)); check_record(plan, logical, i, record, data_->limits);
            if (record.kind != OutcomeKind::Published) { continue; }
            const auto& descriptor = complete_descriptor(plan, i);
            const UpdateContext context{descriptor.key, descriptor.extent, decision_at(plan, descriptor.key), descriptor.available_at_ns};
            auto pending = checked(payload.materialize(i, context, data_->account));
            if (!pending.frame_) { corrupt("artifact materialization returned an empty bundle"); }
            pending.frame_->available_at_ns = record.available_at_ns;
            auto valid = plan.kernel(logical)->validate_outputs(pending, context);
            if (const auto* error = std::get_if<Diagnostic>(&valid)) { return invalid_payload(*error); }
        }
        return ValidatedArtifact{std::make_shared<const ValidatedArtifact::Data>(ValidatedArtifact::Data{data_, std::move(owned->charge), expected, data_->limits, std::move(payload)})};
    });
}
Result<ArtifactCursor> ValidatedArtifact::cursor(std::shared_ptr<const PreparedPlan> plan, std::uint32_t logical,
    std::shared_ptr<ResourceAccount> account, std::vector<BoundInput> inputs, std::vector<std::shared_ptr<PublicationHistory>> histories) const noexcept {
    return protect([&]() -> Result<ArtifactCursor> {
        if (!plan || !plan->admitted() || !account || logical >= plan->nodes().size() || !plan->kernel(logical) || !data_->identity.matches(*plan, logical) ||
            inputs.size() != plan->graph().inputs(logical).size() || histories.size() != inputs.size()) {
            return diagnostic(FailureCode::InvalidConfig, "artifact cursor has incomplete run bindings");
        }
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const auto& input = inputs[i]; const auto edge = plan->graph().inputs(logical)[i];
            if (!input.slot || input.port != edge.output || input.port >= input.slot->contracts().size() ||
                input.prior_outputs != plan->nodes()[logical].spec.inputs[i].prior_outputs ||
                input.slot->contracts()[input.port].semantic != plan->outputs(edge.producer)[edge.output].semantic ||
                input.slot->contracts()[input.port].witness != plan->outputs(edge.producer)[edge.output].witness) {
                return diagnostic(FailureCode::TypeMismatch, "artifact cursor bound input differs from its prepared port");
            }
        }
        const auto bytes = checked(checked_add(sizeof(ArtifactCursor::Data) + 1024,
            checked(checked_multiply(inputs.size(), sizeof(BoundInput) + sizeof(std::shared_ptr<PublicationHistory>) + sizeof(const PublicationHistory*)))));
        if (bytes > data_->limits.cursor_bytes) { return diagnostic(FailureCode::ResourcePlanExceeded, "artifact cursor exceeds explicit metadata capacity"); }
        auto charge = checked(account->reserve(ResourceKind::State, data_->limits.cursor_bytes));
        auto cursor = std::make_unique<ArtifactCursor::Data>(ArtifactCursor::Data{std::move(charge), data_, std::move(plan), logical, std::move(account), std::move(inputs), std::move(histories), {}, 0, {}});
        cursor->history_views.reserve(cursor->histories.size());
        for (const auto& history : cursor->histories) { if (!history) { return diagnostic(FailureCode::InvalidConfig, "missing replay publication history"); } cursor->history_views.push_back(history.get()); }
        return ArtifactCursor{std::move(cursor)};
    });
}
bool ArtifactCursor::exhausted() const noexcept { return data_ && !data_->failure && data_->position == data_->artifact->payload.outcome_count; }
Result<PendingOutcome> ArtifactCursor::next(const UpdateContext& context) noexcept {
    if (!data_ || data_->position == data_->artifact->payload.outcome_count) { return diagnostic(FailureCode::UpdateProtocol, "artifact cursor is exhausted"); }
    auto& data = *data_; if (data.failure) { return *data.failure; }
    if (auto valid = validate_context(context); std::holds_alternative<Diagnostic>(valid)) { return std::get<Diagnostic>(valid); }
    auto inspected = guarded([&]() { return data.artifact->payload.record(data.position); });
    if (const auto* error = std::get_if<Diagnostic>(&inspected)) { data.failure = *error; return *error; }
    const auto record = std::get<ArtifactRecordView>(inspected);
    const auto timeline = data.plan->source().timeline();
    const auto activation = std::find_if(timeline.begin(), timeline.end(), [&](const auto& descriptor) { return descriptor.key == record.key && std::holds_alternative<CompleteRecord>(descriptor.delivery); });
    if (activation == timeline.end()) { data.failure = diagnostic(FailureCode::CacheCorrupt, "cached record no longer matches validated source timing"); return *data.failure; }
    if (record.key != context.key || record.extent != context.extent || activation->available_at_ns != context.activation_available_at_ns) {
        return diagnostic(FailureCode::UpdateProtocol, "artifact cursor activation differs from its next outcome");
    }
    if (record.available_at_ns > context.decision_time_ns) { return diagnostic(FailureCode::FutureRead, "cached outcome is not yet available"); }
    auto result = protect([&]() -> Result<PendingOutcome> {
        auto pending = checked(data.artifact->payload.materialize(data.position, context, data.account));
        if (!pending.frame_) { corrupt("cached materialization returned no staged bundle"); }
        bool waiting = false; auto available = context.activation_available_at_ns;
        for (const auto& input : data.inputs) {
            if (!input.slot || !input.slot->current() || input.slot->current()->context.key != context.key || input.slot->current()->context.extent != context.extent) { corrupt("cached input lacks a matching current outcome"); }
            const auto& frame = *input.slot->current(); available = std::max(available, frame.available_at_ns);
            if (frame.kind == OutcomeKind::Warming) { waiting = true; }
            else if (frame.kind != OutcomeKind::Published) { return diagnostic(FailureCode::MissingRequired, "cached consumer requires a published input"); }
        }
        if (waiting) {
            if (record.kind != OutcomeKind::Warming || !record.parents.empty()) { corrupt("cached dependency-waiting metadata mismatch"); }
            checked(pending.waiting(record.remaining_preceding, available));
        } else {
            checked(ProvenanceValidator::restore(pending, record.parents, data.inputs, data.history_views, local_history(*data.plan, data.logical), context));
            if (record.kind == OutcomeKind::Warming) { pending.warming(record.remaining_preceding); }
            else { checked(data.plan->kernel(data.logical)->validate_outputs(pending, context)); }
        }
        if (pending.frame_->available_at_ns != record.available_at_ns || pending.frame_->kind != record.kind) { corrupt("cached outcome changed its original causal boundary"); }
        return pending;
    });
    if (const auto* error = std::get_if<Diagnostic>(&result)) { data.failure = *error; }
    else { ++data.position; }
    return result;
}
Result<StagedEntry> ResultStore::stage(const ArtifactIdentity& identity, std::shared_ptr<const PreparedPlan> plan, std::uint32_t logical) noexcept {
    return protect([&]() -> Result<StagedEntry> {
        if (!plan || !plan->admitted() || !checked(supports(*plan, logical)) || !(checked(this->identity(*plan, logical)) == identity)) { return diagnostic(FailureCode::InvalidConfig, "staging differs from the eligible prepared computation"); }
        auto charge = checked(data_->account->reserve(ResourceKind::Graph, data_->limits.writer_bytes));
        auto writer = checked(data_->callbacks.stage(identity, *plan, logical));
        if (!writer.storage || !writer.append) { return diagnostic(FailureCode::InvalidConfig, "store returned an incomplete private writer"); }
        return StagedEntry{std::make_unique<StagedEntry::Data>(StagedEntry::Data{data_, std::move(charge), std::move(plan), logical, std::move(writer), 0, {}})};
    });
}
Result<void> StagedEntry::append(const OutcomeFrame& frame) noexcept {
    if (!data_) { return diagnostic(FailureCode::UpdateProtocol, "staged entry is consumed"); }
    auto& data = *data_; if (data.failure) { return *data.failure; }
    if (data.position == data.plan->source().metadata().coverage.complete_keys.size()) { return diagnostic(FailureCode::UpdateProtocol, "staged entry has reached exact scope closure"); }
    auto result = protect([&]() -> Result<void> {
        const auto& descriptor = complete_descriptor(*data.plan, data.position);
        if (frame.context.key != descriptor.key || frame.context.extent != descriptor.extent || frame.available_at_ns < descriptor.available_at_ns || frame.available_at_ns > frame.context.decision_time_ns) {
            return diagnostic(FailureCode::OutputInvalid, "staged artifact frame differs from its complete-update coverage");
        }
        if ((data.position < data.plan->preceding(data.logical)) != (frame.kind == OutcomeKind::Warming) ||
            (frame.kind != OutcomeKind::Published && frame.kind != OutcomeKind::Warming)) { return diagnostic(FailureCode::OutputInvalid, "staged artifact violates count-based startup"); }
        checked(data.writer.append(frame)); ++data.position; return ok();
    });
    if (const auto* error = std::get_if<Diagnostic>(&result)) { data.failure = *error; } return result;
}
Result<PreparedCommit> ResultStore::prepare_commit(std::vector<StagedEntry>&& entries, const RunReport& report, SnapshotFinalizer finalize) noexcept {
    return protect([&]() -> Result<PreparedCommit> {
        if (report.numerical != NumericalOutcome::Completed || report.integration != IntegrationOutcome::Completed || report.acceptance != AcceptanceStatus::Pending || report.cause) {
            return diagnostic(FailureCode::UpdateProtocol, "store finalization requires completed precommit engine/host/evidence");
        }
        auto bytes = checked(checked_add(sizeof(PreparedCommit::Data) + 1024, checked(checked_multiply(entries.size(), sizeof(ArtifactWriter)))));
        if (bytes > data_->limits.metadata_bytes) { return diagnostic(FailureCode::ResourcePlanExceeded, "commit metadata exceeds explicit capacity"); }
        auto charge = checked(data_->account->reserve(ResourceKind::Evidence, data_->limits.metadata_bytes));
        std::vector<ArtifactWriter> writers; writers.reserve(entries.size());
        for (auto& entry : entries) {
            if (!entry.data_ || entry.data_->owner.get() != data_.get() || entry.data_->failure ||
                entry.data_->position != entry.data_->plan->source().metadata().coverage.complete_keys.size()) { return diagnostic(FailureCode::UpdateProtocol, "cannot finalize foreign, failed or incomplete staged entries"); }
        }
        for (auto& entry : entries) { writers.push_back(std::move(entry.data_->writer)); entry.data_.reset(); }
        bool finalized=false;std::optional<StorePublication> summary;
        SnapshotFinalizer bound=[&](const StorePublication& proposed)->Result<void>{return protect([&]()->Result<void>{
            if(finalized){return diagnostic(FailureCode::UpdateProtocol,"snapshot finalizer is single-use");}
            if(proposed.published_entries>entries.size()||proposed.bypassed_entries>entries.size()-proposed.published_entries){return diagnostic(FailureCode::InvalidConfig,"publication summary exceeds staged entries");}
            checked(validate_text(proposed.commit_id,data_->limits.identity.string_bytes,true));
            summary=StorePublication{{},proposed.status,proposed.published_entries,proposed.bypassed_entries,proposed.commit_id,{},proposed.entries};
            if(finalize){checked(finalize(*summary));}finalized=true;return ok();
        });};
        auto publication = checked(data_->callbacks.prepare_commit(std::move(writers), report, bound));
        if(!finalized||!summary||summary->status!=publication.status||summary->commit_id!=publication.commit_id||summary->entries!=publication.entries||summary->published_entries!=publication.published_entries||summary->bypassed_entries!=publication.bypassed_entries){return diagnostic(FailureCode::EvidenceIO,"store failed to finalize the exact precommit snapshot summary");}
        if (publication.published_entries > entries.size() || publication.bypassed_entries > entries.size() - publication.published_entries) {
            return diagnostic(FailureCode::InvalidConfig, "store publication counts exceed staged entries");
        }
        if (publication.status == CachePublicationStatus::Published) {
            checked(validate_text(publication.commit_id, data_->limits.identity.string_bytes, false));
            if (!publication.publish || !publication.published_entries) { return diagnostic(FailureCode::InvalidConfig, "prepared publication is missing its final authority"); }
        } else if ((publication.status != CachePublicationStatus::NoNewEntries && publication.status != CachePublicationStatus::Bypassed) ||
            publication.publish || !publication.commit_id.empty() || publication.published_entries != 0) { return diagnostic(FailureCode::InvalidConfig, "no-entry finalization must not publish a marker"); }
        return PreparedCommit{std::make_unique<PreparedCommit::Data>(PreparedCommit::Data{data_, std::move(charge), std::move(publication)})};
    });
}
CommitOutcome ResultStore::publish(PreparedCommit&& prepared) noexcept {
    if (!prepared.data_ || prepared.data_->owner.get() != data_.get()) { return NotCommitted{diagnostic(FailureCode::UpdateProtocol, "prepared commit is consumed or belongs to another store")}; }
    auto owned = std::move(prepared.data_);
    if (owned->publication.status != CachePublicationStatus::Published) { return Committed{Sha256{}}; }
    auto result = guarded([&]() -> Result<CommitOutcome> { return owned->publication.publish(); });
    CommitOutcome outcome = std::holds_alternative<Diagnostic>(result)
        ? CommitOutcome{CommitUnconfirmed{diagnostic(FailureCode::CommitUnconfirmed, "publication callback failed with unresolved visibility")}}
        : std::get<CommitOutcome>(std::move(result));
    if (const auto* committed = std::get_if<Committed>(&outcome); committed && committed->record_digest == Sha256{}) {
        outcome = CommitUnconfirmed{diagnostic(FailureCode::CommitUnconfirmed, "publication returned no authoritative record digest")};
    }
    if (std::holds_alternative<CommitUnconfirmed>(outcome)) {
        auto reconciled = guarded([&]() { return data_->callbacks.reconcile(owned->publication.commit_id); });
        if (const auto* resolved = std::get_if<CommitOutcome>(&reconciled)) {
            if (const auto* committed = std::get_if<Committed>(resolved); committed && committed->record_digest == Sha256{}) { return outcome; }
            return *resolved;
        }
    }
    return outcome;
}
} // namespace lexongine
