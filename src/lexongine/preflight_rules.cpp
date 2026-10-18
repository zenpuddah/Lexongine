#include "lexongine/registry.hpp"

namespace lexongine {
namespace {
Result<void> binding(const ValueContract& expected, const BoundInput& input, const UpdateContext& context) noexcept {
    if (!input.slot || input.port >= input.slot->contracts().size()) { return diagnostic(FailureCode::TypeMismatch, "source rule input is unbound"); }
    const auto& actual = input.slot->contracts()[input.port];
    if (!(actual.semantic == expected.semantic) || actual.witness != expected.witness || actual.storage != expected.storage) {
        return diagnostic(FailureCode::TypeMismatch, "source rule input semantic/native type mismatch");
    }
    return validate_context(context);
}
}
SourceProjectionDefinition::SourceProjectionDefinition(ModulePin pin, std::shared_ptr<const OperationDefinition> definition,
    SourceRuleContract contract, ValueContract input, ValueContract output, Apply apply)
    : pin_(std::move(pin)), definition_(std::move(definition)), contract_(std::move(contract)), input_(std::move(input)), output_(std::move(output)), apply_(std::move(apply)) {}
SourceProjectionDefinition::~SourceProjectionDefinition() noexcept = default;
Result<PendingOutcome> SourceProjectionDefinition::apply(const CanonicalParams& parameters, const BoundInput& input,
    const UpdateContext& context, std::shared_ptr<ResourceAccount> account) const noexcept {
    return guarded([&]() -> Result<PendingOutcome> {
        if (!account) { return diagnostic(FailureCode::InvalidConfig, "source projection requires a preparation account"); }
        if (auto valid = binding(input_, input, context); std::holds_alternative<Diagnostic>(valid)) { return std::get<Diagnostic>(valid); }
        if (input.prior_outputs != 0 || parameters.schema_version != definition_->contract().parameters.version) {
            return diagnostic(FailureCode::UnsupportedContract, "source projection requires current input and exact parameter schema");
        }
        return apply_(parameters, input, context, std::move(account));
    });
}
SourceConstraintDefinition::SourceConstraintDefinition(ModulePin pin, std::shared_ptr<const OperationDefinition> definition,
    SourceRuleContract contract, PortIndex port, ValueContract input, Apply apply)
    : pin_(std::move(pin)), definition_(std::move(definition)), contract_(std::move(contract)), port_(port), input_(std::move(input)), apply_(std::move(apply)) {}
SourceConstraintDefinition::~SourceConstraintDefinition() noexcept = default;
Result<void> SourceConstraintDefinition::apply(const CanonicalParams& parameters, const BoundInput& input, const UpdateContext& context) const noexcept {
    return guarded([&]() -> Result<void> {
        if (auto valid = binding(input_, input, context); std::holds_alternative<Diagnostic>(valid)) { return std::get<Diagnostic>(valid); }
        if (parameters.schema_version != definition_->contract().parameters.version) { return diagnostic(FailureCode::InvalidConfig, "source constraint parameter schema mismatch"); }
        return apply_(parameters, input, context);
    });
}
Result<std::shared_ptr<const OperationDefinition>> Registry::bind_source_rule(SourceRuleContract& rule, ModulePin& pin) const noexcept {
    return guarded([&]() -> Result<std::shared_ptr<const OperationDefinition>> {
        if (frozen_) { return diagnostic(FailureCode::InvalidConfig, "source rule registration is frozen"); }
        auto resolved = resolve(rule.operation);
        if (const auto* error = std::get_if<Diagnostic>(&resolved)) { return *error; }
        auto definition = std::get<std::shared_ptr<const OperationDefinition>>(std::move(resolved));
        if (module_identity_) {
            rule.implementation = *module_identity_;
            auto combined = combine_pins(module_account_, module_pin_, pin);
            if (const auto* error = std::get_if<Diagnostic>(&combined)) { return *error; }
            pin = std::get<ModulePin>(std::move(combined));
        } else if (!pin) { pin = definition->pin_; }
        if (rule.implementation != definition->contract().implementation) { return diagnostic(FailureCode::SourceChanged, "source rule build differs from its operation implementation"); }
        if (rule.validation.id.empty() || rule.validation.version == 0 || rule.validation.parameters.schema_version == 0) {
            return diagnostic(FailureCode::InvalidConfig, "source rule validation contract is incomplete");
        }
        const auto maximum = module_pin_ ? registration_remaining_ : std::numeric_limits<std::uint64_t>::max();
        auto text = validate_text(rule.validation.id, maximum, false);
        if (const auto* error = std::get_if<Diagnostic>(&text)) { return *error; }
        auto canonical = canonical_params(rule.validation.parameters.schema_version, rule.validation.parameters.values, maximum);
        if (const auto* error = std::get_if<Diagnostic>(&canonical)) { return *error; }
        if (!(std::get<CanonicalParams>(canonical) == rule.validation.parameters)) { return diagnostic(FailureCode::InvalidConfig, "source rule parameters are not canonical"); }
        return definition;
    });
}
Result<void> Registry::check_rule_type(const ValueContract& type) const noexcept {
    for (const auto& registered : types_) {
        if (registered.semantic == type.semantic) {
            if (registered.witness != type.witness || registered.storage != type.storage) {
                return diagnostic(FailureCode::TypeMismatch, "source rule native type differs from registry witness");
            }
            return ok();
        }
    }
    return diagnostic(FailureCode::UnknownDefinition, "source rule type is not registered");
}
Result<void> Registry::consume_rule_registration(const SourceRuleContract& rule, const ValueContract& input, const ValueContract* output) noexcept {
    return guarded([&]() -> Result<void> {
        std::uint64_t bytes = sizeof(SourceProjectionDefinition) + sizeof(SourceConstraintDefinition) + 1024;
        auto add = [&](std::uint64_t amount) -> Result<void> {
            auto sum = checked_add(bytes, amount); if (const auto* error = std::get_if<Diagnostic>(&sum)) { return *error; }
            bytes = std::get<std::uint64_t>(sum); return ok();
        };
        for (const auto* type : {&input, output}) {
            if (!type) { continue; }
            auto measured = type_storage_bytes(type->semantic); if (const auto* error = std::get_if<Diagnostic>(&measured)) { return *error; }
            if (auto result = add(std::get<std::uint64_t>(measured)); std::holds_alternative<Diagnostic>(result)) { return std::get<Diagnostic>(result); }
        }
        const TypeContract validation{rule.validation.id, rule.validation.version, rule.validation.parameters};
        auto measured = type_storage_bytes(validation); if (const auto* error = std::get_if<Diagnostic>(&measured)) { return *error; }
        if (auto result = add(std::get<std::uint64_t>(measured)); std::holds_alternative<Diagnostic>(result)) { return std::get<Diagnostic>(result); }
        for (const auto* text : {&rule.operation.name_space, &rule.operation.name, &rule.operation.implementation_selector, &rule.implementation.sdk_build_contract_id}) {
            if (auto result = add(text->size() + 32); std::holds_alternative<Diagnostic>(result)) { return std::get<Diagnostic>(result); }
        }
        auto copied = checked_multiply(bytes, 4); if (const auto* error = std::get_if<Diagnostic>(&copied)) { return *error; }
        return consume_registration(std::get<std::uint64_t>(copied));
    });
}
Result<void> Registry::add_source_projection(std::shared_ptr<const OperationDefinition> definition, SourceRuleContract rule,
    ValueContract input, ValueContract output, ModulePin pin, SourceProjectionDefinition::Apply apply) noexcept {
    return guarded([&]() -> Result<void> {
        if (frozen_) { return diagnostic(FailureCode::InvalidConfig, "source rule registration is frozen"); }
        const auto& operation = definition->contract();
        if (operation.inputs.size() != 1 || operation.outputs.size() != 1 || operation.sharing.classification != SharingClass::StatelessDeterministic ||
            !operation.sharing.effect_free || !(operation.inputs[0].type == input.semantic) || !(operation.outputs[0].type == output.semantic) ||
            operation.outputs[0].storage != output.storage) { return diagnostic(FailureCode::UnsupportedContract, "source projection requires a matching unary stateless operation"); }
        for (const auto* value : {&input, &output}) { if (auto valid = check_rule_type(*value); std::holds_alternative<Diagnostic>(valid)) { return std::get<Diagnostic>(valid); } }
        if (source_projection(rule.operation)) { return diagnostic(FailureCode::DuplicateDefinition, "source projection already registered for operation"); }
        auto budget = consume_rule_registration(rule, input, &output); if (const auto* error = std::get_if<Diagnostic>(&budget)) { return *error; }
        source_projections_.push_back(std::shared_ptr<const SourceProjectionDefinition>(new SourceProjectionDefinition{
            std::move(pin), std::move(definition), std::move(rule), std::move(input), std::move(output), std::move(apply)}));
        return ok();
    });
}
Result<void> Registry::add_source_constraint(std::shared_ptr<const OperationDefinition> definition, SourceRuleContract rule, PortIndex port,
    ValueContract input, ModulePin pin, SourceConstraintDefinition::Apply apply) noexcept {
    return guarded([&]() -> Result<void> {
        if (frozen_) { return diagnostic(FailureCode::InvalidConfig, "source rule registration is frozen"); }
        if (port >= definition->contract().inputs.size()) { return diagnostic(FailureCode::InvalidConfig, "source constraint input port is undeclared"); }
        if (!(definition->contract().inputs[port].type == input.semantic)) { return diagnostic(FailureCode::TypeMismatch, "source constraint interpretation differs from input"); }
        auto type = check_rule_type(input); if (const auto* error = std::get_if<Diagnostic>(&type)) { return *error; }
        for (const auto& existing : source_constraints_) {
            if (existing->contract().operation == rule.operation && existing->input_port() == port && existing->contract().validation.id == rule.validation.id &&
                existing->contract().validation.version == rule.validation.version) { return diagnostic(FailureCode::DuplicateDefinition, "source constraint already registered"); }
        }
        auto budget = consume_rule_registration(rule, input, nullptr); if (const auto* error = std::get_if<Diagnostic>(&budget)) { return *error; }
        source_constraints_.push_back(std::shared_ptr<const SourceConstraintDefinition>(new SourceConstraintDefinition{
            std::move(pin), std::move(definition), std::move(rule), port, std::move(input), std::move(apply)}));
        return ok();
    });
}
std::shared_ptr<const SourceProjectionDefinition> Registry::source_projection(const OperationKey& key) const noexcept {
    for (const auto& entry : source_projections_) { if (entry->contract().operation == key) { return entry; } }
    return {};
}
Result<Contract> Registry::source_validation_contract(const OperationKey& key, std::uint64_t maximum) const noexcept {
    return guarded([&]() -> Result<Contract> {
        auto resolved = resolve(key); if (const auto* error = std::get_if<Diagnostic>(&resolved)) { return *error; }
        const auto& definition = std::get<std::shared_ptr<const OperationDefinition>>(resolved)->contract();
        std::vector<NamedParam> values;
        auto add = [&](std::string name, ParamValue value) { values.push_back({std::move(name), std::move(value), Origin::Derived, "registered validation contracts"}); };
        auto append = [&](const std::string& prefix, const Contract& contract) {
            add(prefix + ".id", contract.id); add(prefix + ".version", static_cast<std::uint64_t>(contract.version));
            add(prefix + ".parameter_schema", static_cast<std::uint64_t>(contract.parameters.schema_version));
            for (const auto& parameter : contract.parameters.values) { add(prefix + ".parameter." + parameter.name, parameter.value); }
        };
        add("output_count", static_cast<std::uint64_t>(definition.outputs.size()));
        for (std::size_t i = 0; i < definition.outputs.size(); ++i) {
            const auto prefix = "output." + std::to_string(i);
            add(prefix + ".name", definition.outputs[i].name); append(prefix + ".validation", definition.outputs[i].validation);
        }
        const auto projection = source_projection(key); add("projection_count", std::uint64_t{projection ? 1U : 0U});
        if (projection) { append("projection.validation", projection->contract().validation); }
        std::uint64_t count = 0;
        for (const auto& constraint : source_constraints_) {
            if (!(constraint->contract().operation == key)) { continue; }
            const auto prefix = "constraint." + std::to_string(count++);
            add(prefix + ".input", static_cast<std::uint64_t>(constraint->input_port())); append(prefix + ".validation", constraint->contract().validation);
        }
        add("constraint_count", count);
        auto parameters = canonical_params(1, std::move(values), maximum); if (const auto* error = std::get_if<Diagnostic>(&parameters)) { return *error; }
        return Contract{"lexongine.registered_validation_set", 1, std::get<CanonicalParams>(std::move(parameters))};
    });
}
} // namespace lexongine
