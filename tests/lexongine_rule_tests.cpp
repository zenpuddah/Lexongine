#include "quant/lexongine/components.hpp"
#include "lexongine_test_support.hpp"
#include "../integration/lexongine/fixture.hpp"

namespace {
using namespace lexongine;
using namespace lexongine::test;
using quant::data::Bar;
using quant::data::Price;
namespace ql = quant::lexongine;
std::shared_ptr<ResourceAccount> account() {
    return take(ResourceAccount::create(
        {{1048576, 1048576, 1048576, 1048576, 1048576, 1048576, 4194304}, 1024, 1024, 16384, 128}));
}
ql::ComponentRegistration build(ModulePin pin = {}) {
    return {{Sha256{std::byte{1}}, Sha256{std::byte{2}}, Sha256{std::byte{3}}, "rule-test-build"},
            std::move(pin),
            MissingIntervalPolicy::Strict};
}
OperationKey mean_key() {
    return {"statistics", "sma_price", 1, "builtin-v1"};
}
OperationKey close_key() {
    return {"qaedalus", "bar_close", 1, "builtin-v1"};
}
Contract contract(std::string name) {
    return {std::move(name), 1, {1, {}}};
}
Result<void> positive(const Price &value, const CanonicalParams &, const UpdateContext &) {
    return value.raw() >= 0 ? ok()
                            : Result<void>{diagnostic(FailureCode::SourceKnownBoundViolation,
                                                      "negative test input")};
}
void typed_rules_use_source_values() {
    auto resources = account();
    auto registry = take(Registry::create());
    auto types = take(ql::register_components(registry, build()));
    success(registry.freeze());
    const auto projection = registry.source_projection(close_key());
    require(projection != nullptr && registry.source_constraints().size() == 1,
            "builtin source rules were not registered");
    const auto constraint = registry.source_constraints()[0];
    auto bars =
        take(ResultSlot::create(resources, {take(registry.resolve_type(types.bar.policy.contract))},
                                0, Sha256{std::byte{10}}, std::nullopt));
    auto prices = take(ResultSlot::create(resources, {projection->output()}, 0,
                                          Sha256{std::byte{11}}, std::nullopt));
    const auto parameters =
        take(take(registry.resolve(mean_key()))
                 ->prepare_parameters({{{"window", "20", {"test", 1}},
                                        {"max_abs_input_raw", "100500000000", {"test", 1}},
                                        {"sum_width", "signed64", {"test", 1}},
                                        {"rounding", "nearest_ties_even", {"test", 1}},
                                        {"overflow", "fail", {"test", 1}}}},
                                      256));
    std::optional<std::uint64_t> first_bad;
    for (std::uint64_t key = 0; key < fixture::bar_count; ++key) {
        const auto value = take(fixture::complete_bar(key));
        const auto available = value.interval_end.unix_nanos();
        UpdateContext context{{"bar-clock-v1", key},
                              Interval{value.interval_start.unix_nanos(), available},
                              available,
                              available};
        PublicationAllocator allocator{resources, 0};
        auto pending = take(PendingOutcome::create(resources, context, 1));
        success(
            pending.set_output(0, take(ErasedValue::from(take(allocator.freeze(value, types.bar)),
                                                         types.bar.policy.contract))));
        bars->commit(take(validate_pending(std::move(pending), *bars, context)));
        auto projected = take(projection->apply({1, {}}, {bars, 0, 0}, context, resources));
        prices->commit(take(validate_pending(std::move(projected), *prices, context)));
        auto result = constraint->apply(parameters, {prices, 0, 0}, context);
        if (std::holds_alternative<Diagnostic>(result)) {
            rejects(std::move(result), FailureCode::SourceKnownBoundViolation);
            if (!first_bad) {
                first_bad = key;
            }
        }
    }
    require(first_bad == 51,
            "preflight missed the first source-known bound violation after a passing prefix");
    require(registry.source_projection(mean_key()) == nullptr,
            "SMA was registered as a hidden source projection");
}
void exact_binding_and_validation_identity() {
    auto registry = take(Registry::create());
    auto registration = build();
    auto types = take(ql::register_components(registry, registration));
    const auto before = take(registry.source_validation_contract(mean_key(), 1024));
    SourceRuleContract rule{mean_key(), registration.implementation, contract("test.positive")};
    auto changed = rule;
    changed.implementation.binary_digest[0] = std::byte{99};
    rejects(registry.register_source_constraint(changed, 0, types.price, positive, {}),
            FailureCode::SourceChanged);
    auto unknown = rule;
    unknown.operation.semantic_version = 2;
    rejects(registry.register_source_constraint(unknown, 0, types.price, positive, {}),
            FailureCode::UnknownDefinition);
    rejects(registry.register_source_constraint(rule, 1, types.price, positive, {}),
            FailureCode::InvalidConfig);
    RegisteredType<std::uint64_t> wrong{
        {types.price.policy.contract,
         StorageKind::ScalarCopy,
         [](const auto &) -> Result<void> { return ok(); },
         [](const auto &) -> Result<std::uint64_t> { return sizeof(std::uint64_t); },
         FreezeStrategy::ScalarCopy,
         [](const auto &value) { return value; },
         {}},
        {{}, typeid(std::uint64_t)}};
    rejects(registry.register_source_constraint<std::uint64_t>(
                rule, 0, wrong,
                [](const std::uint64_t &, const CanonicalParams &,
                   const UpdateContext &) -> Result<void> { return ok(); },
                {}),
            FailureCode::TypeMismatch);
    rejects(registry.register_source_projection<Price, Price>(
                rule, types.price, types.price,
                [](const Price &value, const CanonicalParams &,
                   const UpdateContext &) -> Result<Price> { return value; },
                {}),
            FailureCode::UnsupportedContract);
    success(registry.register_source_constraint(rule, 0, types.price, positive, {}));
    rejects(registry.register_source_constraint(rule, 0, types.price, positive, {}),
            FailureCode::DuplicateDefinition);
    require(!(take(registry.source_validation_contract(mean_key(), 1024)) == before),
            "registered validation policy omitted from identity facts");
    success(registry.freeze());
    rule.validation.id = "new";
    rejects(registry.register_source_constraint(rule, 0, types.price, positive, {}),
            FailureCode::InvalidConfig);
}
void rule_lifetimes_are_host_pinned() {
    auto resources = account();
    auto pin = std::make_shared<int>(1);
    std::weak_ptr<int> weak = pin;
    std::shared_ptr<const SourceProjectionDefinition> projection;
    {
        auto registry = take(Registry::create());
        static_cast<void>(take(ql::register_components(registry, build(pin))));
        projection = registry.source_projection(close_key());
        pin.reset();
    }
    require(projection && !weak.expired(), "registry teardown invalidated a retained rule");
    require(projection->contract().operation == close_key(),
            "retained rule lost its owned operation binding");
    projection.reset();
    require(weak.expired(), "rule code pin leaked after host-owned destruction");
}
} // namespace
int main() {
    const Case cases[]{
        {"typed projections and full-scope source bounds", typed_rules_use_source_values},
        {"exact rule binding and validation identity", exact_binding_and_validation_identity},
        {"retained source rule lifetime", rule_lifetimes_are_host_pinned}};
    return lexongine::test::run(cases);
}
