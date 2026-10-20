#include "plan_projection.hpp"
#include "native_files.hpp"
namespace lexongine {
namespace {
using namespace wire;
template <class Range, class F> Element *list(Document &d, const Range &values, F project) {
    auto *out = d.list();
    for (const auto &v : values) {
        d.item(out, project(v));
    }
    return out;
}
Element *parameters(Document &d, const CanonicalParams &p) {
    auto *values = d.list();
    for (const auto &v : p.values) {
        auto *scalar = std::visit(
            [&](const auto &x) -> Element * {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<T, std::string>) {
                    return d.variant("text", d.text(x));
                } else if constexpr (std::is_same_v<T, bool>) {
                    return d.variant("boolean", d.boolean(x));
                } else if constexpr (std::is_same_v<T, std::int64_t>) {
                    return d.variant("signed64", d.i64(x));
                } else {
                    return d.variant("unsigned64", d.u64(x));
                }
            },
            v.value);
        const auto origin = v.origin == Origin::ExplicitConfig  ? "explicit_config"
                            : v.origin == Origin::VersionedFact ? "versioned_fact"
                                                                : "derived";
        d.item(values, d.record("NamedParam", {{"name", d.text(v.name)},
                                               {"value", scalar},
                                               {"origin", d.text(origin)},
                                               {"provenance", d.text(v.provenance)}}));
    }
    return d.record("EffectiveParametersV1",
                    {{"schema_version", d.u64(p.schema_version)}, {"values", values}});
}
Element *operation(Document &d, const OperationKey &k) {
    return d.record("OperationKey",
                    {{"namespace", d.text(k.name_space)},
                     {"name", d.text(k.name)},
                     {"semantic_version", d.u64(k.semantic_version)},
                     {"implementation_selector", d.text(k.implementation_selector)}});
}
Element *implementation(Document &d, const ImplementationIdentity &b) {
    return d.record("ImplementationIdentity",
                    {{"binary_digest", d.digest(b.binary_digest)},
                     {"dependencies_digest", d.digest(b.dependencies_digest)},
                     {"source_build_digest", d.digest(b.source_build_digest)},
                     {"sdk_build_contract_id", d.text(b.sdk_build_contract_id)}});
}
Element *source(Document &d, const SourceIdentity &s) {
    auto *op = d.record(
        "OperationIdentityV1",
        {{"namespace", d.text(s.provider_operation.name_space)},
         {"name", d.text(s.provider_operation.name)},
         {"semantic_version", d.u64(s.provider_operation.semantic_version)},
         {"implementation_selector", d.text(s.provider_operation.implementation_selector)},
         {"binary_digest", d.digest(s.provider_implementation.binary_digest)},
         {"dependencies_digest", d.digest(s.provider_implementation.dependencies_digest)},
         {"source_build_digest", d.digest(s.provider_implementation.source_build_digest)},
         {"sdk_build_contract_id", d.text(s.provider_implementation.sdk_build_contract_id)}});
    return d.record("SourceIdentityV1",
                    {{"snapshot_name", d.text(s.snapshot_name)},
                     {"source_version", d.text(s.source_version)},
                     {"content_digest", d.digest(s.content_digest)},
                     {"provider_operation", op},
                     {"interpretation", project(d, s.interpretation)},
                     {"query_selection_contract", project(d, s.query_selection)},
                     {"reference_digest", d.digest(s.reference_digest)},
                     {"validated_scope", project(d, s.validated_scope)},
                     {"coverage", project(d, s.coverage)},
                     {"order_contract", project(d, s.order)},
                     {"availability_contract", project(d, s.availability)},
                     {"validation_contract", project(d, s.validation)}});
}
Element *retention(Document &d, const RetentionLogConfiguration &r) {
    return d.record("RetentionLogConfigurationV1", {{"path", d.text(r.path)},
                                                    {"format", d.text(r.format)},
                                                    {"open", d.text(r.open)},
                                                    {"flush", d.text(r.flush)},
                                                    {"failure", d.text(r.failure)},
                                                    {"overflow", d.text(r.overflow)},
                                                    {"max_bytes", d.u64(r.max_bytes)},
                                                    {"max_events", d.u64(r.max_events)}});
}
Element *run(Document &d, const RunConfiguration &r) {
    return d.record("RunConfigurationV1",
                    {{"id", d.text(r.id)},
                     {"clock", d.text(r.clock)},
                     {"driver", d.text(r.driver == Driver::Standalone ? "standalone" : "hosted")},
                     {"execution", d.text(r.execution)},
                     {"sharing", d.text(r.sharing)},
                     {"failure", d.text(r.failure)},
                     {"trace", d.text(r.trace)},
                     {"retention_warning_steps", d.u64(r.retention_warning_steps)},
                     {"retention_log", retention(d, r.retention_log)}});
}
Element *limits(Document &d, const CompositionLimits &l) {
    return d.record("CompositionLimitsV1", {{"xml_bytes", d.u64(l.xml_bytes)},
                                            {"xml_depth", d.u64(l.xml_depth)},
                                            {"xml_elements", d.u64(l.xml_elements)},
                                            {"string_bytes", d.u64(l.string_bytes)},
                                            {"nodes", d.u64(l.nodes)},
                                            {"edges", d.u64(l.edges)},
                                            {"requests", d.u64(l.requests)},
                                            {"source_updates", d.u64(l.source_updates)},
                                            {"managed_buffer_bytes", d.u64(l.managed_buffer_bytes)},
                                            {"trace_events", d.u64(l.trace_events)},
                                            {"trace_message_bytes", d.u64(l.trace_message_bytes)}});
}
Element *cache(Document &d, const CacheConfiguration &c) {
    if (std::holds_alternative<DisabledCache>(c)) {
        return d.record("DisabledCacheV1", {});
    }
    const auto &v = std::get<LocalCacheConfiguration>(c);
    return d.record("LocalCacheConfigurationV1",
                    {{"mode", d.text(v.mode)},
                     {"backend", d.text(v.backend)},
                     {"directory", d.text(v.directory)},
                     {"matching", d.text(v.matching)},
                     {"trust", d.text(v.trust)},
                     {"integrity", d.text(v.integrity)},
                     {"writer", d.text(v.writer)},
                     {"corruption", d.text(v.corruption)},
                     {"capacity", d.text(v.capacity)},
                     {"cleanup", d.text(v.cleanup)},
                     {"max_entries", d.u64(v.max_entries)},
                     {"max_store_bytes", d.u64(v.max_store_bytes)},
                     {"max_entry_bytes", d.u64(v.max_entry_bytes)},
                     {"max_decoded_bytes", d.u64(v.max_decoded_bytes)}});
}
Element *input(Document &d, const InputBindingSpec &i) {
    return d.record(
        "InputBindingSpec",
        {{"input_name", d.text(i.input_name)},
         {"producer", d.text(i.producer)},
         {"output_name", d.text(i.output_name)},
         {"prior_outputs", d.u64(i.prior_outputs)},
         {"alignment", d.text("same_step")},
         {"missing_intervals",
          d.text(i.missing_intervals == MissingIntervalPolicy::Strict ? "strict" : "skip")}});
}
Element *node(Document &d, const NodeSpec &n) {
    return d.record("NodeSpec",
                    {{"id", d.text(n.id)},
                     {"label", d.text(n.label)},
                     {"ordinal", d.u64(n.ordinal)},
                     {"operation", operation(d, n.operation)},
                     {"parameters", parameters(d, n.parameters)},
                     {"inputs", list(d, n.inputs, [&](const auto &i) { return input(d, i); })},
                     {"initialization", project(d, n.initialization)},
                     {"update_policy", project(d, n.update_policy)},
                     {"availability_policy", project(d, n.availability_policy)},
                     {"effect_policy", project(d, n.effect_policy)}});
}
Element *output(Document &d, const OutputContract &o) {
    return d.record("OutputContract",
                    {{"name", d.text(o.name)},
                     {"type", project(d, o.type)},
                     {"storage", d.text(o.storage == StorageKind::ScalarCopy ? "scalar_copy"
                                                                             : "immutable_owned")},
                     {"codec", project(d, o.codec)},
                     {"validation", project(d, o.validation)},
                     {"cacheable", d.boolean(o.cacheable)}});
}
std::string_view failure_name(FailureCode code) {
    static constexpr std::string_view names[]{"InvalidConfig",       "UnknownDefinition",
                                              "DuplicateDefinition", "UnsupportedContract",
                                              "GraphCycle",          "TypeMismatch",
                                              "SourceInvalid",       "SourceCoverage",
                                              "SourceChanged",       "SourceKnownBoundViolation",
                                              "HistoryInsufficient", "ResourcePlanExceeded",
                                              "ResourceExceeded",    "UpdateProtocol",
                                              "FutureRead",          "MissingRequired",
                                              "NumericBound",        "NumericOverflow",
                                              "CallbackException",   "OutputInvalid",
                                              "EvidenceQuota",       "EvidenceIO",
                                              "CacheCorrupt",        "CacheIO",
                                              "CacheBusy",           "CommitUnconfirmed",
                                              "AlreadyExecuted"};
    const auto index = static_cast<std::size_t>(code);
    if (index >= std::size(names)) {
        fail("unknown diagnostic code");
    }
    return names[index];
}
} // namespace
std::string_view name(NumericalOutcome v) {
    switch (v) {
    case NumericalOutcome::Completed:
        return "completed";
    case NumericalOutcome::Failed:
        return "failed";
    case NumericalOutcome::Aborted:
        return "aborted";
    }
    wire::fail("unknown numerical outcome");
}
std::string_view name(IntegrationOutcome v) {
    switch (v) {
    case IntegrationOutcome::Completed:
        return "completed";
    case IntegrationOutcome::Failed:
        return "failed";
    case IntegrationOutcome::Aborted:
        return "aborted";
    case IntegrationOutcome::NotFinished:
        return "not_finished";
    }
    wire::fail("unknown integration outcome");
}
std::string_view name(AcceptanceStatus v) {
    switch (v) {
    case AcceptanceStatus::Pending:
        return "pending";
    case AcceptanceStatus::Accepted:
        return "accepted";
    case AcceptanceStatus::Rejected:
        return "rejected";
    case AcceptanceStatus::Unconfirmed:
        return "unconfirmed";
    }
    wire::fail("unknown acceptance");
}
std::string_view name(CachePublicationStatus v) {
    switch (v) {
    case CachePublicationStatus::Disabled:
        return "disabled";
    case CachePublicationStatus::NoNewEntries:
        return "no_new_entries";
    case CachePublicationStatus::Bypassed:
        return "bypassed";
    case CachePublicationStatus::Staged:
        return "staged";
    case CachePublicationStatus::Published:
        return "published";
    case CachePublicationStatus::NotPublished:
        return "not_published";
    case CachePublicationStatus::Unconfirmed:
        return "unconfirmed";
    }
    wire::fail("unknown cache status");
}
std::string_view name(OutcomeKind v) {
    switch (v) {
    case OutcomeKind::Published:
        return "published";
    case OutcomeKind::Warming:
        return "warming";
    case OutcomeKind::ValidAbsence:
        return "valid_absence";
    case OutcomeKind::MissingRequired:
        return "missing_required";
    case OutcomeKind::Failure:
        return "failure";
    }
    wire::fail("unknown outcome");
}
wire::Element *project_resources(wire::Document &d, const ResourceBounds &r) {
    return d.record("ResourceBounds", {{"source", d.u64(r.source)},
                                       {"state", d.u64(r.state)},
                                       {"scratch", d.u64(r.scratch)},
                                       {"staging", d.u64(r.staging)},
                                       {"retention", d.u64(r.retention)},
                                       {"graph", d.u64(r.graph)},
                                       {"evidence", d.u64(r.evidence)}});
}
wire::Element *project_observation(wire::Document &d, const ObservationRequest &r) {
    return d.record(
        "ObservationRequest",
        {{"missing_intervals",
          d.text(r.missing_intervals == MissingIntervalPolicy::Strict ? "strict" : "skip")},
         {"id", d.text(r.id)},
         {"producer", d.text(r.producer)},
         {"output_name", d.text(r.output_name)},
         {"evaluation", wire::project(d, r.evaluation)},
         {"absence", d.text("require_value")},
         {"prior_outputs", d.u64(r.prior_outputs)},
         {"label", d.text(r.label)}});
}
wire::Element *project_diagnostic(wire::Document &d, const Diagnostic &e) {
    return d.record(
        "Diagnostic",
        {{"code", d.text(failure_name(e.code))},
         {"message", d.text(e.text())},
         {"length", d.u64(e.length)},
         {"truncated", d.boolean(e.truncated)},
         {"logical_ordinal", d.optional(e.logical_ordinal ? d.u64(*e.logical_ordinal) : nullptr)},
         {"request_ordinal", d.optional(e.request_ordinal ? d.u64(*e.request_ordinal) : nullptr)},
         {"key", d.optional(e.key ? d.record("DiagnosticKey",
                                             {{"clock_ordinal", d.u64(e.key->clock_ordinal)},
                                              {"sequence", d.u64(e.key->sequence)}})
                                  : nullptr)}});
}
wire::Element *project_parent(wire::Document &d, const ParentReference &p) {
    const auto &v = p.publication;
    auto *publication = d.record(
        "ParentPublication",
        {{"computation_digest", d.digest(v.computation_digest)},
         {"key", wire::project(d, v.key)},
         {"port", d.u64(v.port)},
         {"extent", wire::project(d, v.extent)},
         {"available_at_ns", d.i64(v.available_at_ns)},
         {"source_reference_digest",
          d.optional(v.source_reference_digest ? d.digest(*v.source_reference_digest) : nullptr)}});
    return d.record("ParentReference",
                    {{"input_port", d.u64(p.input_port)}, {"publication", publication}});
}
wire::Element *project_plan_identity(wire::Document &d, const PreparedPlan &p) {
    using namespace wire;
    const auto &c = p.configuration();
    auto *nodes = d.list();
    auto *effects = d.list();
    auto *aliases = d.list();
    for (std::uint32_t i = 0; i < p.nodes().size(); ++i) {
        const auto &n = p.nodes()[i];
        d.item(nodes, d.record("PreparedNodeV1",
                               {{"spec", node(d, n.spec)},
                                {"canonical_identity", d.text(hex(p.graph().identity(i)))},
                                {"needed", d.boolean(p.needed(i))},
                                {"retention", d.u64(p.retention(i))},
                                {"preceding", d.u64(p.preceding(i))},
                                {"outputs", list(d, n.definition->contract().outputs,
                                                 [&](const auto &o) { return output(d, o); })}}));
        d.item(aliases, d.record("AliasV1", {{"logical", d.u64(i)},
                                             {"representative", d.u64(p.graph().representative(i))},
                                             {"physical", d.u64(p.graph().physical_index(i))}}));
        if (p.effect_scope(i)) {
            d.item(effects,
                   d.record("EffectScopeV1", {{"node", d.text(n.spec.id)},
                                              {"evaluation", project(d, *p.effect_scope(i))}}));
        }
    }
    auto *effective = d.record(
        "EffectiveCompositionV1",
        {{"run", run(d, c.run)},
         {"limits", limits(d, c.limits)},
         {"cache", cache(d, c.cache)},
         {"plugins", list(d, c.plugins,
                          [&](const auto &v) {
                              return d.record("PluginDeclarationV1",
                                              {{"path", d.text(v.path)},
                                               {"build_contract_id", d.text(v.build_contract_id)}});
                          })},
         {"sources",
          list(d, p.nodes()[0].facts.sources, [&](const auto &s) { return source(d, s); })},
         {"nodes", nodes},
         {"effects", effects},
         {"observations",
          list(d, c.observations, [&](const auto &o) { return project_observation(d, o); })},
         {"overrides", list(d, c.overrides, [&](const auto &o) {
              return d.record("ConfigurationOverrideV1",
                              {{"target", d.text(o.target)}, {"value", d.text(o.value)}});
          })}});
    auto *admitted =
        list(d, p.observations(), [&](const auto &v) { return project_observation(d, v.request); });
    auto *rejected = list(d, p.rejections(), [&](const auto &v) {
        return d.record("RequestRejectionV1",
                        {{"request", d.text(v.request)},
                         {"cause", project_diagnostic(d, v.cause)},
                         {"dependencies",
                          list(d, v.dependencies, [&](const auto &id) { return d.text(id); })}});
    });
    const auto &r = p.runtime_limits();
    auto *runtime = d.record("AccountLimitsV1",
                             {{"bytes", project_resources(d, r.bytes)},
                              {"max_allocations", d.u64(r.max_allocations)},
                              {"max_holders", d.u64(r.max_holders)},
                              {"max_events", d.u64(r.max_events)},
                              {"retention_warning_steps", d.u64(r.retention_warning_steps)}});
    auto *resources =
        d.record("ResourcePlanV1",
                 {{"required", project_resources(d, p.required_resources())},
                  {"runtime_limits", runtime},
                  {"metadata_bytes", d.u64(p.metadata_bytes())},
                  {"evidence_resources", project_resources(d, p.evidence_resource_bounds())}});
    auto *evidence = d.list();
    const auto &log = p.evidence_specification();
    d.item(evidence, project_spec(d, {"retention", log.format, log.path, log.open, log.max_bytes,
                                      log.max_events, log.flush, log.failure}));
    for (const auto &s : p.required_evidence()) {
        d.item(evidence, project_spec(d, s));
    }
    return d.record("PlanIdentityV1", {{"effective_composition", effective},
                                       {"logical_order", list(d, p.graph().logical_order(),
                                                              [&](auto i) { return d.u64(i); })},
                                       {"aliases", aliases},
                                       {"admitted_requests", admitted},
                                       {"rejected_requests", rejected},
                                       {"resource_plan", resources},
                                       {"required_evidence", evidence}});
}
Result<Sha256> plan_digest(const PreparedPlan &p, std::shared_ptr<ResourceAccount> account,
                           const MetadataLimits &limits) noexcept {
    return wire::protect([&]() -> Result<Sha256> {
        auto d = wire::checked(wire::Document::create(account, limits.xml));
        auto *value = project_plan_identity(d, p);
        auto bytes = wire::canonical(*value, 3, limits.xml.bytes);
        return files::digest(bytes);
    });
}
Result<Sha256> acceptance_digest(std::span<const RequiredEvidenceSpec> specs,
                                 std::shared_ptr<ResourceAccount> account,
                                 const MetadataLimits &limits) noexcept {
    return wire::protect([&]() -> Result<Sha256> {
        auto d = wire::checked(wire::Document::create(account, limits.xml));
        auto *values = d.list();
        for (const auto &s : specs) {
            d.item(values, project_spec(d, s));
        }
        auto bytes = wire::canonical(*values, 0, limits.xml.bytes);
        constexpr std::string_view domain = "lexongine.acceptance.v1";
        bytes.insert(bytes.begin(), reinterpret_cast<const std::byte *>(domain.data()),
                     reinterpret_cast<const std::byte *>(domain.data() + domain.size()));
        return files::digest(bytes);
    });
}
Result<Sha256> implementation_digest(const ImplementationIdentity &identity,
                                     std::shared_ptr<ResourceAccount> account,
                                     const MetadataLimits &limits) noexcept {
    return wire::protect([&]() -> Result<Sha256> {
        auto d = wire::checked(wire::Document::create(account, limits.xml));
        auto bytes = wire::canonical(*implementation(d, identity), 0, limits.xml.bytes);
        constexpr std::string_view domain = "lexongine.build.v1";
        bytes.insert(bytes.begin(), reinterpret_cast<const std::byte *>(domain.data()),
                     reinterpret_cast<const std::byte *>(domain.data() + domain.size()));
        return files::digest(bytes);
    });
}
} // namespace lexongine
