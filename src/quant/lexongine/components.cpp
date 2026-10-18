#include "quant/lexongine/components.hpp"
#include "quant/lexongine/bar_close.hpp"
#include "quant/lexongine/lag_price.hpp"
#include "quant/lexongine/price_difference.hpp"
#include "quant/lexongine/sma.hpp"
#include "quant/data/validation.hpp"

namespace quant::lexongine {
namespace {
using namespace ::lexongine;
using data::Bar;
using data::Price;
struct ComponentFailure { Diagnostic error; };
template<class T> T checked(std::variant<T, Diagnostic> result) {
    if (const auto* error = std::get_if<Diagnostic>(&result)) { throw ComponentFailure{*error}; }
    return std::get<T>(std::move(result));
}
void checked(Result<void> result) { if (const auto* error = std::get_if<Diagnostic>(&result)) { throw ComponentFailure{*error}; } }
template<class F> auto safe(F&& call) noexcept -> decltype(call()) {
    return guarded([&]() -> decltype(call()) { try { return call(); } catch (const ComponentFailure& failure) { return failure.error; } });
}
Contract protocol(std::string name) { return {std::move(name), 1, {1, {}}}; }
Result<Price> project_close(const Bar& value, const CanonicalParams& parameters, const UpdateContext& context) {
    if (parameters.schema_version != 1 || !parameters.values.empty()) { return diagnostic(FailureCode::InvalidConfig, "Bar close has an empty parameter schema"); }
    const auto* interval = std::get_if<Interval>(&context.extent);
    if (!interval || interval->begin_ns != value.interval_start.unix_nanos() || interval->end_ns != value.interval_end.unix_nanos() ||
        context.activation_available_at_ns < interval->end_ns) { return diagnostic(FailureCode::UpdateProtocol, "Bar close requires its complete interval activation"); }
    return bar_close(value);
}
ParameterField text(std::string name, std::string value) {
    ParameterConstraints constraints; constraints.allowed_text.push_back(std::move(value));
    return {std::move(name), ParamKind::Text, true, std::move(constraints)};
}
ParameterSchema sma_schema() {
    ParameterConstraints positive; positive.unsigned_min = 1;
    ParameterConstraints nonnegative; nonnegative.signed_min = 0;
    return {1, {{"window", ParamKind::Unsigned64, true, positive}, {"max_abs_input_raw", ParamKind::Signed64, true, nonnegative},
        text("sum_width", "signed64"), text("rounding", "nearest_ties_even"), text("overflow", "fail")}};
}
ParameterSchema lag_schema() {
    ParameterConstraints positive; positive.unsigned_min = 1;
    return {1, {{"lag_steps", ParamKind::Unsigned64, true, positive}, text("count_basis", "complete_bar_updates")}};
}
template<class T> const T& parameter(const CanonicalParams& values, std::string_view name) {
    const auto found = std::find_if(values.values.begin(), values.values.end(), [&](const auto& value) { return value.name == name; });
    if (found == values.values.end()) { throw ComponentFailure{diagnostic(FailureCode::InvalidConfig, "required component parameter absent")}; }
    return std::get<T>(found->value);
}
Result<void> initialization(const PreparationFacts& facts, std::string_view name) {
    const auto& init = facts.initialization;
    if (init.contract.id != name || init.contract.version != 1 || init.contract.parameters.schema_version != 1 ||
        !init.contract.parameters.values.empty() || init.explicit_values.schema_version != 1 || !init.explicit_values.values.empty() ||
        init.actual_prefix.clock != facts.processing_scope.clock || init.actual_prefix.begin != init.actual_prefix.end ||
        init.actual_prefix.begin != facts.processing_scope.begin || (name == "empty_at_step_0" && facts.processing_scope.begin != 0)) {
        return diagnostic(FailureCode::UnsupportedContract, "unsupported component initialization domain");
    }
    return ok();
}
struct Empty {};
Result<Empty> empty_parameters(const ParamSet& input) { if (!input.values.empty()) { return diagnostic(FailureCode::InvalidConfig, "operation has no parameters"); } return Empty{}; }
Result<CanonicalParams> canonical_empty(const Empty&) { return CanonicalParams{1, {}}; }
Result<Empty> empty_state(const Empty&, const InitializationSpec&, StateAllocator&) { return Empty{}; }
Result<HistoryRequirement> stateless_history(const Empty&, const PreparationFacts& facts) {
    if (auto result = initialization(facts, "stateless"); std::holds_alternative<Diagnostic>(result)) { return std::get<Diagnostic>(result); }
    return HistoryRequirement{CountHistory{0}};
}
Result<ResourceBounds> stateless_resources(const Empty&, const PreparationFacts&) { return ResourceBounds{0, 0, 0, 8192, 4096, 8192, 0}; }
OperationContract description(const ComponentRegistration& build, OperationKey key, ParameterSchema parameters,
    std::vector<InputContract> inputs, OutputContract output, bool stateful, std::string explanation) {
    return {std::move(key), build.implementation, std::move(parameters), std::move(inputs), {std::move(output)},
        {stateful ? SharingClass::StatefulDeclared : SharingClass::StatelessDeterministic,
         protocol(stateful ? "empty_at_step_0" : "stateless"), protocol("same_step"), protocol("none"), true, true, true,
         {"registered-component-tests", "independent-arithmetic-oracle"}},
        {"interval_v1"}, {Alignment::SameStep}, {build.missing_intervals}, {"count"}, std::move(explanation)};
}
InputContract input(std::string name, TypeContract type, MissingIntervalPolicy missing) {
    return {std::move(name), std::move(type), Alignment::SameStep, missing, AbsencePolicy::RequireValue};
}
OutputContract price_output(std::string name) {
    return {std::move(name), price_contract(), StorageKind::ScalarCopy, protocol("qaedalus.price9_xml"), protocol("qaedalus.price9"), true};
}
struct ParentRing {
    Reservation charge;
    std::vector<std::optional<ParentSelection>> entries;
    std::vector<ParentSelection> ordered;
    std::size_t head = 0, count = 0;
    static Result<std::uint64_t> bytes(std::uint64_t length) noexcept {
        auto size = checked_multiply(length, sizeof(std::optional<ParentSelection>) + sizeof(ParentSelection));
        if (const auto* error = std::get_if<Diagnostic>(&size)) { return *error; }
        return checked_add(std::get<std::uint64_t>(size), sizeof(ParentRing) + 256);
    }
    static Result<ParentRing> make(std::uint64_t length, StateAllocator& allocator) noexcept {
        return safe([&]() -> Result<ParentRing> {
            auto charge = checked(allocator.reserve(checked(bytes(length))));
            if (length == 0 || length > std::vector<std::optional<ParentSelection>>{}.max_size() || length > std::vector<ParentSelection>{}.max_size()) {
                return diagnostic(FailureCode::ResourcePlanExceeded, "parent metadata ring cannot represent declared count");
            }
            ParentRing result{std::move(charge), std::vector<std::optional<ParentSelection>>(static_cast<std::size_t>(length)), {}, 0, 0};
            result.ordered.reserve(static_cast<std::size_t>(length)); return result;
        });
    }
    void append(ParentSelection value) noexcept {
        entries[head] = std::move(value); head = (head + 1) % entries.size(); count = std::min(count + 1, entries.size());
    }
    std::span<const ParentSelection> window() {
        ordered.clear();
        for (std::size_t i = 0; i < count; ++i) { ordered.push_back(*entries[(head + i) % entries.size()]); }
        return ordered;
    }
};
struct SmaState { SmaWindow values; ParentRing parents; };
struct LagState { LagWindow values; ParentRing parents; };
ResourceBounds ring_resources(std::uint64_t values, std::uint64_t count, const PreparationFacts& facts) {
    const auto metadata = checked(ParentRing::bytes(count));
    const auto certificates = checked(checked_multiply(count, sizeof(ParentCertificate) + sizeof(ParentSelection) + facts.processing_scope.clock.size() + 512));
    return {0, checked(checked_add(values, metadata)), 0, checked(checked_add(certificates, 8192)), 4096, 8192, 0};
}
}

::lexongine::TypeContract price_contract() {
    return {"qaedalus.price", 1, {1, {{"scale", std::uint64_t{data::Price::canonical_scale}, ::lexongine::Origin::VersionedFact, "Qaedalus canonical Price"}}}};
}
::lexongine::TypeContract bar_contract() {
    return {"qaedalus.bar", 1, {1, {{"price_scale", std::uint64_t{data::Price::canonical_scale}, ::lexongine::Origin::VersionedFact, "Qaedalus"},
        {"quantity_scale", std::uint64_t{data::Quantity::canonical_scale}, ::lexongine::Origin::VersionedFact, "Qaedalus"},
        {"time_unit", std::string{"unix_ns"}, ::lexongine::Origin::VersionedFact, "Qaedalus"}}}};
}
::lexongine::Result<ComponentTypes> register_components(::lexongine::Registry& registry, const ComponentRegistration& build) noexcept {
    return safe([&]() -> Result<ComponentTypes> {
        if (build.implementation.binary_digest == Sha256{} || build.implementation.dependencies_digest == Sha256{} ||
            build.implementation.source_build_digest == Sha256{} || build.implementation.sdk_build_contract_id.empty() ||
            (build.missing_intervals != MissingIntervalPolicy::Strict && build.missing_intervals != MissingIntervalPolicy::Skip)) {
            return diagnostic(FailureCode::InvalidConfig, "components require explicit build identity and missing policy");
        }
        auto price = checked(registry.bind_type<Price>({price_contract(), StorageKind::ScalarCopy,
            [](const Price&) -> Result<void> { return ok(); }, [](const Price&) -> Result<std::uint64_t> { return sizeof(Price); },
            FreezeStrategy::ScalarCopy, [](const Price& value) { return value; }, build.pin}));
        auto bar = checked(registry.bind_type<Bar>({bar_contract(), StorageKind::ImmutableOwned,
            [](const Bar& value) -> Result<void> { return data::validate(value).empty() ? ok() : Result<void>{diagnostic(FailureCode::SourceInvalid, "invalid canonical Bar")}; },
            [](const Bar&) -> Result<std::uint64_t> { return sizeof(Bar); }, FreezeStrategy::DeepCopy, [](const Bar& value) { return value; }, build.pin}));
        TypedOperation<Empty, Empty, std::tuple<Bar>, std::tuple<Price>> close{
            description(build, {"qaedalus", "bar_close", 1, "builtin-v1"}, {1, {}}, {input("bar", bar_contract(), build.missing_intervals)}, price_output("price"), false, "Extract the exact close of an eligible complete Bar"),
            {{"bar", bar, 0}}, {{"price", price}}, empty_parameters, canonical_empty, stateless_history, stateless_resources, empty_state,
            [](Empty&, const Empty&, const std::tuple<ReadValue<Bar>>& inputs, const UpdateContext& context, PublicationAllocator& allocator) -> StepResult<std::tuple<Price>> {
                const auto& value = std::get<0>(inputs); auto projected = project_close(value.value, {1, {}}, context);
                if (const auto* error = std::get_if<Diagnostic>(&projected)) { return *error; }
                auto parents = allocator.parents({value.parent()}); if (const auto* error = std::get_if<Diagnostic>(&parents)) { return *error; }
                return Published{std::tuple{std::get<Price>(projected)}, std::get<ParentSelections>(std::move(parents))};
            }, [](const Empty&, const std::tuple<Price>&, const UpdateContext&) -> Result<void> { return ok(); }, build.pin};
        checked(registry.register_definition(std::move(close)));
        TypedOperation<SmaParameters, SmaState, std::tuple<Price>, std::tuple<Price>> average{
            description(build, {"statistics", "sma_price", 1, "builtin-v1"}, sma_schema(), {input("price", price_contract(), build.missing_intervals)}, price_output("mean"), true, "Checked signed64 exact-input SMA with nearest ties-even Price rounding"),
            {{"price", price, 0}}, {{"mean", price}},
            [](const ParamSet& input) -> Result<SmaParameters> {
                return safe([&]() -> Result<SmaParameters> {
                    const auto values = checked(parse_parameters(input, sma_schema(), 256));
                    SmaParameters result{parameter<std::uint64_t>(values, "window"), parameter<std::int64_t>(values, "max_abs_input_raw"), SumWidth::Signed64, Rounding::NearestTiesEven, Overflow::Fail};
                    checked(validate(result)); return result;
                });
            },
            [](const SmaParameters& value) -> Result<CanonicalParams> {
                return CanonicalParams{1, {{"max_abs_input_raw", value.max_abs_input_raw, Origin::ExplicitConfig, "typed"}, {"overflow", std::string{"fail"}, Origin::ExplicitConfig, "typed"},
                    {"rounding", std::string{"nearest_ties_even"}, Origin::ExplicitConfig, "typed"}, {"sum_width", std::string{"signed64"}, Origin::ExplicitConfig, "typed"}, {"window", value.window, Origin::ExplicitConfig, "typed"}}};
            },
            [](const SmaParameters& value, const PreparationFacts& facts) -> Result<HistoryRequirement> {
                if (auto result = initialization(facts, "empty_at_step_0"); std::holds_alternative<Diagnostic>(result)) { return std::get<Diagnostic>(result); }
                return HistoryRequirement{CountHistory{value.window - 1}};
            },
            [](const SmaParameters& value, const PreparationFacts& facts) -> Result<ResourceBounds> {
                return safe([&]() -> Result<ResourceBounds> { return ring_resources(checked(SmaWindow::state_bytes(value)), value.window, facts); });
            },
            [](const SmaParameters& value, const InitializationSpec&, StateAllocator& allocator) -> Result<SmaState> {
                return safe([&]() -> Result<SmaState> { return SmaState{checked(SmaWindow::make(value, allocator)), checked(ParentRing::make(value.window, allocator))}; });
            },
            [](SmaState& state, const SmaParameters& parameters, const std::tuple<ReadValue<Price>>& inputs, const UpdateContext&,
               PublicationAllocator& allocator) -> StepResult<std::tuple<Price>> {
                const auto& input = std::get<0>(inputs); auto result = state.values.advance(input.value);
                if (const auto* error = std::get_if<Diagnostic>(&result)) { return *error; }
                state.parents.append(input.parent()); const auto& value = std::get<std::optional<Price>>(result);
                if (!value) { return Warming{parameters.window - state.values.count()}; }
                auto parents = allocator.parents(state.parents.window()); if (const auto* error = std::get_if<Diagnostic>(&parents)) { return *error; }
                return Published{std::tuple{*value}, std::get<ParentSelections>(std::move(parents))};
            }, [](const SmaParameters&, const std::tuple<Price>&, const UpdateContext&) -> Result<void> { return ok(); }, build.pin};
        average.contract.timing_protocols.push_back("point_v1");
        checked(registry.register_definition(std::move(average)));
        TypedOperation<LagParameters, LagState, std::tuple<Price>, std::tuple<Price>> lag{
            description(build, {"series", "lag_price", 1, "builtin-v1"}, lag_schema(), {input("price", price_contract(), build.missing_intervals)}, price_output("lagged"), true, "Fixed-count complete-update lag with original parent provenance"),
            {{"price", price, 0}}, {{"lagged", price}},
            [](const ParamSet& input) -> Result<LagParameters> {
                return safe([&]() -> Result<LagParameters> { const auto values = checked(parse_parameters(input, lag_schema(), 256));
                    LagParameters result{parameter<std::uint64_t>(values, "lag_steps"), CountBasis::CompleteBarUpdates}; checked(LagWindow::state_bytes(result)); return result; });
            },
            [](const LagParameters& value) -> Result<CanonicalParams> { return CanonicalParams{1, {{"count_basis", std::string{"complete_bar_updates"}, Origin::ExplicitConfig, "typed"}, {"lag_steps", value.lag_steps, Origin::ExplicitConfig, "typed"}}}; },
            [](const LagParameters& value, const PreparationFacts& facts) -> Result<HistoryRequirement> {
                if (auto result = initialization(facts, "empty_at_step_0"); std::holds_alternative<Diagnostic>(result)) { return std::get<Diagnostic>(result); }
                return HistoryRequirement{CountHistory{value.lag_steps}};
            },
            [](const LagParameters& value, const PreparationFacts& facts) -> Result<ResourceBounds> {
                return safe([&]() -> Result<ResourceBounds> { return ring_resources(checked(LagWindow::state_bytes(value)), value.lag_steps, facts); });
            },
            [](const LagParameters& value, const InitializationSpec&, StateAllocator& allocator) -> Result<LagState> {
                return safe([&]() -> Result<LagState> { return LagState{checked(LagWindow::make(value, allocator)), checked(ParentRing::make(value.lag_steps, allocator))}; });
            },
            [](LagState& state, const LagParameters& parameters, const std::tuple<ReadValue<Price>>& inputs, const UpdateContext& context,
               PublicationAllocator& allocator) -> StepResult<std::tuple<Price>> {
                if (!std::holds_alternative<Interval>(context.extent)) { return diagnostic(FailureCode::UnsupportedContract, "complete-bar lag requires interval timing"); }
                const auto& input = std::get<0>(inputs); auto parent = state.parents.entries[state.parents.head];
                auto result = state.values.advance(input.value); if (const auto* error = std::get_if<Diagnostic>(&result)) { return *error; }
                state.parents.append(input.parent()); const auto& value = std::get<std::optional<Price>>(result);
                if (!value) { return Warming{parameters.lag_steps + 1 - state.values.advances()}; }
                if (!parent) { return diagnostic(FailureCode::OutputInvalid, "lag value lacks its original parent"); }
                auto parents = allocator.parents({*parent}); if (const auto* error = std::get_if<Diagnostic>(&parents)) { return *error; }
                return Published{std::tuple{*value}, std::get<ParentSelections>(std::move(parents))};
            }, [](const LagParameters&, const std::tuple<Price>&, const UpdateContext&) -> Result<void> { return ok(); }, build.pin};
        lag.contract.timing_protocols.push_back("same_step_activation");
        checked(registry.register_definition(std::move(lag)));
        TypedOperation<Empty, Empty, std::tuple<Price, Price>, std::tuple<Price>> difference{
            description(build, {"qaedalus", "price_difference", 1, "builtin-v1"}, {1, {text("overflow", "fail")}},
                {input("left", price_contract(), build.missing_intervals), input("right", price_contract(), build.missing_intervals)}, price_output("difference"), false, "Checked Price subtraction with ordered left/right inputs"),
            {{"left", price, 0}, {"right", price, 0}}, {{"difference", price}},
            [](const ParamSet& input) -> Result<Empty> { auto result = parse_parameters(input, {1, {text("overflow", "fail")}}, 256); if (const auto* error = std::get_if<Diagnostic>(&result)) { return *error; } return Empty{}; },
            [](const Empty&) -> Result<CanonicalParams> { return CanonicalParams{1, {{"overflow", std::string{"fail"}, Origin::ExplicitConfig, "typed"}}}; },
            stateless_history, stateless_resources, empty_state,
            [](Empty&, const Empty&, const std::tuple<ReadValue<Price>, ReadValue<Price>>& inputs, const UpdateContext&, PublicationAllocator& allocator) -> StepResult<std::tuple<Price>> {
                auto value = price_difference(std::get<0>(inputs).value, std::get<1>(inputs).value, Overflow::Fail);
                if (const auto* error = std::get_if<Diagnostic>(&value)) { return *error; }
                auto parents = allocator.parents({std::get<0>(inputs).parent(), std::get<1>(inputs).parent()});
                if (const auto* error = std::get_if<Diagnostic>(&parents)) { return *error; }
                return Published{std::tuple{std::get<Price>(value)}, std::get<ParentSelections>(std::move(parents))};
            }, [](const Empty&, const std::tuple<Price>&, const UpdateContext&) -> Result<void> { return ok(); }, build.pin};
        difference.contract.timing_protocols.push_back("point_v1");
        checked(registry.register_definition(std::move(difference)));
        checked(registry.register_source_projection<Bar, Price>({{"qaedalus", "bar_close", 1, "builtin-v1"}, build.implementation,
            protocol("qaedalus.bar_close.source_projection")}, bar, price, project_close, build.pin));
        checked(registry.register_source_constraint<Price>({{"statistics", "sma_price", 1, "builtin-v1"}, build.implementation,
            protocol("statistics.sma_source_input_bound")}, 0, price,
            [](const Price& value, const CanonicalParams& parameters, const UpdateContext&) -> Result<void> {
                return safe([&]() -> Result<void> {
                    const auto bound = parameter<std::int64_t>(parameters, "max_abs_input_raw");
                    if (bound < 0) { return diagnostic(FailureCode::InvalidConfig, "SMA source bound must be nonnegative"); }
                    if (value.raw() < -bound || value.raw() > bound) { return diagnostic(FailureCode::SourceKnownBoundViolation, "source-known SMA input exceeds declared B"); }
                    return ok();
                });
            }, build.pin));
        return ComponentTypes{std::move(bar), std::move(price)};
    });
}
} // namespace quant::lexongine
