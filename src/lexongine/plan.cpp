#include "lexongine/plan.hpp"
#include "lexongine/result_store.hpp"
#include "lexongine/execution.hpp"

namespace lexongine {
namespace {
struct PreparationFailure { Diagnostic error; };
template<class T> T checked(std::variant<T, Diagnostic> result) {
    if (const auto* error = std::get_if<Diagnostic>(&result)) { throw PreparationFailure{*error}; }
    return std::get<T>(std::move(result));
}
[[noreturn]] void fail(FailureCode code, std::string_view text) { throw PreparationFailure{diagnostic(code, text)}; }
Contract protocol(std::string name) { return {std::move(name), 1, {1, {}}}; }
std::array<std::uint64_t, 7> fields(const ResourceBounds& b) { return {b.source, b.state, b.scratch, b.staging, b.retention, b.graph, b.evidence}; }
void add(ResourceBounds& a, const ResourceBounds& b) {
    a.source = checked(checked_add(a.source, b.source)); a.state = checked(checked_add(a.state, b.state));
    a.scratch = checked(checked_add(a.scratch, b.scratch)); a.staging = checked(checked_add(a.staging, b.staging));
    a.retention = checked(checked_add(a.retention, b.retention)); a.graph = checked(checked_add(a.graph, b.graph));
    a.evidence = checked(checked_add(a.evidence, b.evidence));
}
bool budget_failure(const Diagnostic& d) { return d.code == FailureCode::ResourceExceeded || d.code == FailureCode::ResourcePlanExceeded || d.code == FailureCode::EvidenceQuota; }
Contract selection(const SourceRequest& request, std::uint64_t strings) {
    std::vector<NamedParam> values{{"provider", request.provider_name, Origin::Derived, "source request"},
        {"policy_schema", static_cast<std::uint64_t>(request.provider_policies.schema_version), Origin::Derived, "source request"}};
    for (const auto& value : request.provider_policies.values) { values.push_back({"policy." + value.name, value.value, value.origin, value.provenance}); }
    return {"lexongine.source_request", 1, checked(canonical_params(1, std::move(values), strings))};
}
}
struct PreparedPlan::Data {
    Reservation charge;
    std::shared_ptr<const CompositionDeclaration> configuration;
    std::vector<GraphNode> nodes;
    FrozenGraph graph;
    SourceSnapshot source;
    std::vector<std::optional<PreparedKernel>> kernels;
    std::vector<std::vector<ValueContract>> outputs;
    std::vector<PreparedObservation> observations;
    std::vector<RequestRejection> rejections;
    std::vector<bool> needed;
    std::vector<std::uint64_t> retention, preceding;
    std::vector<Sha256> identities;
    std::vector<std::optional<SequenceScope>> effects;
    AccountLimits runtime;
    ResourceBounds required;
    std::uint64_t metadata_bytes;
    std::shared_ptr<ResourceAccount> preparation_account{};
    std::shared_ptr<ResultStore> store{};
    std::vector<std::optional<ArtifactIdentity>> artifact_identities{};
    std::vector<std::optional<ValidatedArtifact>> artifacts{};
    std::vector<std::uint64_t> certificate_history{};
    ResourceBounds future_store{};
    bool admitted = false;
    std::vector<RequiredEvidenceSpec> evidence{};
    std::shared_ptr<ResourceAccount> evidence_account{};
    ResourceBounds evidence_resources{};
    std::uint64_t report_bytes = 0;
    std::vector<std::string_view> reuse_decisions{};
};
class PreparationAccess {
public:
    static std::shared_ptr<const CompositionDeclaration> configuration(const Composition& c) { return c.declaration_; }
    static std::shared_ptr<const PreparedPlan> finish(PreparedPlan::Data data, std::shared_ptr<ResultStore> store, std::shared_ptr<ResourceAccount> account, const PrepareOptions& options) {
        auto owned = std::make_shared<PreparedPlan::Data>(std::move(data)); owned->preparation_account = std::move(account);
        owned->evidence = options.evidence; owned->evidence_account = options.evidence_account; owned->evidence_resources = options.evidence_resources;
        const auto base_report = checked(checked_add(checked(checked_add(owned->metadata_bytes,4096)),checked(checked_multiply(owned->runtime.max_holders,sizeof(RetentionEvent)))));
        auto extra_report = checked(checked_add(4096,checked(checked_multiply(owned->nodes.size(),sizeof(LogicalReach)+sizeof(EntryReference)+1024))));
        for(const auto& spec:owned->evidence){extra_report=checked(checked_add(extra_report,sizeof(RequiredEvidenceSpec)+spec.name.size()+spec.format.size()+spec.absolute_path.size()+spec.open_policy.size()+spec.flush_policy.size()+spec.failure.size()+256));}
        owned->report_bytes=checked(checked_add(base_report,extra_report)); owned->required.evidence=checked(checked_add(owned->required.evidence,extra_report));
        auto plan = std::shared_ptr<const PreparedPlan>(new PreparedPlan{owned});
        if (std::holds_alternative<LocalCacheConfiguration>(owned->configuration->cache)) {
            owned->store = std::move(store); const auto count = owned->nodes.size();
            owned->required.evidence = checked(checked_add(owned->required.evidence, owned->store->limits().identity.string_bytes));
            owned->report_bytes=checked(checked_add(owned->report_bytes,owned->store->limits().identity.string_bytes));
            owned->artifact_identities.resize(count); owned->artifacts.resize(count); owned->certificate_history.resize(count);owned->reuse_decisions.resize(count,"ineligible");
            for (std::uint32_t i = 0; i < count; ++i) {
                if (!owned->needed[i]) { continue; }
                const auto local = std::get<CountHistory>(owned->nodes[i].facts.history).preceding;
                for (std::size_t p = 0; p < owned->graph.inputs(i).size(); ++p) {
                    const auto producer = owned->graph.representative(owned->graph.inputs(i)[p].producer);
                    owned->certificate_history[producer] = std::max(owned->certificate_history[producer], std::max(local, owned->nodes[i].spec.inputs[p].prior_outputs));
                }
            }
            for (const auto i : owned->graph.physical_order()) {
                if (!owned->needed[i]) { continue; }
                owned->required.state = checked(checked_add(owned->required.state, checked(PublicationHistory::resource_bytes(owned->outputs[i].size(), owned->certificate_history[i]))));
                if (!checked(owned->store->supports(*plan, i))) { continue; }
                owned->artifact_identities[i] = checked(owned->store->identity(*plan, i));
                auto found = checked(owned->store->lookup(*owned->artifact_identities[i]));
                if (auto* candidate = std::get_if<ArtifactCandidate>(&found)) {
                    owned->reuse_decisions[i]="hit";
                    owned->artifacts[i] = checked(owned->store->validate(std::move(*candidate), *owned->artifact_identities[i], *plan, i));
                    owned->required.state = checked(checked_add(owned->required.state, owned->store->limits().cursor_bytes));
                } else {
                    const auto reason=std::get<CacheMiss>(found).reason;
                    owned->reuse_decisions[i]=reason==CacheMissReason::Absent?"miss_absent":reason==CacheMissReason::Incompatible?"miss_incompatible":"miss_unaccepted";
                    auto extra = checked(owned->store->staging_resources(*plan, i));
                    const auto scratch = extra.scratch; extra.scratch = 0;
                    add(owned->future_store, extra); owned->future_store.scratch = std::max(owned->future_store.scratch, scratch);
                    owned->future_store.graph = checked(checked_add(owned->future_store.graph, owned->store->limits().writer_bytes));
                }
            }
        }
        const auto need = fields(owned->required), available = fields(owned->runtime.bytes);
        for (std::size_t i = 0; i < need.size(); ++i) { if (need[i] > available[i]) { fail(FailureCode::ResourcePlanExceeded, "prepared execution/report requirements exceed runtime bounds"); } }
        owned->admitted = true;
        auto admitted = plan->admit_instance();
        if (const auto* error = std::get_if<Diagnostic>(&admitted)) { owned->admitted = false; throw PreparationFailure{*error}; }
        return plan;
    }
    static Result<PrepareResult> run(const Composition&, const Registry&, SourceResolver&, const PrepareOptions&, std::shared_ptr<ResultStore>);
};
Result<PrepareResult> prepare(const Composition& composition, const Registry& registry, SourceResolver& sources,
    std::shared_ptr<ResultStore> store, const PrepareOptions& options) noexcept {
    return guarded([&]() -> Result<PrepareResult> {
        try { return PreparationAccess::run(composition, registry, sources, options, std::move(store)); }
        catch (const PreparationFailure& failure) { return failure.error; }
    });
}
Result<PrepareResult> PreparationAccess::run(const Composition& composition, const Registry& registry, SourceResolver& sources, const PrepareOptions& options, std::shared_ptr<ResultStore> store) {
    const auto& config = composition.declaration(); const auto& limits = config.limits;
    if (!registry.frozen() || !options.source_services.account || !options.digest || options.run_id != config.run.id) { fail(FailureCode::InvalidConfig, "preparation needs frozen definitions, explicit services and matching run ID"); }
    if(options.evidence.size()>limits.nodes||(!options.evidence.empty()&&(!options.evidence_account||checked(options.evidence_resources.total())==0))){fail(FailureCode::InvalidConfig,"required evidence needs explicit resources/account");}
    std::size_t snapshots=0;
    for(std::size_t i=0;i<options.evidence.size();++i){const auto& spec=options.evidence[i];
        for(const auto* text:{&spec.name,&spec.format,&spec.absolute_path}){static_cast<void>(checked(validate_text(*text,limits.string_bytes,false)));}
        if(spec.name=="retention"||spec.absolute_path==config.run.retention_log.path||!spec.absolute_path.starts_with('/')||spec.open_policy!="create_new"||spec.failure!="stop"||
            (spec.flush_policy!="on_event"&&spec.flush_policy!="on_close")||!spec.max_bytes||!spec.max_records){fail(FailureCode::InvalidConfig,"invalid required evidence specification");}
        if(spec.format=="run_snapshot_v1"&&++snapshots>1){fail(FailureCode::InvalidConfig,"multiple required run snapshots");}
        for(std::size_t j=0;j<i;++j){if(options.evidence[j].name==spec.name||options.evidence[j].absolute_path==spec.absolute_path){fail(FailureCode::InvalidConfig,"duplicate required evidence name/path");}}
    }
    if (const auto* cache = std::get_if<LocalCacheConfiguration>(&config.cache); cache && (!store || store->configuration() != *cache)) {
        fail(FailureCode::InvalidConfig, "enabled cache requires an exactly configured result-store service");
    }
    if (config.sources.size() != 1 || composition.source_parameters().size() != 1 || config.observations.empty()) {
        fail(FailureCode::UnsupportedContract, "first integrated plan requires one declared source and observations");
    }
    const auto& declaration = config.sources[0]; const auto scope = declaration.scope;
    if (scope.clock != config.run.clock || options.source.required_scope != scope || options.source.requested_version != declaration.source_version ||
        options.source_services.missing_intervals != declaration.missing_intervals || options.runtime_limits.retention_warning_steps != config.run.retention_warning_steps) {
        fail(FailureCode::InvalidConfig, "preparation source/policy/clock differs from effective configuration");
    }
    if (config.run.execution != "synchronous" || config.run.failure != "stop" || (config.run.sharing != "exact" && config.run.sharing != "disabled")) {
        fail(FailureCode::UnsupportedContract, "unsupported execution protocol");
    }
    if (config.nodes.size() + 1 > limits.nodes || config.observations.size() > limits.requests || scope.end < scope.begin || scope.end - scope.begin > limits.source_updates ||
        checked(options.runtime_limits.bytes.total()) > limits.managed_buffer_bytes || options.metadata_bytes > limits.managed_buffer_bytes) {
        fail(FailureCode::ResourcePlanExceeded, "submitted plan exceeds declared global limits");
    }
    auto account = options.source_services.account;
    const auto count = config.nodes.size() + 1;
    const auto minimum_metadata = checked(checked_add(8192, checked(checked_multiply(count + config.observations.size(), 2048))));
    if (minimum_metadata > options.metadata_bytes) { fail(FailureCode::ResourcePlanExceeded, "plan metadata ceiling is too small"); }
    auto charge = checked(account->reserve(ResourceKind::Graph, options.metadata_bytes));
    auto result_charge = checked(account->reserve(ResourceKind::Graph, checked(checked_add(512, checked(checked_multiply(config.observations.size(), 1024))))));
    std::vector<GraphNode> nodes; nodes.reserve(count);
    auto append = [&](const LogicalId& id, DeclaredOrdinal ordinal, const ResolvedParameters& resolved, std::vector<InputBindingSpec> inputs) {
        const auto& definition = resolved.definition->contract();
        NodeSpec spec{id, id, ordinal, definition.key, resolved.parameters, std::move(inputs),
            {definition.sharing.initialization, {scope.clock, scope.begin, scope.begin}, {1, {}}}, definition.sharing.update,
            protocol("max_inputs_activation"), definition.sharing.effect};
        nodes.push_back({std::move(spec), resolved.definition, {scope, CountHistory{0}, {}, protocol("declared_operation_parameters"),
            declaration.missing_intervals, protocol("lexongine.output_bundle_v1"), checked(registry.source_validation_contract(definition.key, limits.string_bytes))}});
    };
    append(declaration.id, declaration.ordinal, composition.source_parameters()[0], {});
    for (std::size_t i = 0; i < config.nodes.size(); ++i) {
        const auto& node = config.nodes[i]; const auto& resolved = composition.node_parameters()[i];
        const auto& contract = resolved.definition->contract();
        const bool declared_activation = node.alignment == "same_step_activation" &&
            std::find(contract.timing_protocols.begin(), contract.timing_protocols.end(), "same_step_activation") != contract.timing_protocols.end();
        if (node.initialization != contract.sharing.initialization.id || (node.alignment != "same_step" && !declared_activation)) { fail(FailureCode::UnsupportedContract, "node initialization/alignment differs from registered contract"); }
        append(node.id, node.ordinal, resolved, node.inputs);
    }
    GraphLimits graph_limits{limits.nodes, limits.edges, options.identity_bytes, limits.string_bytes, config.run.sharing == "exact"};
    auto logical = checked(build_logical_graph(nodes, graph_limits, account));
    auto source_services = options.source_services;
    auto candidate = checked(sources.inspect(options.source, source_services)); const auto source_bounds = candidate.bounds();
    if (checked(source_bounds.resources.total()) > limits.managed_buffer_bytes) { fail(FailureCode::ResourcePlanExceeded, "source bounds exceed global plan budget"); }
    auto snapshot_result = sources.validate(std::move(candidate), options.source, source_services);
    PrepareResult result{std::move(result_charge), {}, {}}; result.rejected.reserve(config.observations.size());
    if (const auto* error = std::get_if<Diagnostic>(&snapshot_result)) {
        if (budget_failure(*error)) { return *error; }
        for (const auto& request : config.observations) { result.rejected.push_back({request.id, *error, {declaration.id}}); }
        return result;
    }
    auto snapshot = std::get<SourceSnapshot>(std::move(snapshot_result)); const auto& metadata = snapshot.metadata();
    if (metadata.provider_implementation != nodes[0].definition->contract().implementation || nodes[0].definition->contract().outputs.size() != 1 ||
        !(metadata.interpretation == nodes[0].definition->contract().outputs[0].type)) { fail(FailureCode::SourceChanged, "source facts differ from the registered operation/build"); }
    nodes[0].facts.sources.push_back({metadata.snapshot_name, metadata.version, metadata.content_digest, nodes[0].spec.operation,
        metadata.provider_implementation, metadata.interpretation, selection(options.source, limits.string_bytes), metadata.source_reference_digest,
        metadata.validated_scope, metadata.coverage, metadata.order, metadata.availability, metadata.validation});
    // Numerical roots still observe this source's activation context and gaps.
    // Record that control dependency without inventing a numerical input port.
    for (std::uint32_t i = 1; i < nodes.size(); ++i) {
        if (logical.inputs(i).empty()) { nodes[i].facts.sources = nodes[0].facts.sources; }
    }
    std::vector<std::optional<PreparedKernel>> kernels(count);
    std::vector<std::optional<Diagnostic>> failures(count);
    std::vector<std::vector<ValueContract>> outputs(count);
    std::vector<std::optional<SequenceScope>> effects(count);
    for (const auto& effect : options.effects) {
        const auto found = std::find_if(nodes.begin(), nodes.end(), [&](const auto& node) { return node.spec.id == effect.node; });
        if (found == nodes.end()) { fail(FailureCode::InvalidConfig, "effect scope names an undeclared node"); }
        const auto i = static_cast<std::size_t>(found - nodes.begin());
        if (effects[i] || found->definition->contract().sharing.classification != SharingClass::Unique || effect.evaluation.clock != scope.clock ||
            effect.evaluation.begin < scope.begin || effect.evaluation.end > scope.end || effect.evaluation.end <= effect.evaluation.begin) { fail(FailureCode::InvalidConfig, "invalid explicit effect scope"); }
        effects[i] = effect.evaluation;
    }
    for (const auto i : logical.logical_order()) {
        const auto& contract = nodes[i].definition->contract();
        for (const auto& output : contract.outputs) { outputs[i].push_back(checked(registry.resolve_type(output.type))); }
        if (i == 0) { continue; }
        if (contract.sharing.classification == SharingClass::Unique && !effects[i]) { fail(FailureCode::InvalidConfig, "unique effect needs an explicit evaluation scope"); }
        for (const auto& edge : logical.inputs(i)) { if (failures[edge.producer]) { failures[i] = failures[edge.producer]; break; } }
        if (failures[i]) { continue; }
        // Source timing-contract names are provider semantics; numerical timing
        // capabilities describe the actual complete activation's extent kind.
        // A gap interval is control evidence and never a numerical activation.
        for (const auto& event : snapshot.timeline()) {
            if (!std::holds_alternative<CompleteRecord>(event.delivery)) { continue; }
            const std::string_view timing = std::holds_alternative<PointTime>(event.extent) ? "point_v1" : "interval_v1";
            if (std::find(contract.timing_protocols.begin(), contract.timing_protocols.end(), timing) == contract.timing_protocols.end()) {
                failures[i] = diagnostic(FailureCode::UnsupportedContract, "operation does not support the source activation extent kind");
                failures[i]->logical_ordinal = nodes[i].spec.ordinal;
                break;
            }
        }
        if (failures[i]) { continue; }
        const PreparationFacts facts{scope, nodes[i].spec.initialization, declaration.missing_intervals, Alignment::SameStep, options.kernel_limits, account};
        auto prepared = nodes[i].definition->prepare(config.nodes[i - 1].parameters, facts);
        if (const auto* error = std::get_if<Diagnostic>(&prepared)) {
            if (budget_failure(*error)) { return *error; }
            failures[i] = *error; failures[i]->logical_ordinal = nodes[i].spec.ordinal; continue;
        }
        kernels[i].emplace(std::get<PreparedKernel>(std::move(prepared))); nodes[i].facts.history = kernels[i]->history();
        nodes[i].spec.parameters = kernels[i]->parameters();
    }
    // Current-value validation slots carry no execution/computation identity. They
    // never escape preparation or enter artifact publication; callbacks see only
    // typed values, resolved parameters and timing, not these temporary authorities.
    std::vector<std::shared_ptr<ResultSlot>> known(count);
    std::vector<std::shared_ptr<const SourceProjectionDefinition>> projections(count);
    known[0] = checked(ResultSlot::create(account, outputs[0], 0, {}, metadata.source_reference_digest));
    for (const auto i : logical.logical_order()) {
        if (i == 0 || failures[i]) { continue; }
        projections[i] = registry.source_projection(nodes[i].spec.operation);
        if (projections[i] && logical.inputs(i).size() == 1 && known[logical.inputs(i)[0].producer]) {
            known[i] = checked(ResultSlot::create(account, outputs[i], 0, {}, metadata.source_reference_digest));
        }
    }
    {
        PublicationAllocator allocator{account, 0}; auto cursor = checked(snapshot.cursor(allocator));
        auto decision = std::numeric_limits<std::int64_t>::min();
        while (const auto* descriptor = cursor.descriptor()) {
            decision = std::max(decision, descriptor->available_at_ns);
            UpdateContext context{descriptor->key, descriptor->extent, decision, descriptor->available_at_ns};
            auto event = checked(cursor.next(context)); if (std::holds_alternative<SourceGap>(event)) { continue; }
            known[0]->commit(checked(validate_pending(std::get<PendingOutcome>(std::move(event)), *known[0], context)));
            for (const auto i : logical.logical_order()) {
                if (i == 0 || failures[i]) { continue; }
                for (const auto& edge : logical.inputs(i)) { if (failures[edge.producer]) { failures[i] = failures[edge.producer]; break; } }
                if (failures[i]) { continue; }
                for (const auto& constraint : registry.source_constraints()) {
                    if (!(constraint->contract().operation == nodes[i].spec.operation)) { continue; }
                    const auto& edge = logical.inputs(i)[constraint->input_port()]; if (!known[edge.producer]) { continue; }
                    auto valid = constraint->apply(nodes[i].spec.parameters, {known[edge.producer], edge.output, 0}, context);
                    if (const auto* error = std::get_if<Diagnostic>(&valid)) {
                        if (budget_failure(*error)) { return *error; }
                        failures[i] = *error; failures[i]->logical_ordinal = nodes[i].spec.ordinal; failures[i]->key = DiagnosticKey{0, context.key.sequence}; break;
                    }
                }
                if (failures[i] || !known[i]) { continue; }
                const auto edge = logical.inputs(i)[0]; auto value = projections[i]->apply(nodes[i].spec.parameters, {known[edge.producer], edge.output, 0}, context, account);
                if (const auto* error = std::get_if<Diagnostic>(&value)) {
                    if (budget_failure(*error)) { return *error; }
                    failures[i] = *error; failures[i]->logical_ordinal = nodes[i].spec.ordinal; failures[i]->key = DiagnosticKey{0, context.key.sequence}; continue;
                }
                known[i]->commit(checked(validate_pending(std::get<PendingOutcome>(std::move(value)), *known[i], context)));
            }
        }
        checked(cursor.finish());
    }
    known.clear(); projections.clear();
    for (const auto i : logical.logical_order()) { for (const auto& edge : logical.inputs(i)) { if (failures[edge.producer] && !failures[i]) { failures[i] = failures[edge.producer]; } } }
    for (std::size_t i = 0; i < effects.size(); ++i) {
        if (effects[i] && failures[i]) { return *failures[i]; }
    }
    std::vector<std::uint32_t> remap(count, std::numeric_limits<std::uint32_t>::max());
    std::vector<GraphNode> valid_nodes; std::vector<std::optional<PreparedKernel>> valid_kernels;
    std::vector<std::vector<ValueContract>> valid_outputs; std::vector<std::optional<SequenceScope>> valid_effects;
    for (std::size_t i = 0; i < count; ++i) {
        if (failures[i]) { continue; } remap[i] = static_cast<std::uint32_t>(valid_nodes.size());
        valid_nodes.push_back(std::move(nodes[i])); valid_kernels.push_back(std::move(kernels[i])); valid_outputs.push_back(std::move(outputs[i])); valid_effects.push_back(std::move(effects[i]));
    }
    auto graph = checked(build_graph(valid_nodes, graph_limits, account, fnv1a64));
    std::vector<std::vector<std::uint32_t>> parents(valid_nodes.size()); std::vector<HistoryRequirement> histories;
    for (std::uint32_t i = 0; i < valid_nodes.size(); ++i) { for (auto edge : graph.inputs(i)) { parents[i].push_back(edge.producer); } histories.push_back(valid_nodes[i].facts.history); }
    auto history = checked(plan_count_history(parents, histories, graph.logical_order(), account));
    std::vector<PreparedObservation> observations;
    for (const auto& request : config.observations) {
        const auto original = std::find_if(logical.logical_order().begin(), logical.logical_order().end(), [&](auto i) { return logical.id(i) == request.producer; });
        if (original == logical.logical_order().end()) { fail(FailureCode::InvalidConfig, "observation producer is undeclared"); }
        const auto old = *original;
        if (failures[old]) { result.rejected.push_back({request.id, *failures[old], {request.producer}}); continue; }
        const auto i = remap[old]; const auto& declared_outputs = valid_nodes[i].definition->contract().outputs;
        const auto output = std::find_if(declared_outputs.begin(), declared_outputs.end(), [&](const auto& port) { return port.name == request.output_name; });
        if (output == declared_outputs.end()) { fail(FailureCode::InvalidConfig, "observation output is undeclared"); }
        auto ready = validate_request_history(request, metadata.coverage, history.preceding[i]);
        if (const auto* error = std::get_if<Diagnostic>(&ready)) { result.rejected.push_back({request.id, *error, {request.producer}}); continue; }
        if (request.missing_intervals != declaration.missing_intervals) { fail(FailureCode::UnsupportedContract, "observation missing policy differs from source"); }
        observations.push_back({request, i, static_cast<PortIndex>(output - declared_outputs.begin())});
    }
    if (observations.empty()) { return result; }
    std::vector<bool> needed(valid_nodes.size(), false);
    needed[0] = true; // The declared driver source also supplies host market updates and scope closure.
    for (const auto& observation : observations) { needed[observation.producer] = true; }
    for (std::size_t i = 0; i < valid_effects.size(); ++i) { needed[i] = needed[i] || valid_effects[i].has_value(); }
    for (std::size_t pass = 0; pass < valid_nodes.size(); ++pass) {
        bool changed = false;
        for (std::uint32_t i = 0; i < valid_nodes.size(); ++i) {
            if (!needed[i]) { continue; }
            const auto representative = graph.representative(i);
            if (!needed[representative]) { needed[representative] = true; changed = true; }
            for (const auto edge : graph.inputs(i)) { if (!needed[edge.producer]) { needed[edge.producer] = true; changed = true; } }
        }
        if (!changed) { break; }
    }
    std::vector<std::uint64_t> retention(valid_nodes.size()); std::vector<Sha256> identities(valid_nodes.size());
    for (const auto& observation : observations) { auto& retained = retention[graph.representative(observation.producer)]; retained = std::max(retained, observation.request.prior_outputs); }
    for (std::uint32_t i = 0; i < valid_nodes.size(); ++i) {
        for (std::size_t p = 0; p < graph.inputs(i).size(); ++p) { auto& retained = retention[graph.representative(graph.inputs(i)[p].producer)]; retained = std::max(retained, valid_nodes[i].spec.inputs[p].prior_outputs); }
        auto scratch = checked(account->reserve(ResourceKind::Scratch, checked(checked_add(4096, checked(checked_multiply(graph.identity(i).size(), 3))))));
        identities[i] = checked(options.digest(graph.identity(i)));
    }
    ResourceBounds required{0, checked(checked_add(options.source_services.limits.cursor_bytes, source_bounds.resources.state)), 0,
        checked(checked_add(8192, options.metadata_bytes)), source_bounds.resources.retention, options.metadata_bytes, 0};
    add(required, checked(ResourceAccount::metadata_bounds(options.runtime_limits)));
    required.evidence = checked(checked_add(required.evidence, checked(checked_add(checked(checked_add(options.metadata_bytes, 4096)),
        checked(checked_multiply(options.runtime_limits.max_holders, sizeof(RetentionEvent)))))));
    std::uint64_t control_count = valid_nodes.size() + observations.size();
    for (const auto i : graph.physical_order()) {
        if (!needed[i]) { continue; }
        if (valid_kernels[i]) { add(required, valid_kernels[i]->resources()); }
        const auto capacity = checked(checked_add(retention[i], 1)); control_count = checked(checked_add(control_count, capacity));
        if (const auto* count_history = std::get_if<CountHistory>(&valid_nodes[i].facts.history)) { control_count = checked(checked_add(control_count, count_history->preceding)); }
        const auto preceding = std::get<CountHistory>(valid_nodes[i].facts.history).preceding;
        std::uint64_t parents = 0;
        for (const auto& input : valid_nodes[i].spec.inputs) { parents = checked(checked_add(parents, checked(checked_add(std::max(preceding, input.prior_outputs), 1)))); }
        const auto certificate_bytes = checked(parent_certificate_storage_bytes(scope.clock.size()));
        const auto parent_certificates = checked(checked_multiply(parents, certificate_bytes));
        auto frame_bytes = checked(PendingOutcome::storage_bytes(valid_outputs[i].size(), scope.clock.size()));
        for (const auto& output : valid_outputs[i]) {
            frame_bytes = checked(checked_add(frame_bytes, checked(type_storage_bytes(output.semantic))));
            frame_bytes = checked(checked_add(frame_bytes, checked(checked_add(ownership_control_reserve, certificate_bytes))));
        }
        frame_bytes = checked(checked_add(frame_bytes, checked(ParentSelections::storage_bytes(parents))));
        frame_bytes = checked(checked_add(frame_bytes, parent_certificates));
        // Retained publications, a current nonpublication and the next private
        // bundle can coexist. State-held history certificates have their own bound.
        required.staging = checked(checked_add(required.staging,
            checked(checked_add(checked(checked_multiply(checked(checked_add(capacity, 2)), frame_bytes)), parent_certificates))));
        required.graph = checked(checked_add(required.graph, checked(checked_add(4096, checked(checked_multiply(capacity, 128))))));
        required.retention = checked(checked_add(required.retention, checked(checked_multiply(retention[i], valid_kernels[i] ? valid_kernels[i]->resources().retention : source_bounds.resources.retention))));
    }
    if (checked(checked_multiply(control_count, 8)) > options.runtime_limits.max_allocations) { fail(FailureCode::ResourcePlanExceeded, "allocation ledger cannot cover declared state/history"); }
    const auto need = fields(required), available = fields(options.runtime_limits.bytes);
    for (std::size_t i = 0; i < need.size(); ++i) { if (need[i] > available[i]) { fail(FailureCode::ResourcePlanExceeded, "combined admitted graph exceeds runtime resource bounds"); } }
    if (checked(checked_add(checked(required.total()), source_bounds.resources.source)) > limits.managed_buffer_bytes) { fail(FailureCode::ResourcePlanExceeded, "combined source/runtime plan exceeds managed budget"); }
    std::uint64_t live_preparation = 0;
    for (auto kind : {ResourceKind::Source, ResourceKind::State, ResourceKind::Scratch, ResourceKind::Staging, ResourceKind::Retention, ResourceKind::Graph, ResourceKind::Evidence}) {
        live_preparation = checked(checked_add(live_preparation, account->current(kind)));
    }
    if (checked(checked_add(live_preparation, checked(options.runtime_limits.bytes.total()))) > limits.managed_buffer_bytes) {
        fail(FailureCode::ResourcePlanExceeded, "pinned preparation plus runtime ceilings exceed managed budget");
    }
    result.plan = finish(PreparedPlan::Data{std::move(charge), configuration(composition), std::move(valid_nodes), std::move(graph), std::move(snapshot),
        std::move(valid_kernels), std::move(valid_outputs), std::move(observations), result.rejected, std::move(needed), std::move(retention),
        std::move(history.preceding), std::move(identities), std::move(valid_effects), options.runtime_limits, required, options.metadata_bytes}, std::move(store), account, options);
    return result;
}
const CompositionDeclaration& PreparedPlan::configuration() const noexcept { return *data_->configuration; }
const FrozenGraph& PreparedPlan::graph() const noexcept { return data_->graph; }
std::span<const GraphNode> PreparedPlan::nodes() const noexcept { return data_->nodes; }
const SourceSnapshot& PreparedPlan::source() const noexcept { return data_->source; }
const PreparedKernel* PreparedPlan::kernel(std::uint32_t i) const noexcept { return data_->kernels[i] ? &*data_->kernels[i] : nullptr; }
std::span<const ValueContract> PreparedPlan::outputs(std::uint32_t i) const { return data_->outputs.at(i); }
std::span<const PreparedObservation> PreparedPlan::observations() const noexcept { return data_->observations; }
std::span<const RequestRejection> PreparedPlan::rejections() const noexcept { return data_->rejections; }
bool PreparedPlan::needed(std::uint32_t i) const { return data_->needed.at(i); }
std::uint64_t PreparedPlan::retention(std::uint32_t i) const { return data_->retention.at(data_->graph.representative(i)); }
std::uint64_t PreparedPlan::preceding(std::uint32_t i) const { return data_->preceding.at(i); }
const Sha256& PreparedPlan::identity(std::uint32_t i) const { return data_->identities.at(i); }
const AccountLimits& PreparedPlan::runtime_limits() const noexcept { return data_->runtime; }
const ResourceBounds& PreparedPlan::required_resources() const noexcept { return data_->required; }
const RetentionLogConfiguration& PreparedPlan::evidence_specification() const noexcept { return data_->configuration->run.retention_log; }
std::uint64_t PreparedPlan::metadata_bytes() const noexcept { return data_->metadata_bytes; }
const std::optional<SequenceScope>& PreparedPlan::effect_scope(std::uint32_t i) const { return data_->effects.at(i); }
bool PreparedPlan::admitted() const noexcept { return data_->admitted; }
std::shared_ptr<ResultStore> PreparedPlan::store() const noexcept { return data_->store; }
std::span<const RequiredEvidenceSpec> PreparedPlan::required_evidence() const noexcept{return data_->evidence;}
std::shared_ptr<ResourceAccount> PreparedPlan::evidence_account() const noexcept{return data_->evidence_account;}
std::uint64_t PreparedPlan::report_bytes() const noexcept{return data_->report_bytes;}
const ResourceBounds& PreparedPlan::evidence_resource_bounds() const noexcept{return data_->evidence_resources;}
std::string_view PreparedPlan::reuse_decision(std::uint32_t i) const {if(!data_->needed.at(i)){return "unused";}if(!data_->store){return "disabled";}return data_->reuse_decisions.at(data_->graph.representative(i));}
const ArtifactIdentity* PreparedPlan::artifact_identity(std::uint32_t i) const {
    if (!data_->store) { return nullptr; } const auto& value = data_->artifact_identities.at(data_->graph.representative(i)); return value ? &*value : nullptr;
}
const ValidatedArtifact* PreparedPlan::artifact(std::uint32_t i) const {
    if (!data_->store) { return nullptr; } const auto& value = data_->artifacts.at(data_->graph.representative(i)); return value ? &*value : nullptr;
}
std::uint64_t PreparedPlan::certificate_history(std::uint32_t i) const { return data_->certificate_history.at(data_->graph.representative(i)); }
Result<void> PreparedPlan::admit_instance() const noexcept {
    return guarded([&]() -> Result<void> {
        try {
            if (!data_->admitted) { return diagnostic(FailureCode::UpdateProtocol, "prepared facts have not completed artifact/resource admission"); }
            auto bytes = checked(data_->runtime.bytes.total());
            const std::array<std::shared_ptr<ResourceAccount>, 4> accounts{data_->preparation_account, data_->configuration->account, data_->store ? data_->store->account() : nullptr, data_->evidence_account};
            for (std::size_t i = 0; i < accounts.size(); ++i) {
                if (!accounts[i] || std::find(accounts.begin(), accounts.begin() + static_cast<std::ptrdiff_t>(i), accounts[i]) != accounts.begin() + static_cast<std::ptrdiff_t>(i)) { continue; }
                for (auto kind : {ResourceKind::Source, ResourceKind::State, ResourceKind::Scratch, ResourceKind::Staging, ResourceKind::Retention, ResourceKind::Graph, ResourceKind::Evidence}) {
                    bytes = checked(checked_add(bytes, accounts[i]->current(kind)));
                }
            }
            bytes = checked(checked_add(bytes, checked(data_->future_store.total())));
            bytes = checked(checked_add(bytes, checked(data_->evidence_resources.total())));
            if (bytes > data_->configuration->limits.managed_buffer_bytes) { return diagnostic(FailureCode::ResourcePlanExceeded, "pinned preparation/store plus run and staging bounds exceed managed budget"); }
            return ok();
        } catch (const PreparationFailure& failure) { return failure.error; }
    });
}
} // namespace lexongine
