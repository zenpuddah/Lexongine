#include "test_plugin_sdk.hpp"
#include "lexongine/build_contract.hpp"

#include <algorithm>

#ifndef LEXONGINE_TEST_PLUGIN_MODE
#error explicit test plugin mode required
#endif

#if LEXONGINE_TEST_PLUGIN_MODE != 7
extern "C" __attribute__((visibility("default"))) int lexongine_probe_v1(char* output, std::uint32_t capacity) noexcept {
#if LEXONGINE_TEST_PLUGIN_MODE == 8
    static_cast<void>(output); static_cast<void>(capacity);
    return 1; // A complete build identifier does not fit this probe's declared contract.
#elif LEXONGINE_TEST_PLUGIN_MODE == 1
    constexpr std::string_view id = "wrong-build";
#elif LEXONGINE_TEST_PLUGIN_MODE == 3
    std::fill_n(output, capacity, 'x');
    return 0; // Deliberately violates the required NUL-termination contract.
#else
    constexpr auto id = lexongine::build_contract_id;
#endif
#if LEXONGINE_TEST_PLUGIN_MODE != 3 && LEXONGINE_TEST_PLUGIN_MODE != 8
    if (capacity <= id.size()) { return 1; }
    std::copy(id.begin(), id.end(), output); output[id.size()] = '\0';
    return 0;
#endif
}
#endif

