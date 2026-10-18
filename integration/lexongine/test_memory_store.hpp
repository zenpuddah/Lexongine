#pragma once

#include "lexongine/result_store.hpp"
#include "lexongine/execution.hpp"
#include "price_artifact.hpp"
#include "quant/lexongine/components.hpp"
#include "quant/storage/file_integrity.hpp"
#include "../../tests/lexongine_test_support.hpp"

namespace lexongine::test {
namespace memory_detail {
struct Failure { Diagnostic error; };
template<class T> T checked(std::variant<T, Diagnostic> result) {
    if (const auto* error = std::get_if<Diagnostic>(&result)) { throw Failure{*error}; } return std::get<T>(std::move(result));
}
inline void checked(Result<void> result) { if (const auto* error = std::get_if<Diagnostic>(&result)) { throw Failure{*error}; } }
template<class F> auto protect(F&& function) noexcept -> decltype(function()) {
    return guarded([&]() -> decltype(function()) { try { return function(); } catch (const Failure& failure) { return failure.error; } });
}
inline Result<Sha256> digest(std::span<const std::byte> bytes) {
    return guarded([&]() -> Result<Sha256> {
        const auto hex = quant::storage::files::sha256(bytes); Sha256 value{};
        for (std::size_t i = 0; i < value.size(); ++i) { unsigned byte = 0; const auto result = std::from_chars(hex.data() + 2 * i, hex.data() + 2 * i + 2, byte, 16);
            if (result.ec != std::errc{}) { return diagnostic(FailureCode::CacheCorrupt, "test-store digest conversion failed"); } value[i] = static_cast<std::byte>(byte); }
        return value;
    });
}
inline Sha256 digest_text(std::string_view text) { return checked(digest(std::as_bytes(std::span{text.data(), text.size()}))); }
struct Entry {
    Reservation metadata, payload_charge;
    ArtifactIdentity identity;
    SourceCoverage coverage;
    InitializationSpec initialization;
    HistoryRequirement history;
    std::vector<OutputContract> outputs;
    ArtifactOrigin origin;
    std::string payload;
    Sha256 payload_digest;
};
struct Rows {
    Reservation charge;
    ArtifactIdentity identity;
    SourceCoverage coverage;
    InitializationSpec initialization;
    HistoryRequirement history;
    std::vector<OutputContract> outputs;
    std::vector<PriceOutcomeRecord> records;
    std::uint64_t maximum_parents;
    bool sealed = false;
};
struct OriginRecord {
    Reservation charge;
    ArtifactOrigin origin;
    std::vector<std::pair<ArtifactIdentity, Sha256>> entries;
};
struct State {
    Reservation charge;
    std::shared_ptr<ResourceAccount> account;
    LocalCacheConfiguration config;
    RegisteredType<quant::data::Price> price;
    StoreServiceLimits limits;
    std::vector<std::shared_ptr<const Entry>> entries;
    std::vector<std::shared_ptr<const OriginRecord>> origins;
    std::uint64_t next_commit = 1, lookups = 0, publications = 0;
    bool fail_publish = false, lose_acknowledgement = false, unresolved = false;
};
inline std::uint64_t parent_bound(const PreparedPlan& plan, std::uint32_t logical) {
    const auto history = std::get<CountHistory>(plan.nodes()[logical].facts.history).preceding; std::uint64_t count = 0;
    for (const auto& input : plan.nodes()[logical].spec.inputs) { count = checked(checked_add(count, checked(checked_add(std::max(history, input.prior_outputs), 1)))); }
    return count;
}
inline std::uint64_t row_bytes(const PreparedPlan& plan, std::uint32_t logical) {
    const auto parents = parent_bound(plan, logical);
    const auto per_record = checked(checked_add(sizeof(PriceOutcomeRecord) + 512,
        checked(checked_add(checked(checked_multiply(plan.outputs(logical).size(), sizeof(quant::data::Price))),
            checked(checked_multiply(parents, sizeof(ParentReference) + plan.configuration().run.clock.size() + 64))))));
    return checked(checked_add(65536, checked(checked_multiply(plan.source().metadata().coverage.complete_keys.size(), per_record))));
}
inline PricePayloadLimits codec_limits(const State& state) {
    return {state.config.max_entry_bytes, 65536, 1024, state.limits.max_outcomes, state.limits.max_parents, state.config.max_decoded_bytes, 16777216};
}
}
// Test-only in-memory implementation of the local-store port. It retains encoded
// Price bytes and typed origin records; it makes no filesystem/locking claim.
class MemoryStore {
public:
    static MemoryStore make(LocalCacheConfiguration config, RegisteredType<quant::data::Price> price) {
        using namespace memory_detail;
        auto resources = take(ResourceAccount::create({{config.max_store_bytes, 1048576, 16777216, 8388608, 1048576, 16777216, 1048576}, 4096, 1024, 4096, 128}));
        auto charge = take(resources->reserve(ResourceKind::Graph, 131072));
        StoreServiceLimits limits{{1048576, 1024, 32768, 4194304}, 32768, 4096, 4096, 4096, 4096};
        auto state = std::make_shared<State>(State{std::move(charge), resources, config, std::move(price), limits, {}, {}});
        state->entries.reserve(static_cast<std::size_t>(config.max_entries)); state->origins.reserve(static_cast<std::size_t>(config.max_entries));
        ResultStoreCallbacks callbacks;
        callbacks.codec = {"qaedalus.price9_xml", 1, {1, {}}}; callbacks.validation = {"lexongine.count_ready_price", 1, {1, {}}};
        callbacks.supports = [state](const PreparedPlan& plan, std::uint32_t logical) -> Result<bool> {
            const auto& outputs = plan.nodes()[logical].definition->contract().outputs;
            for (std::size_t i = 0; i < outputs.size(); ++i) {
                if (outputs[i].type != state->price.policy.contract || outputs[i].codec != Contract{"qaedalus.price9_xml", 1, {1, {}}} ||
                    outputs[i].validation != Contract{"qaedalus.price9", 1, {1, {}}} || plan.outputs(logical)[i].witness != typeid(quant::data::Price)) { return false; }
            }
            return !outputs.empty();
        };
        callbacks.staging_resources = [state](const PreparedPlan& plan, std::uint32_t logical) -> Result<ResourceBounds> {
            return protect([&]() -> Result<ResourceBounds> {
                const auto count = plan.source().metadata().coverage.complete_keys.size();
                const auto encoded = std::min(state->config.max_entry_bytes, checked(checked_add(4096, checked(checked_multiply(count,
                    checked(checked_add(1024, checked(checked_multiply(parent_bound(plan, logical), 768)))))))));
                return ResourceBounds{encoded, 0, checked(checked_add(65536, checked(checked_multiply(encoded, 3)))), encoded, 0,
                    checked(checked_add(row_bytes(plan, logical), state->limits.metadata_bytes)), state->limits.metadata_bytes};
            });
        };
        callbacks.lookup = [state](const ArtifactIdentity& identity) -> Result<std::variant<CacheMiss, ArtifactInspection>> {
            ++state->lookups;
            for (const auto& entry : state->entries) { if (entry->identity == identity) { return std::variant<CacheMiss, ArtifactInspection>{ArtifactInspection{{}, entry->identity, entry}}; } }
            return std::variant<CacheMiss, ArtifactInspection>{CacheMiss{state->entries.empty() ? CacheMissReason::Absent : CacheMissReason::Incompatible}};
        };
        callbacks.validate = [state](ArtifactInspection&& inspection, const PreparedPlan& plan, std::uint32_t logical) -> Result<ArtifactPayload> {
            return protect([&]() -> Result<ArtifactPayload> {
                const auto entry = std::static_pointer_cast<const Entry>(inspection.storage);
                auto scratch = checked(state->account->reserve(ResourceKind::Scratch, checked(checked_add(4096, checked(checked_multiply(entry->payload.size(), 3))))));
                if (digest_text(entry->payload) != entry->payload_digest) { return diagnostic(FailureCode::CacheCorrupt, "memory payload bytes changed after accepted publication"); }
                const auto origin = std::find_if(state->origins.begin(), state->origins.end(), [&](const auto& value) { return value->origin.commit_id == entry->origin.commit_id; });
                if (origin == state->origins.end() || (*origin)->origin != entry->origin ||
                    std::none_of((*origin)->entries.begin(), (*origin)->entries.end(), [&](const auto& link) { return link.first == entry->identity && link.second == entry->payload_digest; })) {
                    return diagnostic(FailureCode::CacheCorrupt, "memory entry has no matching accepted origin association");
                }
                const auto before = state->account->current(ResourceKind::Graph);
                const PricePayloadContract contract{plan.nodes()[logical].facts.processing_scope, plan.nodes()[logical].definition->contract().outputs,
                    plan.source().metadata().coverage.complete_keys};
                auto decoded = checked(decode_price_payload(entry->payload, contract, state->account, codec_limits(*state)));
                const auto bytes = state->account->current(ResourceKind::Graph) - before;
                auto frozen = std::make_shared<const DecodedPricePayload>(std::move(decoded)); const auto price = state->price;
                const auto ports = entry->outputs.size();
                ArtifactPayload payload{{}, frozen, entry->coverage, entry->initialization, entry->history, entry->origin,
                    frozen->outcomes().size(), bytes, {}, {}};
                payload.record = [frozen](std::uint64_t index) -> Result<ArtifactRecordView> {
                    if (index >= frozen->outcomes().size()) { return diagnostic(FailureCode::CacheCorrupt, "memory artifact record is out of range"); }
                    const auto& row = frozen->outcomes()[static_cast<std::size_t>(index)];
                    return ArtifactRecordView{row.key, row.extent, row.available_at_ns, row.kind, row.remaining_preceding, row.declared_reason, row.parents};
                };
                payload.materialize = [frozen, price, ports](std::uint64_t index, const UpdateContext& context, std::shared_ptr<ResourceAccount> account) -> Result<PendingOutcome> {
                    return protect([&]() -> Result<PendingOutcome> {
                        if (index >= frozen->outcomes().size()) { return diagnostic(FailureCode::CacheCorrupt, "memory artifact materialization is out of range"); }
                        const auto& row = frozen->outcomes()[static_cast<std::size_t>(index)];
                        auto pending = checked(PendingOutcome::create(account, context, ports)); PublicationAllocator allocator{account, 0};
                        for (std::size_t i = 0; i < row.values.size(); ++i) {
                            auto value = checked(allocator.freeze(row.values[i], price));
                            checked(pending.set_output(static_cast<PortIndex>(i), checked(ErasedValue::from(std::move(value), price.policy.contract))));
                        }
                        return pending;
                    });
                };
                return payload;
            });
        };
        callbacks.stage = [state](const ArtifactIdentity& identity, const PreparedPlan& plan, std::uint32_t logical) -> Result<ArtifactWriter> {
            return protect([&]() -> Result<ArtifactWriter> {
                auto charge = checked(state->account->reserve(ResourceKind::Graph, row_bytes(plan, logical)));
                auto rows = std::make_shared<Rows>(Rows{std::move(charge), identity, plan.source().metadata().coverage,
                    plan.nodes()[logical].spec.initialization, plan.nodes()[logical].facts.history,
                    plan.nodes()[logical].definition->contract().outputs, {}, parent_bound(plan, logical)});
                rows->records.reserve(rows->coverage.complete_keys.size());
                return ArtifactWriter{{}, rows, [rows](const OutcomeFrame& frame) -> Result<void> {
                    return protect([&]() -> Result<void> {
                        if (rows->sealed || rows->records.size() == rows->coverage.complete_keys.size() || frame.parents.entries().size() > rows->maximum_parents) {
                            return diagnostic(FailureCode::CacheIO, "memory writer exceeds its admitted stream");
                        }
                        PriceOutcomeRecord row{frame.context.key, frame.context.extent, frame.available_at_ns, frame.kind,
                            frame.remaining_preceding, frame.declared_reason, {}, {}};
                        row.values.reserve(rows->outputs.size()); row.parents.reserve(frame.parents.entries().size());
                        if (frame.kind == OutcomeKind::Published) {
                            for (std::size_t i = 0; i < rows->outputs.size(); ++i) {
                                if (!frame.outputs[i] || frame.outputs[i]->semantic() != rows->outputs[i].type) { return diagnostic(FailureCode::TypeMismatch, "memory writer output type mismatch"); }
                                auto value = checked(frame.outputs[i]->share<quant::data::Price>("cache_writer", "stage_output")); row.values.push_back(value.get());
                            }
                        }
                        for (const auto& parent : frame.parents.entries()) {
                            if (!parent.input_port()) { return diagnostic(FailureCode::OutputInvalid, "memory writer parent has no consuming input"); }
                            row.parents.push_back({*parent.input_port(), parent.metadata()});
                        }
                        rows->records.push_back(std::move(row)); return ok();
                    });
                }};
            });
        };
        callbacks.prepare_commit = [state](std::vector<ArtifactWriter>&& writers, const RunReport& report, const SnapshotFinalizer& finalize) -> Result<StorePublication> {
            return protect([&]() -> Result<StorePublication> {
                std::vector<std::shared_ptr<const Entry>> next = state->entries; next.reserve(static_cast<std::size_t>(state->config.max_entries));
                std::vector<std::shared_ptr<Entry>> created; created.reserve(writers.size()); std::uint64_t bypassed = 0, bytes = 0;
                for (const auto& entry : next) { bytes = checked(checked_add(bytes, checked(checked_add(entry->payload.size(), entry->identity.canonical_bytes().size())))); }
                for (auto& writer : writers) {
                    auto rows = std::static_pointer_cast<Rows>(writer.storage);
                    if (rows->sealed || rows->records.size() != rows->coverage.complete_keys.size()) { return diagnostic(FailureCode::CacheIO, "memory writer is incomplete or already sealed"); }
                    rows->sealed = true;
                    if (std::any_of(next.begin(), next.end(), [&](const auto& entry) { return entry->identity == rows->identity; })) { continue; }
                    if (next.size() >= state->config.max_entries) { ++bypassed; continue; }
                    const PricePayloadContract contract{rows->coverage.expected_scope, rows->outputs, rows->coverage.complete_keys};
                    const auto measured = checked(measure_price_payload(rows->records, contract, state->account, codec_limits(*state)));
                    const auto entry_bytes = checked(checked_add(measured, rows->identity.canonical_bytes().size()));
                    if (entry_bytes > state->config.max_entry_bytes || entry_bytes > state->config.max_store_bytes - bytes) { ++bypassed; continue; }
                    auto encoded = checked(encode_price_payload(rows->records, contract, state->account, codec_limits(*state)));
                    auto metadata = checked(state->account->reserve(ResourceKind::Graph, state->limits.metadata_bytes));
                    auto payload_charge = checked(state->account->reserve(ResourceKind::Source, encoded.bytes().size()));
                    auto entry = std::make_shared<Entry>(Entry{std::move(metadata), std::move(payload_charge), rows->identity,
                        std::move(rows->coverage), std::move(rows->initialization), std::move(rows->history), std::move(rows->outputs),
                        {report.run_id, {}, {}, NumericalOutcome::Completed, IntegrationOutcome::Completed, AcceptanceStatus::Accepted},
                        std::string{encoded.bytes()}, digest_text(encoded.bytes())});
                    bytes += entry_bytes; next.push_back(entry); created.push_back(std::move(entry));
                }
                if (created.empty()) { StorePublication publication{{}, bypassed ? CachePublicationStatus::Bypassed : CachePublicationStatus::NoNewEntries, 0, bypassed, {}, {}};checked(finalize(publication));return publication; }
                if (state->next_commit == std::numeric_limits<std::uint64_t>::max()) { return diagnostic(FailureCode::ResourceExceeded, "memory commit identity exhausted"); }
                const auto commit_id = "memory-" + std::to_string(state->next_commit++);
                auto origin_charge = checked(state->account->reserve(ResourceKind::Graph, state->limits.metadata_bytes));
                auto origin = std::make_shared<OriginRecord>(OriginRecord{std::move(origin_charge),
                    {report.run_id, commit_id, {}, NumericalOutcome::Completed, IntegrationOutcome::Completed, AcceptanceStatus::Accepted}, {}});
                origin->entries.reserve(created.size());
                std::string witness = "lexongine.memory-test-origin.v1:" + report.run_id + ":" + commit_id;
                for (auto& entry : created) {
                    entry->origin.commit_id = commit_id; origin->entries.emplace_back(entry->identity, entry->payload_digest);
                    witness.append(reinterpret_cast<const char*>(entry->identity.digest().data()), entry->identity.digest().size());
                    witness.append(reinterpret_cast<const char*>(entry->payload_digest.data()), entry->payload_digest.size());
                }
                origin->origin.record_digest = digest_text(witness);
                for (auto& entry : created) { entry->origin.record_digest = origin->origin.record_digest; }
                auto origins = state->origins; origins.reserve(static_cast<std::size_t>(state->config.max_entries)); origins.push_back(origin);
                const auto record_digest = origin->origin.record_digest; const auto count = created.size();
                StorePublication publication{{}, CachePublicationStatus::Published, count, bypassed, commit_id,
                    [state, next = std::move(next), origins = std::move(origins), record_digest]() mutable -> CommitOutcome {
                        if (state->fail_publish) { return NotCommitted{diagnostic(FailureCode::CacheIO, "injected memory publication failure")}; }
                        state->entries.swap(next); state->origins.swap(origins); ++state->publications;
                        if (state->lose_acknowledgement) { return CommitUnconfirmed{diagnostic(FailureCode::CommitUnconfirmed, "injected lost memory acknowledgement")}; }
                        return Committed{record_digest};
                    }};
                checked(finalize(publication));return publication;
            });
        };
        callbacks.reconcile = [state](std::string_view commit_id) -> Result<CommitOutcome> {
            if (state->unresolved) { return diagnostic(FailureCode::CacheIO, "injected unresolved reconciliation"); }
            for (const auto& origin : state->origins) { if (origin->origin.commit_id == commit_id) { return CommitOutcome{Committed{origin->origin.record_digest}}; } }
            return CommitOutcome{NotCommitted{diagnostic(FailureCode::CacheIO, "memory marker is absent")}};
        };
        return MemoryStore{take(ResultStore::create(std::move(config), limits, std::move(callbacks), resources, memory_detail::digest)), std::move(state)};
    }
    [[nodiscard]] std::shared_ptr<ResultStore> store() const { return store_; }
    [[nodiscard]] std::size_t entries() const { return state_->entries.size(); }
    [[nodiscard]] std::uint64_t lookups() const { return state_->lookups; }
    [[nodiscard]] std::uint64_t publications() const { return state_->publications; }
    [[nodiscard]] std::vector<std::uint64_t> entry_sizes() const {
        std::vector<std::uint64_t> sizes;
        for (const auto& entry : state_->entries) { sizes.push_back(entry->payload.size() + entry->identity.canonical_bytes().size()); }
        return sizes;
    }
    void fail_publication(bool value) { state_->fail_publish = value; }
    void lose_acknowledgement(bool value, bool unresolved) { state_->lose_acknowledgement = value; state_->unresolved = unresolved; }
    void corrupt_first_payload() {
        using namespace memory_detail;
        require(!state_->entries.empty(), "no memory payload to corrupt"); const auto& old = *state_->entries.front();
        auto metadata = take(state_->account->reserve(ResourceKind::Graph, state_->limits.metadata_bytes));
        auto payload_charge = take(state_->account->reserve(ResourceKind::Source, old.payload.size()));
        auto changed = std::make_shared<Entry>(Entry{std::move(metadata), std::move(payload_charge), old.identity, old.coverage,
            old.initialization, old.history, old.outputs, old.origin, old.payload, old.payload_digest});
        const auto raw = changed->payload.find("raw=\""); require(raw != std::string::npos, "memory payload has no raw field");
        changed->payload[raw + 5] = changed->payload[raw + 5] == '9' ? '8' : '9'; state_->entries.front() = std::move(changed);
    }
private:
    MemoryStore(std::shared_ptr<ResultStore> store, std::shared_ptr<memory_detail::State> state) : store_(std::move(store)), state_(std::move(state)) {}
    std::shared_ptr<ResultStore> store_;
    std::shared_ptr<memory_detail::State> state_;
};
} // namespace lexongine::test
