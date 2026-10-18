#include "quant/lexongine/components.hpp"
#include "lexongine_test_support.hpp"
#include "../integration/lexongine/fixture.hpp"

namespace {
using namespace lexongine;
using namespace lexongine::test;
namespace ql = quant::lexongine;
using Price = quant::data::Price;
std::shared_ptr<ResourceAccount> account() {
    return take(ResourceAccount::create({{1048576, 1048576, 1048576, 1048576, 1048576, 1048576, 4194304}, 1024, 1024, 16384, 128}));
}
Contract contract(std::string name) { return {std::move(name), 1, {1, {}}}; }
ql::ComponentRegistration registration(MissingIntervalPolicy missing = MissingIntervalPolicy::Strict) {
    return {{Sha256{std::byte{1}}, Sha256{std::byte{2}}, Sha256{std::byte{3}}, "component-test-build"}, {}, missing};
}
PreparationFacts facts(std::shared_ptr<ResourceAccount> resources, MissingIntervalPolicy missing, std::string init) {
    return {{"bars", 0, 4}, {contract(std::move(init)), {"bars", 0, 0}, {1, {}}}, missing, Alignment::SameStep,
        {65536, 131072, 65536, 131072, 65536, 16384, 65536}, std::move(resources)};
}
ParamSet sma(std::uint64_t window, std::int64_t bound) {
    return {{{"window", std::to_string(window), {"test", 1}}, {"max_abs_input_raw", std::to_string(bound), {"test", 1}},
        {"sum_width", "signed64", {"test", 1}}, {"rounding", "nearest_ties_even", {"test", 1}}, {"overflow", "fail", {"test", 1}}}};
}
void publish(std::shared_ptr<ResourceAccount> resources, const RegisteredType<Price>& type, ResultSlot& slot,
             const UpdateContext& context, std::int64_t raw) {
    PublicationAllocator allocator{resources, 0}; auto value = take(allocator.freeze(Price::from_raw(raw), type));
    auto pending = take(PendingOutcome::create(resources, context, 1));
    success(pending.set_output(0, take(ErasedValue::from(std::move(value), type.policy.contract))));
    slot.commit(take(validate_pending(std::move(pending), slot, context)));
}
void registered_sma_and_provenance() {
    auto resources = account(); auto registry = take(Registry::create());
    auto types = take(ql::register_components(registry, registration())); success(registry.freeze());
    auto definition = take(registry.resolve({"statistics", "sma_price", 1, "builtin-v1"}));
    auto prepared = take(definition->prepare(sma(3, 8), facts(resources, MissingIntervalPolicy::Strict, "empty_at_step_0")));
    auto input = take(ResultSlot::create(resources, {{types.price.policy.contract, typeid(Price), StorageKind::ScalarCopy, {}}}, 0, Sha256{std::byte{10}}, std::nullopt));
    auto output = take(ResultSlot::create(resources, {{types.price.policy.contract, typeid(Price), StorageKind::ScalarCopy, {}}}, 0, Sha256{std::byte{11}}, std::nullopt));
    InstanceServices services{resources, {{input, 0, 0}}};
    auto instance = take(prepared.instantiate(services));
    const std::array<std::int64_t, 4> values{2, 2, 2, 8};
    for (std::uint64_t key = 0; key < values.size(); ++key) {
        UpdateContext current{{"bars", key}, Interval{static_cast<std::int64_t>(key * 60), static_cast<std::int64_t>((key + 1) * 60)},
            static_cast<std::int64_t>((key + 1) * 60), static_cast<std::int64_t>((key + 1) * 60)};
        publish(resources, types.price, *input, current, values[key]);
        output->commit(take(validate_pending(take(instance.step(current)), *output, current)));
        if (key < 2) { require(output->current()->kind == OutcomeKind::Warming, "registered SMA did not warm"); }
        else {
            require(take(output->read<Price>(0, current)).value.raw() == (key == 2 ? 2 : 4), "registered SMA differs from independent expected values");
            const auto parents = output->current()->parents.entries();
            require(parents.size() == 3 && parents[0].metadata().key.sequence == key - 2 && parents[2].metadata().key.sequence == key,
                "SMA omitted or reordered window parent provenance");
        }
    }
}
void registered_lag_keeps_original_keys() {
    auto resources = account(); auto registry = take(Registry::create());
    auto types = take(ql::register_components(registry, registration(MissingIntervalPolicy::Skip))); success(registry.freeze());
    auto definition = take(registry.resolve({"series", "lag_price", 1, "builtin-v1"}));
    auto prepared = take(definition->prepare({{{"lag_steps", "1", {"test", 1}}, {"count_basis", "complete_bar_updates", {"test", 1}}}},
        facts(resources, MissingIntervalPolicy::Skip, "empty_at_step_0")));
    auto input = take(ResultSlot::create(resources, {{types.price.policy.contract, typeid(Price), StorageKind::ScalarCopy, {}}}, 0, Sha256{std::byte{10}}, std::nullopt));
    auto output = take(ResultSlot::create(resources, {{types.price.policy.contract, typeid(Price), StorageKind::ScalarCopy, {}}}, 0, Sha256{std::byte{11}}, std::nullopt));
    InstanceServices services{resources, {{input, 0, 0}}}; auto instance = take(prepared.instantiate(services));
    const std::array<std::uint64_t, 3> keys{0, 2, 3}; const std::array<std::int64_t, 3> values{100, 110, 120};
    for (std::size_t index = 0; index < keys.size(); ++index) {
        auto key = keys[index]; UpdateContext current{{"bars", key}, Interval{static_cast<std::int64_t>(key * 60), static_cast<std::int64_t>((key + 1) * 60)},
            static_cast<std::int64_t>((key + 1) * 60), static_cast<std::int64_t>((key + 1) * 60)};
        publish(resources, types.price, *input, current, values[index]);
        output->commit(take(validate_pending(take(instance.step(current)), *output, current)));
        if (index == 0) { require(output->current()->kind == OutcomeKind::Warming, "lag emitted before history"); }
        else {
            require(take(output->read<Price>(0, current)).value.raw() == values[index - 1], "lag treated key distance as publication count");
            require(output->current()->parents.entries()[0].metadata().key.sequence == keys[index - 1] &&
                output->current()->available_at_ns == current.activation_available_at_ns, "lag lost original parent or backdated availability");
        }
    }
}
void preparation_rejects_invalid_component_facts() {
    auto resources = account(); auto registry = take(Registry::create());
    static_cast<void>(take(ql::register_components(registry, registration()))); success(registry.freeze());
    auto definition = take(registry.resolve({"statistics", "sma_price", 1, "builtin-v1"}));
    auto valid = facts(resources, MissingIntervalPolicy::Strict, "empty_at_step_0");
    rejects(definition->prepare(sma(0, 1), valid), FailureCode::InvalidConfig);
    rejects(definition->prepare(sma(2, std::numeric_limits<std::int64_t>::max()), valid), FailureCode::InvalidConfig);
    rejects(definition->prepare(sma(std::numeric_limits<std::int64_t>::max(), 1), valid), FailureCode::ResourcePlanExceeded);
    valid.initialization.contract = contract("stateless");
    rejects(definition->prepare(sma(2, 1), valid), FailureCode::UnsupportedContract);
}
void declared_state_covers_typed_controls() {
    auto resources = account(); auto registry = take(Registry::create());
    auto types = take(ql::register_components(registry, registration())); success(registry.freeze());
    auto definition = take(registry.resolve({"statistics", "sma_price", 1, "builtin-v1"}));
    auto prepared = take(definition->prepare(sma(3, 8), facts(resources, MissingIntervalPolicy::Strict, "empty_at_step_0")));
    auto input = take(ResultSlot::create(resources, {{types.price.policy.contract, typeid(Price), StorageKind::ScalarCopy, {}}}, 0, {}, std::nullopt));
    InstanceServices services{resources, {{input, 0, 0}}}; const auto before = resources->current(ResourceKind::State);
    auto instance = take(prepared.instantiate(services));
    require(resources->current(ResourceKind::State) - before <= prepared.resources().state,
        "declared state bound omitted typed adapter/control allocation");
}
void registered_close_and_ordered_difference() {
    auto resources = account(); auto registry = take(Registry::create());
    auto types = take(ql::register_components(registry, registration())); success(registry.freeze());
    auto bars = take(ResultSlot::create(resources, {take(registry.describe_type<quant::data::Bar>(types.bar.policy.contract))}, 0, Sha256{std::byte{20}}, std::nullopt));
    auto prices = take(ResultSlot::create(resources, {take(registry.describe_type<Price>(types.price.policy.contract))}, 0, Sha256{std::byte{21}}, std::nullopt));
    auto right = take(ResultSlot::create(resources, {take(registry.describe_type<Price>(types.price.policy.contract))}, 0, Sha256{std::byte{22}}, std::nullopt));
    auto result = take(ResultSlot::create(resources, {take(registry.describe_type<Price>(types.price.policy.contract))}, 0, Sha256{std::byte{23}}, std::nullopt));
    auto close = take(take(registry.resolve({"qaedalus", "bar_close", 1, "builtin-v1"}))->prepare({}, facts(resources, MissingIntervalPolicy::Strict, "stateless")));
    auto subtract = take(take(registry.resolve({"qaedalus", "price_difference", 1, "builtin-v1"}))->prepare({{{"overflow", "fail", {"test", 1}}}}, facts(resources, MissingIntervalPolicy::Strict, "stateless")));
    InstanceServices close_inputs{resources, {{bars, 0, 0}}}, difference_inputs{resources, {{prices, 0, 0}, {right, 0, 0}}};
    auto close_instance = take(close.instantiate(close_inputs)), difference_instance = take(subtract.instantiate(difference_inputs));
    auto bar = take(lexongine::fixture::complete_bar(0));
    UpdateContext current{{"bars", 0}, Interval{0, lexongine::fixture::interval_ns}, lexongine::fixture::interval_ns, lexongine::fixture::interval_ns};
    PublicationAllocator allocator{resources, 0}; auto pending = take(PendingOutcome::create(resources, current, 1));
    success(pending.set_output(0, take(ErasedValue::from(take(allocator.freeze(bar, types.bar)), types.bar.policy.contract))));
    bars->commit(take(validate_pending(std::move(pending), *bars, current)));
    prices->commit(take(validate_pending(take(close_instance.step(current)), *prices, current)));
    publish(resources, types.price, *right, current, 1000000000);
    result->commit(take(validate_pending(take(difference_instance.step(current)), *result, current)));
    require(take(result->read<Price>(0, current)).value.raw() == 99000000000 && result->current()->parents.entries().size() == 2,
        "registered close/subtraction changed scale, order or provenance");
    auto next = current; next.key.sequence = 1; next.extent = Interval{lexongine::fixture::interval_ns, 2 * lexongine::fixture::interval_ns};
    next.activation_available_at_ns = next.decision_time_ns = 2 * lexongine::fixture::interval_ns;
    publish(resources, types.price, *prices, next, std::numeric_limits<std::int64_t>::max());
    publish(resources, types.price, *right, next, -1);
    rejects(difference_instance.step(next), FailureCode::NumericOverflow);
    require(result->current()->context.key.sequence == 0, "failed subtraction exposed a new output");
}
}
int main() {
    const Case cases[]{{"registered SMA values and complete window provenance", registered_sma_and_provenance},
        {"registered Skip lag preserves original parent timing", registered_lag_keeps_original_keys},
        {"component preparation rejects unsupported initialization and bounds", preparation_rejects_invalid_component_facts},
        {"declared state includes typed adapter controls", declared_state_covers_typed_controls},
        {"registered close and ordered difference", registered_close_and_ordered_difference}};
    return lexongine::test::run(cases);
}
