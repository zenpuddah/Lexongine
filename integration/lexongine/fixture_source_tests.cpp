#include "fixture_source.hpp"
#include "fixture.hpp"
#include "../../tests/lexongine_test_support.hpp"

namespace {
using namespace lexongine;
using namespace lexongine::test;
namespace ql = quant::lexongine;
std::shared_ptr<ResourceAccount> account() {
    return take(ResourceAccount::create({{1048576, 1048576, 1048576, 1048576, 1048576, 2097152, 4194304}, 1024, 1024, 16384, 128}));
}
SourceDeclaration declaration() {
    return {"bars", 0, {"qaedalus", "fixture_bars", 1, "fixture-generator-v1"}, "fixture-bars-v1", {"bar-clock-v1", 0, 120},
        "complete_only", "reject", "fail", "interval_end_zero_lag_fixture", MissingIntervalPolicy::Strict, {}, {"test", 1}};
}
struct Setup {
    std::shared_ptr<ResourceAccount> resources = account();
    Registry registry = take(Registry::create());
    ql::ComponentRegistration build{{Sha256{std::byte{1}}, Sha256{std::byte{2}}, Sha256{std::byte{3}}, "fixture-test"}, {}, MissingIntervalPolicy::Strict};
    ql::ComponentTypes types = take(ql::register_components(registry, build));
    SourceResolver resolver = take(SourceResolver::create(resources, 65536, 4));
    SourceValidationServices services{resources, {{120, 120, 0, 120}, 1024, 32768, 131072, 32768}, MissingIntervalPolicy::Strict,
        {1048576, 1048576, 1048576, 1048576, 1048576, 1048576, 1048576}};
    Setup() {
        success(fixture::register_source(registry, resolver, types.bar, build, {120, 65536, 131072, 65536}));
        success(registry.freeze()); success(resolver.freeze());
    }
    SourceSnapshot prepare(SourceDeclaration source) {
        auto request = take(fixture::source_request(source)); auto candidate = take(resolver.inspect(request, services));
        return take(resolver.validate(std::move(candidate), request, services));
    }
};
void complete_fixture_publications() {
    Setup setup; auto snapshot = setup.prepare(declaration()); auto run = account(); PublicationAllocator allocator{run, 0};
    auto cursor = take(snapshot.cursor(allocator));
    auto slot = take(ResultSlot::create(run, {snapshot.output_contract()}, 0, snapshot.metadata().content_digest, snapshot.metadata().source_reference_digest));
    require(snapshot.metadata().coverage.complete_keys.size() == 120 && snapshot.metadata().coverage.gaps.empty(), "fixture coverage changed");
    for (std::uint64_t key = 0; key < 120; ++key) {
        const auto timing = *cursor.descriptor(); UpdateContext current{timing.key, timing.extent, timing.available_at_ns, timing.available_at_ns};
        require(current.key.sequence == key && std::get<Interval>(current.extent).begin_ns == static_cast<std::int64_t>(key) * fixture::interval_ns,
            "fixture source compressed or moved original keys");
        auto event = take(cursor.next(current)); slot->commit(take(validate_pending(std::get<PendingOutcome>(std::move(event)), *slot, current)));
        auto value = take(slot->read<quant::data::Bar>(0, current));
        require(value.value.close.raw() == 100000000000LL + 10000000LL * static_cast<std::int64_t>(key) &&
            value.value.header.sequence == key && value.value.volume.raw() == 1000000 && value.publication().available_at_ns == current.decision_time_ns,
            "typed fixture source changed canonical values or availability");
    }
    require(cursor.descriptor() == nullptr && take(cursor.finish()).completed_scope == declaration().scope, "fixture scope did not close exactly");
}
void fixed_snapshot_scope_and_policy() {
    Setup setup; auto short_source = declaration(); short_source.scope.end = 3; auto first = setup.prepare(short_source);
    short_source.scope.end = 4; auto extended = setup.prepare(short_source);
    require(first.metadata().content_digest == extended.metadata().content_digest && first.metadata().source_reference_digest == extended.metadata().source_reference_digest,
        "processing scope changed fixed snapshot content/reference identity");
    require(first.metadata().validated_scope.end == 3 && extended.metadata().validated_scope.end == 4, "selected coverage was silently widened");
    short_source.scope.begin = 1; auto partial = setup.prepare(short_source);
    auto run = account(); PublicationAllocator allocator{run, 0}; auto cursor = take(partial.cursor(allocator));
    require(cursor.descriptor()->key.sequence == 1 && std::get<CompleteRecord>(cursor.descriptor()->delivery).payload_index == 0, "physical selection index became semantic key");
    auto unsupported = declaration(); unsupported.availability = "event_time";
    rejects(fixture::source_request(unsupported), FailureCode::UnsupportedContract);
    unsupported = declaration(); unsupported.source_version = "unavailable";
    auto request = take(fixture::source_request(unsupported));
    rejects(setup.resolver.inspect(request, setup.services), FailureCode::SourceChanged);
    unsupported = declaration(); unsupported.scope.end = 121; request = take(fixture::source_request(unsupported));
    setup.services.limits.coverage.max_expected_updates = 121;
    rejects(setup.resolver.inspect(request, setup.services), FailureCode::SourceCoverage);
}
}
int main() {
    const Case cases[]{{"120 canonical fixture publications through source service", complete_fixture_publications},
        {"fixed snapshot identity and explicit selected scope/policies", fixed_snapshot_scope_and_policy}};
    return lexongine::test::run(cases);
}
