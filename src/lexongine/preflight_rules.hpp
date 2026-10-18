#pragma once

#include "lexongine/registry.hpp"

namespace lexongine {
template<class In, class Out>
Result<void> Registry::register_source_projection(SourceRuleContract contract, RegisteredType<In> input, RegisteredType<Out> output,
    Result<Out> (*project)(const In&, const CanonicalParams&, const UpdateContext&), ModulePin pin) noexcept {
    return guarded([&]() -> Result<void> {
        if constexpr (PublicationTraits<Out>::storage != StorageKind::ScalarCopy || !std::is_trivially_copyable_v<Out>) {
            return diagnostic(FailureCode::UnsupportedContract, "first source projection protocol produces a scalar value");
        } else {
            if (!project || !output.policy.validate || !output.policy.measure || !output.policy.freeze ||
                output.policy.storage != StorageKind::ScalarCopy || output.policy.freeze_strategy != FreezeStrategy::ScalarCopy ||
                input.witness.type != typeid(In) || output.witness.type != typeid(Out)) {
                return diagnostic(FailureCode::InvalidConfig, "incomplete typed source projection");
            }
            auto definition = bind_source_rule(contract, pin);
            if (const auto* error = std::get_if<Diagnostic>(&definition)) { return *error; }
            return add_source_projection(std::get<std::shared_ptr<const OperationDefinition>>(std::move(definition)), std::move(contract),
                {input.policy.contract, typeid(In), input.policy.storage, input.policy.pin},
                {output.policy.contract, typeid(Out), output.policy.storage, output.policy.pin}, pin,
                [project, output, pin](const CanonicalParams& parameters, const BoundInput& input, const UpdateContext& context,
                                        std::shared_ptr<ResourceAccount> account) -> Result<PendingOutcome> {
                    auto read = input.slot->template read<In>(input.port, context, 0, pin);
                    if (const auto* error = std::get_if<Diagnostic>(&read)) { return *error; }
                    auto staged = PendingOutcome::create(account, context, 1);
                    if (const auto* error = std::get_if<Diagnostic>(&staged)) { return *error; }
                    auto pending = std::get<PendingOutcome>(std::move(staged));
                    auto value = project(std::get<ReadValue<In>>(read).value, parameters, context);
                    if (const auto* error = std::get_if<Diagnostic>(&value)) { return *error; }
                    PublicationAllocator allocator{std::move(account), 1, {}, pin};
                    auto copied = allocator.freeze(std::get<Out>(value), output);
                    if (const auto* error = std::get_if<Diagnostic>(&copied)) { return *error; }
                    auto erased = ErasedValue::from(std::get<Immutable<Out>>(std::move(copied)), output.policy.contract);
                    if (const auto* error = std::get_if<Diagnostic>(&erased)) { return *error; }
                    auto stored = pending.set_output(0, std::get<ErasedValue>(std::move(erased)));
                    if (const auto* error = std::get_if<Diagnostic>(&stored)) { return *error; }
                    auto parents = allocator.parents({std::get<ReadValue<In>>(read).parent()});
                    if (const auto* error = std::get_if<Diagnostic>(&parents)) { return *error; }
                    auto linked = ProvenanceValidator::attach(pending, std::get<ParentSelections>(std::move(parents)), std::span{&input, 1}, 0, context);
                    if (const auto* error = std::get_if<Diagnostic>(&linked)) { return *error; }
                    return pending;
                });
        }
    });
}
template<class In>
Result<void> Registry::register_source_constraint(SourceRuleContract contract, PortIndex port, RegisteredType<In> input,
    Result<void> (*validate)(const In&, const CanonicalParams&, const UpdateContext&), ModulePin pin) noexcept {
    return guarded([&]() -> Result<void> {
        if (!validate || input.witness.type != typeid(In)) { return diagnostic(FailureCode::InvalidConfig, "incomplete typed source constraint"); }
        auto definition = bind_source_rule(contract, pin);
        if (const auto* error = std::get_if<Diagnostic>(&definition)) { return *error; }
        return add_source_constraint(std::get<std::shared_ptr<const OperationDefinition>>(std::move(definition)), std::move(contract), port,
            {input.policy.contract, typeid(In), input.policy.storage, input.policy.pin}, pin,
            [validate, pin](const CanonicalParams& parameters, const BoundInput& input, const UpdateContext& context) -> Result<void> {
                auto value = input.slot->template read<In>(input.port, context, std::nullopt, pin);
                if (const auto* error = std::get_if<Diagnostic>(&value)) { return *error; }
                return validate(std::get<ReadValue<In>>(value).value, parameters, context);
            });
    });
}
} // namespace lexongine
