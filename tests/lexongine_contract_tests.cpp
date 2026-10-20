#include "lexongine_test_support.hpp"
#include "lexongine/registry.hpp"
#include "lexongine/schema.hpp"

#include <limits>

namespace lexongine {
template <> struct PublicationTraits<std::int64_t> {
    using type = std::int64_t;
    static constexpr StorageKind storage = StorageKind::ScalarCopy;
};
} // namespace lexongine

namespace {
using namespace lexongine;
using namespace lexongine::test;

CanonicalParams empty_params() {
    return {1, {}};
}
Contract contract(std::string id) {
    return {std::move(id), 1, empty_params()};
}
TypeContract integer_type(std::string units = "count") {
    return {
        "test.integer", 1, {1, {{"units", std::move(units), Origin::VersionedFact, "test-v1"}}}};
}
OperationContract operation(std::string name, SharingClass sharing) {
    return {{"test", std::move(name), 1, "test-build"},
            {{}, {}, {}, "test-sdk"},
            {1, {{"amount", ParamKind::Signed64, true, {}}}},
            {{"input", integer_type(), Alignment::SameStep, MissingIntervalPolicy::Strict,
              AbsencePolicy::RequireValue}},
            {{"output", integer_type(), StorageKind::ScalarCopy, contract("integer-codec"),
              contract("integer-validation"), true}},
            {sharing,
             contract("empty"),
             contract("same_step"),
             contract("none"),
             sharing != SharingClass::Unique,
             true,
             true,
             {"contract-test"}},
            {"point_v1"},
            {Alignment::SameStep},
            {MissingIntervalPolicy::Strict},
            {"count"},
            "owned description"};
}
void diagnostics() {
    const auto error = diagnostic(FailureCode::InvalidConfig, std::string(400, 'x'));
    require(error.length == 256 && error.truncated,
            "diagnostic must be bounded without losing code");
    static_assert(std::is_nothrow_copy_constructible_v<Diagnostic>);
    require(error.text().size() == 256, "length must describe exact bounded message");
}
void parameters() {
    const auto schema = operation("identity", SharingClass::StatelessDeterministic).parameters;
    const auto parsed =
        take(parse_parameters({{{"amount", "-42", {"input.xml", 7}}}}, schema, 128));
    require(std::get<std::int64_t>(parsed.values[0].value) == -42, "signed literal lost");
    rejects(parse_parameters({}, schema, 128), FailureCode::InvalidConfig);
    rejects(parse_parameters({{{"other", "1", {"", 0}}}}, schema, 128), FailureCode::InvalidConfig);
    rejects(parse_parameters({{{"amount", "1", {"", 0}}, {"amount", "2", {"", 0}}}}, schema, 128),
            FailureCode::InvalidConfig);
    for (const auto *token : {"20junk", "+1", "01", "-0", " 1", "9223372036854775808"}) {
        rejects(parse_parameters({{{"amount", token, {"", 0}}}}, schema, 128),
                FailureCode::InvalidConfig);
    }
    auto unsigned_schema = schema;
    unsigned_schema.fields[0].kind = ParamKind::Unsigned64;
    rejects(parse_parameters({{{"amount", "-1", {"", 0}}}}, unsigned_schema, 128),
            FailureCode::InvalidConfig);
}
void canonical_ownership() {
    auto parameters =
        take(canonical_params(1,
                              {{"z", std::uint64_t{2}, Origin::ExplicitConfig, "file"},
                               {"a", std::string{"text"}, Origin::Derived, "derivation"}},
                              128));
    require(parameters.values.front().name == "a", "canonical parameters not ordered");
    auto changed_origin = parameters;
    changed_origin.values[0].origin = Origin::VersionedFact;
    changed_origin.values[0].provenance = "different display metadata";
    require(parameters == changed_origin, "origin leaked into semantic equality");
    rejects(canonical_params(
                1,
                {{"a", true, Origin::ExplicitConfig, ""}, {"a", false, Origin::ExplicitConfig, ""}},
                128),
            FailureCode::InvalidConfig);
    rejects(validate_text(std::string("\xc0\x80", 2), 8, false), FailureCode::InvalidConfig);
    rejects(validate_text("too long", 3, false), FailureCode::InvalidConfig);
}
void registry() {
    auto registry = take(Registry::create());
    auto definition = operation("identity", SharingClass::StatelessDeterministic);
    auto reject_factory = [](const ParamSet &, const PreparationFacts &) -> Result<PreparedKernel> {
        return diagnostic(FailureCode::UnsupportedContract, "test-only rejected factory");
    };
    rejects(registry.register_definition(definition, {}, {}), FailureCode::InvalidConfig);
    success(registry.register_definition(definition, {}, reject_factory));
    rejects(registry.register_definition(definition, {}, reject_factory),
            FailureCode::DuplicateDefinition);
    auto unresolved = definition.key;
    unresolved.semantic_version = 2;
    rejects(registry.resolve(unresolved), FailureCode::UnknownDefinition);
    definition.description = "destroyed caller text";
    const auto resolved = take(registry.resolve(definition.key));
    require(resolved->contract().description == "owned description",
            "registry borrowed caller descriptor");
    auto prepared =
        take(resolved->prepare_parameters({{{"amount", "2", {"temporary.xml", 2}}}}, 128));
    require(std::get<std::int64_t>(prepared.values[0].value) == 2,
            "prepare did not own typed parameter");
    success(registry.freeze());
    rejects(
        registry.register_definition(operation("new", SharingClass::Unique), {}, reject_factory),
        FailureCode::InvalidConfig);
}
void ports_and_clocks() {
    const auto input = operation("identity", SharingClass::StatelessDeterministic).inputs.front();
    success(validate_connection(input, integer_type(), "clock-A", "clock-A",
                                MissingIntervalPolicy::Strict));
    rejects(validate_connection(input, integer_type("money"), "clock-A", "clock-A",
                                MissingIntervalPolicy::Strict),
            FailureCode::TypeMismatch);
    rejects(validate_connection(input, integer_type(), "clock-A", "clock-B",
                                MissingIntervalPolicy::Strict),
            FailureCode::UnsupportedContract);
    rejects(validate_connection(input, integer_type(), "clock-A", "clock-A",
                                MissingIntervalPolicy::Skip),
            FailureCode::UnsupportedContract);
}
void time_and_resources() {
    success(validate_context({{"clock", 2}, PointTime{95}, 112, 112}));
    rejects(validate_context({{"clock", 2}, PointTime{95}, 111, 112}), FailureCode::FutureRead);
    rejects(validate_context({{"clock", 2}, Interval{100, 100}, 112, 112}),
            FailureCode::UpdateProtocol);
    rejects(checked_add(std::numeric_limits<std::uint64_t>::max(), 1),
            FailureCode::ResourcePlanExceeded);
    rejects(checked_multiply(std::numeric_limits<std::uint64_t>::max(), 2),
            FailureCode::ResourcePlanExceeded);
    require(take(checked_multiply(3, 8)) == 24, "valid bound calculation failed");
    static_assert(!std::is_same_v<SourceGap, ValidAbsence>);
}
template <class T>
concept HasPhase = requires(T value) { value.phase; };
template <class T>
concept HasRequest = requires(T value) { value.request; };
void schema_and_context() {
    static_assert(!HasPhase<UpdateContext> && !HasRequest<UpdateContext>);
    static_assert(HasPhase<RequestContext> && HasRequest<RequestContext>);
    require(schema_fields("UpdateContext") ==
                "key,extent,decision_time_ns,activation_available_at_ns",
            "numeric context schema drift");
    require(schema_fields("DiagnosticKey") == "clock_ordinal,sequence",
            "failure key must not own strings");
    require(schema_fields("GapV1") == "missing_scope,interval,declared_reason", "gap schema drift");
    require(schema_fields("OperationIdentityV1") ==
                "namespace,name,semantic_version,implementation_selector,binary_digest,"
                "dependencies_digest,source_build_digest,sdk_build_contract_id",
            "operation identity omitted the accepted factory selector");
    require(schema_fields("StepReceipt").find("status") == 0, "host receipt schema missing");
    require(schema_fields("Unknown").empty(), "unknown schema must not be fabricated");
    require(schema_descriptor("DisabledCacheV1") != nullptr &&
                schema_descriptor("Unknown") == nullptr,
            "empty supported record confused with an unknown schema");
}
void typed_factory_ownership() {
    struct Parameters {
        std::int64_t amount;
    };
    auto registry = take(Registry::create());
    auto registered = take(registry.register_type<std::int64_t>(
        {integer_type(),
         StorageKind::ScalarCopy,
         [](const std::int64_t &) -> Result<void> { return ok(); },
         [](const std::int64_t &) -> Result<std::uint64_t> { return sizeof(std::int64_t); },
         FreezeStrategy::ScalarCopy,
         [](const std::int64_t &value) { return value; },
         {}}));
    auto description = operation("source", SharingClass::StatefulDeclared);
    description.inputs.clear();
    TypedOperation<Parameters, int, std::tuple<>, std::tuple<std::int64_t>> source{
        description,
        {},
        {{"output", registered}},
        [](const ParamSet &values) -> Result<Parameters> {
            auto parsed = parse_integer<std::int64_t>(values.values.at(0).text);
            if (const auto *error = std::get_if<Diagnostic>(&parsed)) {
                return *error;
            }
            return Parameters{std::get<std::int64_t>(parsed)};
        },
        [](const Parameters &value) -> Result<CanonicalParams> {
            return CanonicalParams{1, {{"amount", value.amount, Origin::ExplicitConfig, "typed"}}};
        },
        [](const Parameters &, const PreparationFacts &) -> Result<HistoryRequirement> {
            return HistoryRequirement{CountHistory{0}};
        },
        [](const Parameters &, const PreparationFacts &) -> Result<ResourceBounds> {
            return ResourceBounds{0, 4096, 0, 4096, 16, 4096, 0};
        },
        [](const Parameters &, const InitializationSpec &, StateAllocator &) -> Result<int> {
            return 0;
        },
        [](int &state, const Parameters &parameters, const std::tuple<> &, const UpdateContext &,
           PublicationAllocator &) -> StepResult<std::tuple<std::int64_t>> {
            ++state;
            return Published{std::tuple{parameters.amount + state}, ParentSelections::none()};
        },
        [](const Parameters &, const std::tuple<std::int64_t> &,
           const UpdateContext &) -> Result<void> { return ok(); },
        {}};
    success(registry.register_definition(std::move(source)));
    TypedOperation<Parameters, int, std::tuple<std::int64_t>, std::tuple<std::int64_t>> identity{
        operation("identity", SharingClass::StatelessDeterministic),
        {{"input", registered, 0}},
        {{"output", registered}},
        source.parse_params,
        source.canonicalize,
        source.history,
        source.resources,
        source.make_state,
        [](int &, const Parameters &parameters, const std::tuple<ReadValue<std::int64_t>> &inputs,
           const UpdateContext &,
           PublicationAllocator &allocator) -> StepResult<std::tuple<std::int64_t>> {
            auto parents = allocator.parents({std::get<0>(inputs).parent()});
            if (const auto *error = std::get_if<Diagnostic>(&parents)) {
                return *error;
            }
            return Published{std::tuple{std::get<0>(inputs).value + parameters.amount},
                             std::get<ParentSelections>(std::move(parents))};
        },
        source.validate_outputs,
        {}};
    const auto identity_key = identity.contract.key;
    success(registry.register_definition(std::move(identity)));
    auto definition = take(registry.resolve(description.key));
    auto resources = take(ResourceAccount::create(
        {{65536, 65536, 65536, 65536, 65536, 65536, 65536}, 64, 64, 256, 2}));
    const PreparationFacts facts{{"clock", 0, 3},
                                 {contract("empty"), {"clock", 0, 3}, empty_params()},
                                 MissingIntervalPolicy::Strict,
                                 Alignment::SameStep,
                                 {4096, 4096, 4096, 4096, 4096, 8192, 4096},
                                 resources};
    {
        const auto remaining = 65536 - resources->current(ResourceKind::Graph);
        auto occupied = take(resources->reserve(ResourceKind::Graph, remaining));
        rejects(definition->prepare({{{"amount", "5", {"bounded", 1}}}}, facts),
                FailureCode::ResourceExceeded);
    }
    auto prepared = [&] {
        ParamSet temporary{{{"amount", "5", {"temporary", 1}}}};
        return take(definition->prepare(temporary, facts));
    }();
    source.contract.description.clear();
    InstanceServices services{resources, {}};
    auto first = take(prepared.instantiate(services));
    auto second = take(prepared.instantiate(services));
    auto slot = take(ResultSlot::create(
        resources,
        {{integer_type(), std::type_index(typeid(std::int64_t)), StorageKind::ScalarCopy, {}}}, 0,
        Sha256{}, std::nullopt));
    UpdateContext context{{"clock", 0}, PointTime{10}, 10, 10};
    slot->commit(take(validate_pending(take(first.step(context)), *slot, context)));
    require(take(slot->read<std::int64_t>(0, context)).value == 6,
            "typed parameters did not survive source text");
    auto identity_prepared = take(
        take(registry.resolve(identity_key))->prepare({{{"amount", "0", {"typed", 1}}}}, facts));
    InstanceServices identity_services{resources, {{slot, 0, 0}}};
    auto identity_instance = take(identity_prepared.instantiate(identity_services));
    auto identity_slot = take(ResultSlot::create(
        resources,
        {{integer_type(), std::type_index(typeid(std::int64_t)), StorageKind::ScalarCopy, {}}}, 0,
        Sha256{std::byte{1}}, std::nullopt));
    identity_slot->commit(
        take(validate_pending(take(identity_instance.step(context)), *identity_slot, context)));
    require(take(identity_slot->read<std::int64_t>(0, context)).value == 6,
            "typed input binding changed the value");
    require(identity_slot->current()->parents.entries().size() == 1,
            "typed factory dropped selected input provenance");
    auto wrong_type = take(ResultSlot::create(
        resources,
        {{integer_type(), std::type_index(typeid(std::uint64_t)), StorageKind::ScalarCopy, {}}}, 0,
        Sha256{}, std::nullopt));
    InstanceServices wrong_services{resources, {{wrong_type, 0, 0}}};
    rejects(identity_prepared.instantiate(wrong_services), FailureCode::TypeMismatch);
    auto read = take(slot->read<std::int64_t>(0, context));
    require(read.value == 6, "read advanced state");
    rejects(first.step(context), FailureCode::UpdateProtocol);
    context.key.sequence = 1;
    slot->commit(take(validate_pending(take(first.step(context)), *slot, context)));
    require(take(slot->read<std::int64_t>(0, context)).value == 7,
            "persistent per-instance state missing");
    context.key.sequence = 2;
    slot->commit(take(validate_pending(take(second.step(context)), *slot, context)));
    require(take(slot->read<std::int64_t>(0, context)).value == 6,
            "fresh instance shared mutable state");
}
} // namespace
int main() {
    const Case cases[]{{"bounded diagnostic", diagnostics},
                       {"required exact parameters", parameters},
                       {"owned canonical equality", canonical_ownership},
                       {"registry ownership and version", registry},
                       {"units clocks and missing policy", ports_and_clocks},
                       {"time and size bounds", time_and_resources},
                       {"schema and producer context boundary", schema_and_context},
                       {"typed factory ownership and fresh state", typed_factory_ownership}};
    return lexongine::test::run(cases);
}
