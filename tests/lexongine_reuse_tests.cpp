#include "lexongine/reuse.hpp"
#include "lexongine/registry.hpp"
#include "lexongine_test_support.hpp"

namespace {
using namespace lexongine;
using namespace lexongine::test;
struct Fixture {
    std::shared_ptr<ResourceAccount> account = take(ResourceAccount::create({{1048576, 1048576, 1048576, 1048576, 1048576, 1048576, 1048576}, 256, 256, 4096, 128}));
    TypeContract semantic{"test.integer", 1, {1, {}}};
    ValueContract type{semantic, typeid(std::int64_t), StorageKind::ScalarCopy, {}};
    std::shared_ptr<ResultSlot> source = take(ResultSlot::create(account, {type}, 0, Sha256{std::byte{1}}, Sha256{std::byte{2}}));
    PublicationHistory history = take(PublicationHistory::create(account, *source, 3));
    std::vector<ParentReference> references;
    UpdateContext context{{"clock", 0}, PointTime{0}, 0, 0};
    PendingOutcome pending(std::int64_t value = 42) {
        auto result = take(PendingOutcome::create(account, context, 1));
        auto payload = take(make_owned<std::int64_t>(account, ResourceKind::Retention, 8, {}, [value] { return value; }));
        success(result.set_output(0, take(ErasedValue::from(std::move(payload), semantic)))); return result;
    }
    void publish(std::uint64_t key, bool warming = false) {
        context = {{"clock", key}, PointTime{static_cast<std::int64_t>(key)}, static_cast<std::int64_t>(key), static_cast<std::int64_t>(key)};
        auto value = pending(); if (warming) { value.warming(1); }
        source->commit(take(validate_pending(std::move(value), *source, context))); success(history.capture(*source));
        if (!warming) { references.push_back({0, take(source->read<std::int64_t>(0, context, 0)).publication()}); }
    }
    std::shared_ptr<ResultSlot> restore(std::span<const ParentReference> parents, std::uint64_t preceding = 3) {
        const std::array<BoundInput, 2> inputs{{{source, 0, 0}, {source, 0, 0}}};
        const std::array<const PublicationHistory*, 2> histories{&history, &history};
        auto output = take(ResultSlot::create(account, {type}, 0, Sha256{std::byte{3}}, std::nullopt)); auto value = pending();
        success(ProvenanceValidator::restore(value, parents, inputs, histories, preceding, context));
        output->commit(take(validate_pending(std::move(value), *output, context))); return output;
    }
};
void exact_new_run_rebinding() {
    Fixture f; for (std::uint64_t key = 10; key < 14; ++key) { f.publish(key); }
    require(f.account->current(ResourceKind::Retention) == 8, "replay history retained old numerical payloads");
    auto parents = f.references; parents[2].input_port = parents[3].input_port = 1;
    auto first = f.restore(parents); parents[1].input_port = 1; auto second = f.restore(parents);
    const auto a = first->current()->parents.entries(), b = second->current()->parents.entries();
    require(a.size() == 4 && b.size() == 4 && a[1].input_port() == 0 && b[1].input_port() == 1 && a[1].metadata() == b[1].metadata(),
        "replay guessed a different grouping for a shared producer");
    require(take(first->read<std::int64_t>(0, f.context)).publication().available_at_ns == 13 && a[0].metadata().available_at_ns == 10,
        "restored history backdated activation availability");
}
void rejected_forged_or_foreign_references() {
    Fixture f; f.publish(0); f.publish(2); const std::array<BoundInput, 1> inputs{{{f.source, 0, 0}}};
    const std::array<const PublicationHistory*, 1> histories{&f.history};
    auto reject = [&](ParentReference parent) {
        auto pending = f.pending(); rejects(ProvenanceValidator::restore(pending, std::span{&parent, 1}, inputs, histories, 1, f.context), FailureCode::CacheCorrupt);
    };
    auto parent = f.references.front(); parent.input_port = 1; reject(parent);
    parent = f.references.front(); parent.publication.port = 1; reject(parent);
    parent = f.references.front(); parent.publication.computation_digest[0] = std::byte{9}; reject(parent);
    parent = f.references.front(); parent.publication.available_at_ns = 1; reject(parent);
    parent = f.references.front(); parent.publication.extent = Interval{0, 1}; reject(parent);
    parent = f.references.front(); parent.publication.source_reference_digest.reset(); reject(parent);
    parent = f.references.front(); parent.publication.key.sequence = 3; reject(parent);
    auto other = take(ResultSlot::create(f.account, {f.type}, 0, Sha256{std::byte{1}}, Sha256{std::byte{2}}));
    other->commit(take(validate_pending(f.pending(), *other, f.context)));
    const std::array<BoundInput, 1> foreign{{{other, 0, 0}}}; auto pending = f.pending();
    rejects(ProvenanceValidator::restore(pending, std::span{&f.references.front(), 1}, foreign, histories, 1, f.context), FailureCode::CacheCorrupt);
}
void history_counts_publications_and_is_bounded() {
    Fixture f; f.publish(0); f.publish(2, true); f.publish(5);
    const std::array<BoundInput, 1> inputs{{{f.source, 0, 0}}}; const std::array<const PublicationHistory*, 1> histories{&f.history};
    { auto pending = f.pending(); success(ProvenanceValidator::restore(pending, std::span{&f.references.front(), 1}, inputs, histories, 1, f.context)); }
    { auto over_history = f.pending(); rejects(ProvenanceValidator::restore(over_history, std::span{&f.references.front(), 1}, inputs, histories, 0, f.context), FailureCode::OutputInvalid); }
    for (std::uint64_t key = 6; key < 10; ++key) { f.publish(key); }
    { auto evicted = f.pending(); rejects(ProvenanceValidator::restore(evicted, std::span{&f.references.front(), 1}, inputs, histories, 10, f.context), FailureCode::CacheCorrupt); }
    require(f.account->current(ResourceKind::Retention) == 8, "metadata ring owns historical payloads");
    rejects(f.history.capture(*f.source), FailureCode::UpdateProtocol);
}
void skipped_capture_and_capacity_fail() {
    Fixture f;
    rejects(PublicationHistory::create(f.account, *f.source, std::numeric_limits<std::uint64_t>::max()), FailureCode::ResourcePlanExceeded);
    rejects(f.history.capture(*f.source), FailureCode::MissingRequired);
    f.publish(0);
    f.context = {{"clock", 1}, PointTime{1}, 1, 1}; f.source->commit(take(validate_pending(f.pending(), *f.source, f.context)));
    f.context = {{"clock", 2}, PointTime{2}, 2, 2}; f.source->commit(take(validate_pending(f.pending(), *f.source, f.context)));
    rejects(f.history.capture(*f.source), FailureCode::UpdateProtocol);
}
}
int main() {
    const Case cases[]{{"exact parent authority rebinding with shared inputs", exact_new_run_rebinding},
        {"forged and foreign cached parent metadata rejects", rejected_forged_or_foreign_references},
        {"publication-count history across warming/gaps and eviction", history_counts_publications_and_is_bounded},
        {"missing capture, repeated capture and capacity bounds", skipped_capture_and_capacity_fail}};
    return lexongine::test::run(cases);
}