#if LEXONGINE_TEST_PLUGIN_MODE != 2
extern "C" __attribute__((visibility("default"))) int lexongine_register_v1(lexongine::Registry& registry, lexongine::Diagnostic* error) noexcept {
    using namespace lexongine;
    using namespace lexongine::plugin_test;
#if LEXONGINE_TEST_PLUGIN_MODE == 5
    using Output = OtherPayload;
#else
    using Output = Payload;
#endif
    auto result = guarded([&]() -> Result<void> {
        auto registered = registry.bind_type<Output>({payload_contract(), StorageKind::ImmutableOwned,
            [](const Output& value) -> Result<void> {
                return value.values.size() == 2 ? ok() : Result<void>{diagnostic(FailureCode::OutputInvalid, "test payload arity")};
            },
            [](const Output& value) -> Result<std::uint64_t> {
                auto bytes = checked_multiply(value.values.size(), sizeof(std::int64_t));
                if (const auto* failure = std::get_if<Diagnostic>(&bytes)) { return *failure; }
                return checked_add(sizeof(Output), std::get<std::uint64_t>(bytes));
            }, FreezeStrategy::DeepCopy, [](const Output& value) { return value; }, {}});
        if (const auto* failure = std::get_if<Diagnostic>(&registered)) { return *failure; }
        auto type = std::get<RegisteredType<Output>>(std::move(registered));
        struct Params { std::int64_t value; };
        OperationContract contract{operation_key(), {{}, {}, {}, std::string{build_contract_id}},
            {1, {{"value", ParamKind::Signed64, true, {}}}}, {},
            {{"payload", payload_contract(), StorageKind::ImmutableOwned, protocol("unsupported-cache-codec"), protocol("payload-v1"), false}},
            {SharingClass::StatefulDeclared, protocol("empty"), protocol("same_step"), protocol("none"), true, false, false, {"native-plugin-test"}},
            {"point_v1"}, {Alignment::SameStep}, {MissingIntervalPolicy::Strict}, {"count"}, "native typed test source"};
        // The typed callbacks, state destructor and captured parameters reside in this module.
        TypedOperation<Params, std::int64_t, std::tuple<>, std::tuple<Output>> definition{
            contract, {}, {{"payload", type}},
            [](const ParamSet& values) -> Result<Params> {
                auto value = parse_integer<std::int64_t>(values.values.at(0).text);
                if (const auto* failure = std::get_if<Diagnostic>(&value)) { return *failure; }
                return Params{std::get<std::int64_t>(value)};
            },
            [](const Params& value) -> Result<CanonicalParams> { return CanonicalParams{1, {{"value", value.value, Origin::ExplicitConfig, "plugin"}}}; },
            [](const Params&, const PreparationFacts&) -> Result<HistoryRequirement> { return HistoryRequirement{CountHistory{0}}; },
            [](const Params&, const PreparationFacts&) -> Result<ResourceBounds> { return ResourceBounds{0, 4096, 4096, 4096, 4096, 8192, 0}; },
            [](const Params&, const InitializationSpec&, StateAllocator&) -> Result<std::int64_t> { return 0; },
            // The host binds the declared output policy before this numerical callback.
            [](std::int64_t& state, const Params& parameters, const std::tuple<>&, const UpdateContext&, PublicationAllocator& allocator)
                -> StepResult<std::tuple<Immutable<Output>>> {
                constexpr std::int64_t adjustment = LEXONGINE_TEST_PLUGIN_MODE == 6 ? 100 : 0;
                Output value{{parameters.value + adjustment, ++state}};
                auto published = allocator.copy_output<Output>(0, value);
                if (const auto* failure = std::get_if<Diagnostic>(&published)) { return *failure; }
                return Published{std::tuple{std::get<Immutable<Output>>(std::move(published))}, ParentSelections::none()};
            },
            [](const Params&, const std::tuple<Immutable<Output>>& values, const UpdateContext&) -> Result<void> {
                return std::get<0>(values).get().values.size() == 2 ? ok() : Result<void>{diagnostic(FailureCode::OutputInvalid, "incomplete native output")};
            }, {}};
        auto added = registry.register_definition(std::move(definition));
        if (const auto* failure = std::get_if<Diagnostic>(&added)) { return *failure; }
        auto consumer = contract;
        consumer.key = consumer_key(); consumer.parameters = {1, {}};
        consumer.inputs = {{"payload", payload_contract(), Alignment::SameStep, MissingIntervalPolicy::Strict, AbsencePolicy::RequireValue}};
        consumer.outputs = {{"first", scalar_contract(), StorageKind::ScalarCopy, protocol("unsupported-cache-codec"), protocol("scalar-v1"), false}};
        consumer.sharing.classification = SharingClass::StatelessDeterministic;
        auto scalar_result = registry.bind_type<std::int64_t>({scalar_contract(), StorageKind::ScalarCopy,
            [](const std::int64_t&) -> Result<void> { return ok(); }, [](const std::int64_t&) -> Result<std::uint64_t> { return sizeof(std::int64_t); },
            FreezeStrategy::ScalarCopy, [](const std::int64_t& value) { return value; }, {}});
        if (const auto* failure = std::get_if<Diagnostic>(&scalar_result)) { return *failure; }
        auto scalar = std::get<RegisteredType<std::int64_t>>(std::move(scalar_result));
        auto described = registry.register_definition(std::move(consumer), {}, [](const ParamSet&, const PreparationFacts&) -> Result<PreparedKernel> {
            return diagnostic(FailureCode::UnsupportedContract, "native preflight test does not construct a numerical consumer");
        });
        if (const auto* failure = std::get_if<Diagnostic>(&described)) { return *failure; }
        auto rule = registry.register_source_constraint<Output>({consumer_key(), contract.implementation,
            {"test.payload_nonnegative", 1, {1, {{"min_value", std::int64_t{0}, Origin::VersionedFact, "native test"}}}}}, 0, type,
            [](const Output& value, const CanonicalParams&, const UpdateContext&) -> Result<void> {
                if (value.values.size() != 2 || value.values[0] < 0) { return diagnostic(FailureCode::SourceKnownBoundViolation, "native source-known constraint rejected payload"); }
                return ok();
            }, {});
        if (const auto* failure = std::get_if<Diagnostic>(&rule)) { return *failure; }
        auto projection = registry.register_source_projection<Output, std::int64_t>({consumer_key(), contract.implementation, protocol("test.payload_first")}, type, scalar,
            [](const Output& value, const CanonicalParams&, const UpdateContext&) -> Result<std::int64_t> {
                if (value.values.size() != 2) { return diagnostic(FailureCode::SourceInvalid, "native projection payload shape"); }
                return value.values[0];
            }, {});
        if (const auto* failure = std::get_if<Diagnostic>(&projection)) { return *failure; }
#if LEXONGINE_TEST_PLUGIN_MODE == 4
        return diagnostic(FailureCode::InvalidConfig, "deliberate failure after partial registration");
#else
        return ok();
#endif
    });
    if (const auto* failure = std::get_if<Diagnostic>(&result)) { if (error) { *error = *failure; } return 1; }
    return 0;
}
#endif
