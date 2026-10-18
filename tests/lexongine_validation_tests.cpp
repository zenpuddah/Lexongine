#include "lexongine/registry.hpp"
#include "lexongine_test_support.hpp"

namespace lexongine {
template<> struct PublicationTraits<std::int64_t> {
    using type = std::int64_t;
    static constexpr StorageKind storage = StorageKind::ScalarCopy;
};
}
namespace {
using namespace lexongine;
using namespace lexongine::test;
std::uint64_t states = 0, steps = 0, operation_checks = 0, type_checks = 0;
bool throwing_validator = false;
std::weak_ptr<const void> required_pin;
TypeContract type() { return {"test.validated_integer", 1, {1, {}}}; }
Contract protocol(std::string name) { return {std::move(name), 1, {1, {}}}; }
std::shared_ptr<ResourceAccount> account() {
    return take(ResourceAccount::create({{1048576, 1048576, 1048576, 1048576, 1048576, 2097152, 1048576}, 256, 256, 4096, 128}));
}
PreparedKernel prepare(std::shared_ptr<ResourceAccount> resources, ModulePin pin) {
    states = steps = operation_checks = type_checks = 0; throwing_validator = false; required_pin = pin;
    auto registry = take(Registry::create());
    auto integer = take(registry.register_type<std::int64_t>({type(), StorageKind::ScalarCopy,
        [](const std::int64_t& value) -> Result<void> { ++type_checks; return value < 0 ? Result<void>{diagnostic(FailureCode::OutputInvalid, "negative test output")} : ok(); },
        [](const std::int64_t&) -> Result<std::uint64_t> { return std::uint64_t{8}; }, FreezeStrategy::ScalarCopy,
        [](const std::int64_t& value) { return value; }, {}}));
    OperationContract description{{"test", "validated_pair", 1, "test-v1"}, {{}, {}, {}, "validation-test"},
        {1, {{"ceiling", ParamKind::Signed64, true, {}}}}, {},
        {{"left", type(), StorageKind::ScalarCopy, protocol("test.integer"), protocol("test.nonnegative"), true},
         {"right", type(), StorageKind::ScalarCopy, protocol("test.integer"), protocol("test.nonnegative"), true}},
        {SharingClass::StatelessDeterministic, protocol("stateless"), protocol("same_step"), protocol("none"), true, true, true, {"prepared-validation-tests"}},
        {"point_v1"}, {Alignment::SameStep}, {MissingIntervalPolicy::Strict}, {"count"}, "Typed prepared validation probe"};
    TypedOperation<std::int64_t, int, std::tuple<>, std::tuple<std::int64_t, std::int64_t>> operation{
        description, {}, {{"left", integer}, {"right", integer}},
        [](const ParamSet& values) -> Result<std::int64_t> { return parse_integer<std::int64_t>(values.values.at(0).text); },
        [](const std::int64_t& value) -> Result<CanonicalParams> { return CanonicalParams{1, {{"ceiling", value, Origin::ExplicitConfig, "test"}}}; },
        [](const std::int64_t&, const PreparationFacts&) -> Result<HistoryRequirement> { return HistoryRequirement{CountHistory{0}}; },
        [](const std::int64_t&, const PreparationFacts&) -> Result<ResourceBounds> { return ResourceBounds{0, 0, 0, 8192, 4096, 16384, 0}; },
        [](const std::int64_t&, const InitializationSpec&, StateAllocator&) -> Result<int> { ++states; return 0; },
        [](int&, const std::int64_t&, const std::tuple<>&, const UpdateContext&, PublicationAllocator&) -> StepResult<std::tuple<std::int64_t, std::int64_t>> {
            ++steps; return Published{std::tuple<std::int64_t, std::int64_t>{5, 6}, ParentSelections::none()};
        },
        [](const std::int64_t& ceiling, const std::tuple<std::int64_t, std::int64_t>& values, const UpdateContext&) -> Result<void> {
            ++operation_checks; require(!required_pin.expired(), "prepared validator lost module pin");
            if (throwing_validator) { throw std::runtime_error("injected output validation exception"); }
            if (std::get<0>(values) > ceiling || std::get<1>(values) > ceiling) { return diagnostic(FailureCode::NumericBound, "test output exceeds prepared ceiling"); }
            return ok();
        }, std::move(pin)};
    success(registry.register_definition(std::move(operation))); success(registry.freeze());
    const PreparationFacts facts{{"clock", 0, 3}, {protocol("stateless"), {"clock", 0, 0}, {1, {}}},
        MissingIntervalPolicy::Strict, Alignment::SameStep, {65536, 65536, 65536, 65536, 65536, 16384, 65536}, resources};
    return take(take(registry.resolve(description.key))->prepare({{{"ceiling", "10", {"test", 1}}}}, facts));
}
UpdateContext context() { return {{"clock", 0}, PointTime{10}, 12, 11}; }
PendingOutcome bundle(std::shared_ptr<ResourceAccount> resources, std::int64_t left, std::int64_t right, std::size_t count = 2, bool wrong_witness = false) {
    auto pending = take(PendingOutcome::create(resources, context(), count));
    for (std::size_t i = 0; i < count; ++i) {
        if (i == 1 && wrong_witness) {
            auto value = take(make_owned<std::uint64_t>(resources, ResourceKind::Retention, 8, {}, [] { return std::uint64_t{6}; }));
            success(pending.set_output(static_cast<PortIndex>(i), take(ErasedValue::from(std::move(value), type()))));
        } else {
            auto value = take(make_owned<std::int64_t>(resources, ResourceKind::Retention, 8, {}, [=] { return i == 0 ? left : right; }));
            success(pending.set_output(static_cast<PortIndex>(i), take(ErasedValue::from(std::move(value), type()))));
        }
    }
    return pending;
}
void no_state_or_step_and_typed_rules() {
    auto resources = account(); auto prepared = prepare(resources, std::make_shared<int>(1));
    require(prepared.has_output_validation(), "typed preparation did not bind output validation");
    auto valid = bundle(resources, 5, 6); const auto retained = resources->current(ResourceKind::Retention);
    success(prepared.validate_outputs(valid, context()));
    require(states == 0 && steps == 0 && operation_checks == 1 && type_checks == 2 && resources->current(ResourceKind::Retention) == retained,
        "prepared validation constructed state, advanced math or copied payloads");
    rejects(prepared.validate_outputs(bundle(resources, 5, 11), context()), FailureCode::NumericBound);
    rejects(prepared.validate_outputs(bundle(resources, 5, -1), context()), FailureCode::OutputInvalid);
    require(states == 0 && steps == 0, "failed validation invoked a numerical factory");
    auto slot = take(ResultSlot::create(resources, {{type(), typeid(std::int64_t), StorageKind::ScalarCopy, {}},
        {type(), typeid(std::int64_t), StorageKind::ScalarCopy, {}}}, 0, {}, {}));
    slot->commit(take(validate_pending(std::move(valid), *slot, context())));
    require(take(slot->read<std::int64_t>(0, context())).value == 5 && take(slot->read<std::int64_t>(1, context())).value == 6, "validation consumed or changed the staged bundle");
}
void shape_witness_and_temporal_guards() {
    auto resources = account(); auto prepared = prepare(resources, std::make_shared<int>(1));
    rejects(prepared.validate_outputs(bundle(resources, 5, 6, 1), context()), FailureCode::OutputInvalid);
    rejects(prepared.validate_outputs(bundle(resources, 5, 6, 2, true), context()), FailureCode::TypeMismatch);
    auto changed = context(); changed.key.sequence = 1;
    rejects(prepared.validate_outputs(bundle(resources, 5, 6), changed), FailureCode::UpdateProtocol);
    changed = context(); changed.extent = Interval{0, 10};
    rejects(prepared.validate_outputs(bundle(resources, 5, 6), changed), FailureCode::UpdateProtocol);
    changed = context(); changed.decision_time_ns = 10;
    rejects(prepared.validate_outputs(bundle(resources, 5, 6), changed), FailureCode::FutureRead);
    auto warming = bundle(resources, 5, 6); warming.warming(1);
    rejects(prepared.validate_outputs(warming, context()), FailureCode::OutputInvalid);
    require(operation_checks == 0 && type_checks == 0, "malformed output reached author validation");
}
void pin_and_exception_boundary() {
    auto resources = account(); ModulePin pin = std::make_shared<int>(1); std::weak_ptr<const void> weak = pin;
    std::optional<PreparedKernel> prepared{prepare(resources, pin)}; pin.reset();
    require(!weak.expired(), "prepared validator did not own its module after registry teardown");
    throwing_validator = true;
    rejects(prepared->validate_outputs(bundle(resources, 5, 6), context()), FailureCode::CallbackException);
    require(states == 0 && steps == 0, "validation exception entered numerical execution");
    prepared.reset(); require(weak.expired(), "validator module pin survived final prepared owner");
}
void unavailable_validation_is_explicit() {
    auto resources = account();
    auto prepared = take(PreparedKernel::create({}, {1, {}}, CountHistory{0}, {},
        [](InstanceServices&) -> Result<KernelInstance> { return diagnostic(FailureCode::UnsupportedContract, "description-only test factory"); }));
    require(!prepared.has_output_validation(), "custom factory invented a validation capability");
    rejects(prepared.validate_outputs(bundle(resources, 5, 6), context()), FailureCode::UnsupportedContract);
}
}
int main() {
    const Case cases[]{{"prepared typed validation without state or numerical steps", no_state_or_step_and_typed_rules},
        {"bundle shape, native witness and temporal validation", shape_witness_and_temporal_guards},
        {"prepared validator pin lifetime and exception boundary", pin_and_exception_boundary},
        {"unavailable validation is explicit", unavailable_validation_is_explicit}};
    return lexongine::test::run(cases);
}
