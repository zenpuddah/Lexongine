#include "lexongine/source.hpp"
#include "lexongine_test_support.hpp"

namespace lexongine {
template <> struct PublicationTraits<std::int64_t> {
    using type = std::int64_t;
    static constexpr auto storage = StorageKind::ScalarCopy;
};
} // namespace lexongine
namespace {
using namespace lexongine;
using namespace lexongine::test;
struct Counters {
    std::size_t inspected = 0, validated = 0, constructed = 0, delivered = 0, closed = 0;
};
struct Fixture {
    SourceMetadata metadata;
    std::vector<TimingDescriptor> timeline;
    std::vector<std::int64_t> values;
};
struct Data {
    Reservation charge;
    Fixture fixture;
};
struct Cursor {
    std::size_t position;
};
using Provider = TypedSourceProvider<std::int64_t, std::shared_ptr<const Fixture>, Data, Cursor>;
std::shared_ptr<ResourceAccount> account() {
    return take(ResourceAccount::create(
        {{1048576, 1048576, 1048576, 1048576, 1048576, 1048576, 1048576}, 128, 128, 4096, 128}));
}
TypeContract type() {
    return {"test.source_value", 1, {1, {}}};
}
Contract contract(std::string name) {
    return {std::move(name), 1, {1, {}}};
}
SourceRequest request() {
    return {"test", "fixture", "v1", type(), {"clock", 0, 2}, {1, {}}};
}
SourceValidationServices services(std::shared_ptr<ResourceAccount> resources,
                                  MissingIntervalPolicy missing = MissingIntervalPolicy::Strict) {
    return {std::move(resources),
            {{16, 16, 16, 16}, 1024, 16384, 32768, 16384},
            missing,
            {65536, 65536, 65536, 65536, 65536, 65536, 65536}};
}
std::shared_ptr<Fixture> fixture() {
    return std::make_shared<Fixture>(
        Fixture{{"fixture",
                 "v1",
                 Sha256{std::byte{1}},
                 type(),
                 {{}, typeid(std::int64_t)},
                 {"clock", 0, 2},
                 {{"clock", 0, 2}, {{"clock", 0}, {"clock", 1}}, {}, contract("test.succession")},
                 contract("point_time"),
                 contract("test.order"),
                 contract("test.availability"),
                 contract("test.validation"),
                 Sha256{std::byte{2}},
                 {Sha256{std::byte{3}}, Sha256{std::byte{4}}, Sha256{std::byte{5}}, "test-build"}},
                {{{"clock", 0}, PointTime{20}, 30, CompleteRecord{0}},
                 {{"clock", 1}, PointTime{19}, 40, CompleteRecord{1}}},
                {7, 8}});
}
Provider provider(std::shared_ptr<Fixture> input, std::shared_ptr<Counters> calls,
                  ModulePin pin = {}) {
    auto registry = take(Registry::create());
    auto output = take(registry.register_type<std::int64_t>(
        {type(), StorageKind::ScalarCopy,
         [](const auto &value) -> Result<void> {
             return value < 0 ? Result<void>{diagnostic(FailureCode::SourceInvalid,
                                                        "invalid fixture value")}
                              : ok();
         },
         [](const auto &) -> Result<std::uint64_t> { return sizeof(std::int64_t); },
         FreezeStrategy::ScalarCopy, [](const auto &value) { return value; }, pin}));
    return {
        "test",
        output,
        [input, calls](const SourceRequest &,
                       SourceValidationServices &) -> Result<std::shared_ptr<const Fixture>> {
            ++calls->inspected;
            return input;
        },
        [](const auto &input) -> Result<SourceBounds> {
            return SourceBounds{input->metadata.validated_scope,
                                {8192, 1024, 0, 4096, 4096, 8192, 0}};
        },
        [calls](std::shared_ptr<const Fixture> &&input, const SourceRequest &,
                SourceValidationServices &services) -> Result<Data> {
            ++calls->validated;
            for (auto value : input->values) {
                if (value < 0) {
                    return diagnostic(FailureCode::SourceInvalid, "invalid present fixture data");
                }
            }
            auto charge = services.account->reserve(ResourceKind::Source, 8192);
            if (const auto *error = std::get_if<Diagnostic>(&charge)) {
                return *error;
            }
            return Data{std::get<Reservation>(std::move(charge)), *input};
        },
        [](const Data &data) -> Result<SourceMetadata> { return data.fixture.metadata; },
        [](const Data &data) -> Result<std::span<const TimingDescriptor>> {
            return std::span<const TimingDescriptor>{data.fixture.timeline};
        },
        [calls](const Data &, SourceCursorServices &services) -> Result<Cursor> {
            require(services.account != nullptr &&
                        services.account->current(ResourceKind::State) > 0,
                    "cursor construction was not precharged to run");
            ++calls->constructed;
            return Cursor{0};
        },
        [calls](Cursor &cursor, const Data &data, const UpdateContext &,
                PublicationAllocator &) -> Result<SourceDelivery<std::int64_t>> {
            ++calls->delivered;
            const auto &timing = data.fixture.timeline.at(cursor.position++);
            if (const auto *gap = std::get_if<DeclaredGap>(&timing.delivery)) {
                return SourceDelivery<std::int64_t>{gap->gap};
            }
            return SourceDelivery<std::int64_t>{SourceRecord<std::int64_t>{
                timing,
                data.fixture.values.at(std::get<CompleteRecord>(timing.delivery).payload_index)}};
        },
        [calls](Cursor &cursor, const Data &data) -> Result<SourceEnd> {
            ++calls->closed;
            if (cursor.position != data.fixture.timeline.size()) {
                return diagnostic(FailureCode::SourceCoverage, "provider cursor incomplete");
            }
            return SourceEnd{data.fixture.metadata.validated_scope};
        },
        pin};
}
SourceSnapshot prepare(Provider value, SourceRequest requested,
                       SourceValidationServices &validation) {
    auto resolver = take(SourceResolver::create(validation.account, 65536, 4));
    success(resolver.register_provider(std::move(value)));
    success(resolver.freeze());
    auto candidate = take(resolver.inspect(requested, validation));
    return take(resolver.validate(std::move(candidate), requested, validation));
}
UpdateContext activation(const TimingDescriptor &timing) {
    return {timing.key, timing.extent, timing.available_at_ns, timing.available_at_ns};
}
void immutable_sources_and_run_accounts() {
    auto input = fixture();
    auto calls = std::make_shared<Counters>();
    auto prep_account = account();
    auto validation = services(prep_account);
    auto snapshot = prepare(provider(input, calls), request(), validation);
    input->values[0] = 999;
    input->timeline[0].available_at_ns = 999;
    auto first_account = account(), second_account = account();
    PublicationAllocator first_allocator{first_account, 0}, second_allocator{second_account, 0};
    auto first = take(snapshot.cursor(first_allocator)),
         second = take(snapshot.cursor(second_allocator));
    require(prep_account->current(ResourceKind::State) == 0 &&
                first_account->current(ResourceKind::State) > 0 &&
                second_account->current(ResourceKind::State) > 0,
            "cursor charged the preparation or wrong run account");
    require(first.descriptor()->available_at_ns == 30 && calls->delivered == 0,
            "descriptor inspection advances or uses mutated input");
    auto current = activation(*first.descriptor());
    auto invalid = current;
    ++invalid.key.sequence;
    rejects(first.next(invalid), FailureCode::UpdateProtocol);
    invalid = current;
    --invalid.decision_time_ns;
    rejects(first.next(invalid), FailureCode::FutureRead);
    require(calls->delivered == 0, "invalid activation reached provider");
    auto slot = take(ResultSlot::create(first_account, {snapshot.output_contract()}, 0,
                                        snapshot.metadata().content_digest,
                                        snapshot.metadata().source_reference_digest));
    auto event = take(first.next(current));
    slot->commit(
        take(validate_pending(std::get<PendingOutcome>(std::move(event)), *slot, current)));
    require(take(slot->read<std::int64_t>(0, current)).value == 7 &&
                take(slot->read<std::int64_t>(0, current)).value == 7,
            "source value changed or publication read advanced");
    require(calls->delivered == 1 && second.descriptor()->key.sequence == 0,
            "fresh cursors shared position");
    rejects(first.next(current), FailureCode::UpdateProtocol);
    current = activation(*first.descriptor());
    static_cast<void>(take(first.next(current)));
    require(first.descriptor() == nullptr, "exhausted cursor exposes an event");
    require(take(first.finish()).completed_scope == request().required_scope && calls->closed == 1,
            "closure changed scope");
    rejects(first.finish(), FailureCode::AlreadyExecuted);
    rejects(first.next(current), FailureCode::AlreadyExecuted);
}
void exact_candidate_binding() {
    auto calls = std::make_shared<Counters>();
    auto validation = services(account());
    auto requested = request();
    auto resolver = take(SourceResolver::create(validation.account, 65536, 4));
    auto invalid_type = provider(fixture(), calls);
    invalid_type.output_type.policy.freeze_strategy = FreezeStrategy::DeepCopy;
    rejects(resolver.register_provider(std::move(invalid_type)), FailureCode::InvalidConfig);
    success(resolver.register_provider(provider(fixture(), calls)));
    rejects(resolver.register_provider(provider(fixture(), calls)),
            FailureCode::DuplicateDefinition);
    rejects(resolver.inspect(requested, validation), FailureCode::InvalidConfig);
    success(resolver.freeze());
    auto candidate = take(resolver.inspect(requested, validation));
    auto changed = requested;
    changed.requested_version = "v2";
    rejects(resolver.validate(std::move(candidate), changed, validation),
            FailureCode::SourceChanged);
    require(calls->validated == 0, "changed candidate request reached validation");
    rejects(resolver.validate(std::move(candidate), requested, validation),
            FailureCode::InvalidConfig);
    candidate = take(resolver.inspect(requested, validation));
    auto other = services(account());
    rejects(resolver.validate(std::move(candidate), requested, other), FailureCode::SourceChanged);
    requested.provider_name = "unknown";
    rejects(resolver.inspect(requested, validation), FailureCode::UnknownDefinition);
    requested = request();
    requested.interpretation.nominal_name = "other";
    rejects(resolver.inspect(requested, validation), FailureCode::TypeMismatch);
    rejects(resolver.register_provider(provider(fixture(), calls)), FailureCode::InvalidConfig);
}
void gaps_and_full_validation() {
    auto input = fixture();
    auto calls = std::make_shared<Counters>();
    input->metadata.validated_scope.end = 3;
    input->metadata.coverage.expected_scope.end = 3;
    input->metadata.coverage.complete_keys[1].sequence = 2;
    SourceGap gap{{"clock", 1, 2}, {30, 40}, 7};
    input->metadata.coverage.gaps.push_back(gap);
    input->timeline[1].key.sequence = 2;
    input->timeline.insert(input->timeline.begin() + 1,
                           {{"clock", 1}, gap.interval, 40, DeclaredGap{gap}});
    auto requested = request();
    requested.required_scope.end = 3;
    auto validation = services(account());
    auto resolver = take(SourceResolver::create(validation.account, 65536, 4));
    success(resolver.register_provider(provider(input, calls)));
    success(resolver.freeze());
    auto candidate = take(resolver.inspect(requested, validation));
    rejects(resolver.validate(std::move(candidate), requested, validation),
            FailureCode::SourceCoverage);
    validation.missing_intervals = MissingIntervalPolicy::Skip;
    auto snapshot = prepare(provider(input, calls), requested, validation);
    auto run = account();
    PublicationAllocator allocator{run, 0};
    auto cursor = take(snapshot.cursor(allocator));
    static_cast<void>(take(cursor.next(activation(*cursor.descriptor()))));
    auto event = take(cursor.next(activation(*cursor.descriptor())));
    require(std::get<SourceGap>(event) == gap && cursor.descriptor()->key.sequence == 2,
            "gap was fabricated as a value or keys compressed");
    static_cast<void>(take(cursor.next(activation(*cursor.descriptor()))));
    static_cast<void>(take(cursor.finish()));
    input->values[1] = -1;
    candidate = take(resolver.inspect(requested, validation));
    rejects(resolver.validate(std::move(candidate), requested, validation),
            FailureCode::SourceInvalid);
}
void closure_and_failed_cursor() {
    auto calls = std::make_shared<Counters>();
    auto validation = services(account());
    auto value = provider(fixture(), calls);
    value.next = [calls](Cursor &, const Data &, const UpdateContext &,
                         PublicationAllocator &) -> Result<SourceDelivery<std::int64_t>> {
        ++calls->delivered;
        return SourceDelivery<std::int64_t>{SourceEnd{{"clock", 0, 2}}};
    };
    auto snapshot = prepare(std::move(value), request(), validation);
    auto run = account();
    PublicationAllocator allocator{run, 0};
    auto cursor = take(snapshot.cursor(allocator));
    rejects(cursor.finish(), FailureCode::SourceCoverage);
    require(calls->closed == 0, "early finish reached provider");
    const auto current = activation(*cursor.descriptor());
    rejects(cursor.next(current), FailureCode::SourceCoverage);
    rejects(cursor.next(current), FailureCode::SourceCoverage);
    rejects(cursor.finish(), FailureCode::SourceCoverage);
    require(calls->delivered == 1, "failed provider was retried");
    auto empty = fixture();
    empty->values.clear();
    empty->timeline.clear();
    empty->metadata.validated_scope.end = 0;
    empty->metadata.coverage.expected_scope.end = 0;
    empty->metadata.coverage.complete_keys.clear();
    auto requested = request();
    requested.required_scope.end = 0;
    snapshot = prepare(provider(empty, calls), requested, validation);
    cursor = take(snapshot.cursor(allocator));
    require(cursor.descriptor() == nullptr && take(cursor.finish()).completed_scope.end == 0,
            "empty source needed a fictitious activation");
}
void admission_and_metadata_failures() {
    auto calls = std::make_shared<Counters>();
    auto validation = services(account());
    auto resolver = take(SourceResolver::create(validation.account, 65536, 4));
    auto input = fixture();
    success(resolver.register_provider(provider(input, calls)));
    success(resolver.freeze());
    auto tiny = validation;
    tiny.limits.candidate_bytes = 1;
    rejects(resolver.inspect(request(), tiny), FailureCode::ResourcePlanExceeded);
    require(calls->inspected == 0, "inspection preceded wrapper admission");
    tiny = validation;
    tiny.limits.snapshot_bytes = 1;
    auto candidate = take(resolver.inspect(request(), tiny));
    rejects(resolver.validate(std::move(candidate), request(), tiny),
            FailureCode::ResourcePlanExceeded);
    require(calls->validated == 0, "validation preceded snapshot admission");
    tiny = validation;
    tiny.limits.cursor_bytes = 1;
    candidate = take(resolver.inspect(request(), tiny));
    rejects(resolver.validate(std::move(candidate), request(), tiny),
            FailureCode::ResourcePlanExceeded);
    require(calls->validated == 0, "known unallocatable cursor reached provider validation");
    input->metadata.version = "wrong";
    candidate = take(resolver.inspect(request(), validation));
    rejects(resolver.validate(std::move(candidate), request(), validation),
            FailureCode::SourceChanged);
    input->metadata.version = "v1";
    input->metadata.witness.type = typeid(std::uint64_t);
    candidate = take(resolver.inspect(request(), validation));
    rejects(resolver.validate(std::move(candidate), request(), validation),
            FailureCode::TypeMismatch);
    input->metadata.witness.type = typeid(std::int64_t);
    input->timeline[1].key.sequence = 0;
    candidate = take(resolver.inspect(request(), validation));
    rejects(resolver.validate(std::move(candidate), request(), validation),
            FailureCode::SourceCoverage);
}
void callback_failure_and_output_pin() {
    auto calls = std::make_shared<Counters>();
    auto validation = services(account());
    auto run = account();
    PublicationAllocator allocator{run, 0};
    std::optional<Immutable<std::int64_t>> retained;
    auto pin = std::make_shared<int>(1);
    std::weak_ptr<int> weak = pin;
    {
        auto snapshot = prepare(provider(fixture(), calls, pin), request(), validation);
        pin.reset();
        auto cursor = take(snapshot.cursor(allocator));
        auto current = activation(*cursor.descriptor());
        auto slot = take(ResultSlot::create(run, {snapshot.output_contract()}, 0,
                                            snapshot.metadata().content_digest,
                                            snapshot.metadata().source_reference_digest));
        auto event = take(cursor.next(current));
        slot->commit(
            take(validate_pending(std::get<PendingOutcome>(std::move(event)), *slot, current)));
        retained = take(take(slot->read<std::int64_t>(0, current)).retain("test", "source_pin"));
    }
    require(!weak.expired() && retained->get() == 7,
            "source output lost code pin after snapshot/cursor teardown");
    retained.reset();
    require(weak.expired(), "source pin leaked after final output release");
    auto value = provider(fixture(), calls);
    value.next = [calls](Cursor &, const Data &, const UpdateContext &,
                         PublicationAllocator &) -> Result<SourceDelivery<std::int64_t>> {
        ++calls->delivered;
        throw std::runtime_error("provider failure");
    };
    auto snapshot = prepare(std::move(value), request(), validation);
    auto cursor = take(snapshot.cursor(allocator));
    const auto current = activation(*cursor.descriptor());
    const auto before = calls->delivered;
    rejects(cursor.next(current), FailureCode::CallbackException);
    rejects(cursor.next(current), FailureCode::CallbackException);
    require(calls->delivered == before + 1, "exception allowed cursor retry");
}
void delivery_and_terminal_contracts() {
    auto validation = services(account());
    auto calls = std::make_shared<Counters>();
    auto value = provider(fixture(), calls);
    value.next = [](Cursor &, const Data &data, const UpdateContext &,
                    PublicationAllocator &) -> Result<SourceDelivery<std::int64_t>> {
        auto wrong = data.fixture.timeline[0];
        ++wrong.available_at_ns;
        return SourceDelivery<std::int64_t>{SourceRecord<std::int64_t>{std::move(wrong), 7}};
    };
    auto snapshot = prepare(std::move(value), request(), validation);
    auto run = account();
    PublicationAllocator allocator{run, 0};
    auto cursor = take(snapshot.cursor(allocator));
    rejects(cursor.next(activation(*cursor.descriptor())), FailureCode::SourceCoverage);
    value = provider(fixture(), calls);
    value.finish = [calls](Cursor &, const Data &) -> Result<SourceEnd> {
        ++calls->closed;
        return SourceEnd{{"clock", 0, 3}};
    };
    snapshot = prepare(std::move(value), request(), validation);
    cursor = take(snapshot.cursor(allocator));
    for (unsigned i = 0; i < 2; ++i) {
        static_cast<void>(take(cursor.next(activation(*cursor.descriptor()))));
    }
    rejects(cursor.finish(), FailureCode::SourceCoverage);
    rejects(cursor.finish(), FailureCode::SourceCoverage);
    require(calls->closed == 1, "failed terminal callback was retried");
    value = provider(fixture(), calls);
    value.make_cursor = [](const Data &, SourceCursorServices &) -> Result<Cursor> {
        throw std::runtime_error("construction failed");
    };
    snapshot = prepare(std::move(value), request(), validation);
    const auto before = run->current(ResourceKind::State);
    rejects(snapshot.cursor(allocator), FailureCode::CallbackException);
    require(run->current(ResourceKind::State) == before,
            "failed cursor construction leaked run reservation");
    auto bounded = take(ResourceAccount::create(
        {{1048576, 0, 1048576, 1048576, 1048576, 1048576, 1048576}, 128, 128, 4096, 128}));
    PublicationAllocator denied{bounded, 0};
    snapshot = prepare(provider(fixture(), calls), request(), validation);
    const auto constructed = calls->constructed;
    rejects(snapshot.cursor(denied), FailureCode::ResourceExceeded);
    require(calls->constructed == constructed, "cursor factory ran before its run reservation");
}
} // namespace
int main() {
    const Case cases[]{
        {"immutable source and independent run accounts", immutable_sources_and_run_accounts},
        {"candidate identity and single use", exact_candidate_binding},
        {"Strict/Skip and full present-data validation", gaps_and_full_validation},
        {"terminal closure and sticky failure", closure_and_failed_cursor},
        {"pre-callback admission and metadata validation", admission_and_metadata_failures},
        {"callback failure and retained output pin", callback_failure_and_output_pin},
        {"delivery and terminal contract failures", delivery_and_terminal_contracts}};
    return lexongine::test::run(cases);
}
