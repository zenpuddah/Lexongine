#include "xml_report.hpp"
#include "native_files.hpp"
#include <sstream>

namespace lexongine {
namespace {
using namespace wire;
template <class R, class F> Element *list(Document &d, const R &values, F f) {
    auto *result = d.list();
    for (const auto &value : values) {
        d.item(result, f(value));
    }
    return result;
}
void section(Document &d, Element *root, std::string_view name, Element *value) {
    auto *e = d.element(name);
    e->InsertEndChild(value);
    root->InsertEndChild(e);
}
Element *operation_key(Document &d, const OperationKey &k) {
    return d.record("OperationKey",
                    {{"namespace", d.text(k.name_space)},
                     {"name", d.text(k.name)},
                     {"semantic_version", d.u64(k.semantic_version)},
                     {"implementation_selector", d.text(k.implementation_selector)}});
}
std::string_view missing(MissingIntervalPolicy v) {
    return v == MissingIntervalPolicy::Strict ? "strict" : "skip";
}
std::string_view sharing(SharingClass v) {
    return v == SharingClass::Unique             ? "unique"
           : v == SharingClass::StatefulDeclared ? "stateful_declared"
                                                 : "stateless_deterministic";
}
std::string_view cache_decision(const PreparedPlan &p, std::uint32_t i) {
    return p.reuse_decision(i);
}
Element *statistics(Document &d, const ProducerStatistics &p) {
    return d.record("ProducerStatisticsV1", {{"id", d.text(p.id)},
                                             {"physical", d.u64(p.physical)},
                                             {"advances", d.u64(p.advances)},
                                             {"publications", d.u64(p.publications)},
                                             {"aliases", d.u64(p.aliases)},
                                             {"waiting", d.u64(p.waiting)},
                                             {"cached_deliveries", d.u64(p.cached_deliveries)}});
}
} // namespace
wire::Element *project_tool_profile(wire::Document &d) {
    return d.record("ToolProfileV1",
                    {{"id", d.text("lexongine.cli.v1")},
                     {"authoring_xml_bytes", d.u64(1048576)},
                     {"authoring_depth", d.u64(32)},
                     {"authoring_elements", d.u64(16384)},
                     {"authoring_string_bytes", d.u64(1024)},
                     {"metadata_xml_bytes", d.u64(cli_metadata_limits.xml.bytes)},
                     {"metadata_depth", d.u64(cli_metadata_limits.xml.depth)},
                     {"metadata_elements", d.u64(cli_metadata_limits.xml.elements)},
                     {"metadata_string_bytes", d.u64(cli_metadata_limits.xml.string_bytes)},
                     {"metadata_workspace_bytes", d.u64(cli_metadata_limits.xml.workspace_bytes)},
                     {"decoded_metadata_bytes", d.u64(cli_metadata_limits.owned_bytes)},
                     {"snapshot_capture_bytes", d.u64(cli_capture_bytes)},
                     {"max_parents", d.u64(cli_max_parents)}});
}
wire::Element *project_operation(wire::Document &d, const OperationContract &o) {
    const auto &b = o.implementation;
    auto *build = d.record("ImplementationIdentity",
                           {{"binary_digest", d.digest(b.binary_digest)},
                            {"dependencies_digest", d.digest(b.dependencies_digest)},
                            {"source_build_digest", d.digest(b.source_build_digest)},
                            {"sdk_build_contract_id", d.text(b.sdk_build_contract_id)}});
    auto *parameters = list(d, o.parameters.fields, [&](const auto &p) {
        const auto &c = p.constraints;
        auto *constraints = d.record(
            "ParameterConstraints",
            {{"signed_min", d.optional(c.signed_min ? d.i64(*c.signed_min) : nullptr)},
             {"signed_max", d.optional(c.signed_max ? d.i64(*c.signed_max) : nullptr)},
             {"unsigned_min", d.optional(c.unsigned_min ? d.u64(*c.unsigned_min) : nullptr)},
             {"unsigned_max", d.optional(c.unsigned_max ? d.u64(*c.unsigned_max) : nullptr)},
             {"allowed_text", list(d, c.allowed_text, [&](const auto &s) { return d.text(s); })}});
        return d.record("ParameterField",
                        {{"name", d.text(p.name)},
                         {"kind", d.text(p.kind == ParamKind::Signed64     ? "signed64"
                                         : p.kind == ParamKind::Unsigned64 ? "unsigned64"
                                         : p.kind == ParamKind::Boolean    ? "boolean"
                                                                           : "text")},
                         {"required", d.boolean(p.required)},
                         {"constraints", constraints}});
    });
    auto *inputs = list(d, o.inputs, [&](const auto &i) {
        return d.record("InputContract",
                        {{"name", d.text(i.name)},
                         {"type", project(d, i.type)},
                         {"alignment", d.text("same_step")},
                         {"missing_intervals", d.text(missing(i.missing_intervals))},
                         {"absence", d.text("require_value")}});
    });
    auto *outputs = list(d, o.outputs, [&](const auto &v) {
        return d.record("OutputContract", {{"name", d.text(v.name)},
                                           {"type", project(d, v.type)},
                                           {"storage", d.text(v.storage == StorageKind::ScalarCopy
                                                                  ? "scalar_copy"
                                                                  : "immutable_owned")},
                                           {"codec", project(d, v.codec)},
                                           {"validation", project(d, v.validation)},
                                           {"cacheable", d.boolean(v.cacheable)}});
    });
    const auto &s = o.sharing;
    auto *share =
        d.record("SharingContract",
                 {{"classification", d.text(sharing(s.classification))},
                  {"initialization", project(d, s.initialization)},
                  {"update", project(d, s.update)},
                  {"effect", project(d, s.effect)},
                  {"effect_free", d.boolean(s.effect_free)},
                  {"replay_safe", d.boolean(s.replay_safe)},
                  {"cacheable", d.boolean(s.cacheable)},
                  {"correctness_evidence",
                   list(d, s.correctness_evidence, [&](const auto &v) { return d.text(v); })}});
    return d.record(
        "OperationContract",
        {{"key", operation_key(d, o.key)},
         {"implementation", build},
         {"parameters", d.record("ParameterSchema", {{"version", d.u64(o.parameters.version)},
                                                     {"fields", parameters}})},
         {"inputs", inputs},
         {"outputs", outputs},
         {"sharing", share},
         {"timing_protocols",
          list(d, o.timing_protocols, [&](const auto &v) { return d.text(v); })},
         {"alignment_protocols",
          list(d, o.alignment_protocols, [&](const auto &) { return d.text("same_step"); })},
         {"missing_protocols",
          list(d, o.missing_protocols, [&](const auto &v) { return d.text(missing(v)); })},
         {"history_protocols",
          list(d, o.history_protocols, [&](const auto &v) { return d.text(v); })},
         {"description", d.text(o.description)}});
}
Result<wire::EncodedXml> export_registry_xml(const Registry &registry, const OperationKey *selected,
                                             std::shared_ptr<ResourceAccount> account,
                                             const MetadataLimits &limits) noexcept {
    return wire::protect([&]() -> Result<wire::EncodedXml> {
        auto d = checked(Document::create(account, limits.xml));
        auto *operations = d.list();
        if (selected) {
            d.item(operations,
                   project_operation(d, checked(registry.resolve(*selected))->contract()));
        } else {
            for (const auto &definition : registry.definitions()) {
                d.item(operations, project_operation(d, definition->contract()));
            }
        }
        auto *schemas = d.list();
        for (const auto &schema : schema_descriptors) {
            const auto *shape = schema_value_shape(schema.name);
            d.item(schemas, d.record("SchemaInspectionV1",
                                     {{"name", d.text(schema.name)},
                                      {"fields", d.text(schema.fields)},
                                      {"value_shapes",
                                       d.optional(shape ? d.text(shape->fields) : nullptr)}}));
        }
        auto *value = d.record("RegistryInspectionV1", {{"operations", operations},
                                                        {"schemas", schemas},
                                                        {"tool_profile", project_tool_profile(d)}});
        validate_value(*value);
        auto *root = d.element("registry");
        d.attribute(root, "version", std::uint64_t{1});
        root->InsertEndChild(value);
        d.dom().InsertEndChild(root);
        return d.render();
    });
}
Result<wire::EncodedXml> export_plan_xml(const PreparedPlan &p,
                                         std::shared_ptr<ResourceAccount> account,
                                         const MetadataLimits &limits) noexcept {
    return wire::protect([&]() -> Result<wire::EncodedXml> {
        auto d = checked(Document::create(account, limits.xml));
        auto *identity = project_plan_identity(d, p);
        auto f = fields(*identity, "PlanIdentityV1");
        auto composition = fields(*f[0], "EffectiveCompositionV1");
        auto hash = checked(files::digest(canonical(*identity, 3, limits.xml.bytes)));
        auto *root = d.element("plan");
        d.attribute(root, "version", std::uint64_t{1});
        d.attribute(root, "run_id", p.configuration().run.id);
        d.attribute(root, "plan_digest", hex(hash));
        d.attribute(root, "schedule_policy", "logical_first_occurrence_v1");
        d.dom().InsertEndChild(root);
        section(d, root, "effective_config", d.copy(*f[0]));
        section(d, root, "sources", d.copy(*composition[4]));
        section(d, root, "logical_nodes", d.copy(*composition[5]));
        section(d, root, "physical_nodes",
                list(d, p.graph().physical_order(), [&](auto i) { return d.u64(i); }));
        section(d, root, "aliases", d.copy(*f[2]));
        section(d, root, "logical_schedule", d.copy(*f[1]));
        section(
            d, root, "requests",
            d.record("PlanRequestsV1", {{"admitted", d.copy(*f[3])}, {"rejected", d.copy(*f[4])}}));
        section(d, root, "resources",
                d.record("InspectionResourcesV1", {{"admission", d.copy(*f[5])},
                                                   {"tool_profile", project_tool_profile(d)}}));
        auto *decisions = d.list();
        for (std::uint32_t i = 0; i < p.nodes().size(); ++i) {
            const auto *artifact = p.needed(i) ? p.artifact(i) : nullptr;
            const auto *id = p.needed(i) ? p.artifact_identity(i) : nullptr;
            Element *origin = nullptr;
            if (artifact) {
                const auto &o = artifact->origin();
                origin =
                    d.record("ArtifactOriginV1", {{"producer", d.text(p.nodes()[i].spec.id)},
                                                  {"run_id", d.text(o.run_id)},
                                                  {"commit_id", d.text(o.commit_id)},
                                                  {"record_digest", d.digest(o.record_digest)}});
            }
            d.item(decisions, d.record("PlanCacheDecisionV1",
                                       {{"logical", d.u64(i)},
                                        {"eligible", d.boolean(id != nullptr)},
                                        {"decision", d.text(cache_decision(p, i))},
                                        {"artifact_identity",
                                         d.optional(id ? d.digest(id->digest()) : nullptr)},
                                        {"origin", d.optional(origin)}}));
        }
        section(d, root, "cache_decisions", decisions);
        section(d, root, "required_evidence", d.copy(*f[6]));
        return d.render();
    });
}
Result<wire::Document> decode_plan_xml(std::string_view xml,
                                       std::shared_ptr<ResourceAccount> account,
                                       const MetadataLimits &limits) noexcept {
    return wire::protect([&]() -> Result<wire::Document> {
        auto d = checked(Document::parse(xml, account, limits.xml));
        const auto &root = d.root();
        attributes(root, {"version", "run_id", "plan_digest", "schedule_policy"});
        if (std::string_view{root.Name()} != "plan" || attribute(root, "version") != "1" ||
            attribute(root, "schedule_policy") != "logical_first_occurrence_v1") {
            fail("unsupported plan envelope");
        }
        const std::string_view names[]{"effective_config", "sources",   "logical_nodes",
                                       "physical_nodes",   "aliases",   "logical_schedule",
                                       "requests",         "resources", "cache_decisions",
                                       "required_evidence"};
        auto c = children(root);
        if (c.size() != std::size(names)) {
            fail("incomplete plan envelope");
        }
        for (std::size_t i = 0; i < c.size(); ++i) {
            if (c[i]->Name() != names[i]) {
                fail("unknown or reordered plan section");
            }
            attributes(*c[i], {});
            c[i] = &only_child(*c[i]);
            validate_value(*c[i]);
        }
        auto requests = fields(*c[6], "PlanRequestsV1");
        auto resources = fields(*c[7], "InspectionResourcesV1");
        auto *identity = d.record("PlanIdentityV1", {{"effective_composition", d.copy(*c[0])},
                                                     {"logical_order", d.copy(*c[5])},
                                                     {"aliases", d.copy(*c[4])},
                                                     {"admitted_requests", d.copy(*requests[0])},
                                                     {"rejected_requests", d.copy(*requests[1])},
                                                     {"resource_plan", d.copy(*resources[0])},
                                                     {"required_evidence", d.copy(*c[9])}});
        if (checked(files::digest(canonical(*identity, 3, limits.xml.bytes))) !=
            digest_hex(attribute(root, "plan_digest"))) {
            fail("plan digest mismatch");
        }
        if (canonical(*resources[1], 0, limits.xml.bytes) !=
            canonical(*project_tool_profile(d), 0, limits.xml.bytes)) {
            fail("unsupported inspection tool profile");
        }
        auto composition = fields(*c[0], "EffectiveCompositionV1");
        auto run = fields(*composition[0], "RunConfigurationV1");
        if (text(*run[0]) != attribute(root, "run_id") ||
            canonical(*composition[4], 0, limits.xml.bytes) !=
                canonical(*c[1], 0, limits.xml.bytes) ||
            canonical(*composition[5], 0, limits.xml.bytes) !=
                canonical(*c[2], 0, limits.xml.bytes)) {
            fail("plan projections disagree");
        }
        auto items = [](const Element &e) {
            if (std::string_view{e.Name()} != "list") {
                fail("plan list required");
            }
            auto values = children(e);
            for (auto *&v : values) {
                v = &only_child(*v);
            }
            return values;
        };
        const auto nodes = items(*c[2]);
        const auto aliases = items(*c[4]);
        const auto order = items(*c[5]);
        const auto physical = items(*c[3]);
        const auto decisions = items(*c[8]);
        if (aliases.size() != nodes.size() || order.size() != nodes.size() ||
            decisions.size() != nodes.size()) {
            fail("plan node/mapping/decision counts disagree");
        }
        std::vector<bool> seen(nodes.size(), false);
        std::vector<std::uint64_t> representatives;
        representatives.reserve(nodes.size());
        for (const auto *step : order) {
            auto logical = u64(*step);
            if (logical >= nodes.size() || seen[logical]) {
                fail("plan schedule is not a complete logical permutation");
            }
            seen[logical] = true;
            auto alias = fields(*aliases[logical], "AliasV1");
            const auto rep = u64(*alias[1]);
            if (u64(*alias[0]) != logical || rep >= nodes.size()) {
                fail("plan alias refers outside logical graph");
            }
            auto pos = std::find(representatives.begin(), representatives.end(), rep);
            if (pos == representatives.end()) {
                if (rep != logical) {
                    fail("plan alias precedes its physical representative");
                }
                representatives.push_back(rep);
                pos = representatives.end() - 1;
            }
            if (u64(*alias[2]) != static_cast<std::uint64_t>(pos - representatives.begin())) {
                fail("plan physical index disagrees with first occurrence");
            }
        }
        if (physical.size() != representatives.size()) {
            fail("plan physical projection has missing or extra producers");
        }
        for (std::size_t i = 0; i < physical.size(); ++i) {
            if (u64(*physical[i]) != representatives[i]) {
                fail("plan physical projection is reordered");
            }
        }
        for (std::size_t i = 0; i < decisions.size(); ++i) {
            auto f = fields(*decisions[i], "PlanCacheDecisionV1");
            const auto status = text(*f[2]);
            const bool has_identity = attribute(*f[3], "present") == "true",
                       has_origin = attribute(*f[4], "present") == "true";
            if (status != "hit" && status != "miss_absent" && status != "miss_incompatible" &&
                status != "miss_unaccepted" && status != "disabled" && status != "ineligible" &&
                status != "unused") {
                fail("unknown cache preparation decision");
            }
            const bool eligible = status == "hit" || status.starts_with("miss_");
            if (u64(*f[0]) != i || boolean(*f[1]) != has_identity || eligible != has_identity ||
                has_origin != (status == "hit")) {
                fail("cache decision and origin/identity fields disagree");
            }
        }
        return d;
    });
}
Result<wire::EncodedXml> export_dot(const PreparedPlan &p, GraphView view,
                                    std::shared_ptr<ResourceAccount> account,
                                    std::uint64_t maximum) noexcept {
    return wire::protect([&]() -> Result<wire::EncodedXml> {
        if (view != GraphView::Logical && view != GraphView::Physical) {
            return diagnostic(FailureCode::InvalidConfig, "unknown graph inspection view");
        }
        auto charge =
            checked(account->reserve(ResourceKind::Staging, checked(checked_add(maximum, 256))));
        std::string out;
        out.reserve(static_cast<std::size_t>(maximum));
        auto append = [&](std::string_view value) {
            if (value.size() > maximum - out.size()) {
                throw Error{
                    diagnostic(FailureCode::EvidenceQuota, "DOT exceeds explicit byte limit")};
            }
            out.append(value);
        };
        auto quoted = [&](std::string_view value) {
            append("\"");
            for (char c : value) {
                if (c == '"' || c == '\\') {
                    append("\\");
                    append({&c, 1});
                } else if (c == '\n') {
                    append("\\n");
                } else if (c == '\r') {
                    append("\\r");
                } else if (c == '\t') {
                    append("\\t");
                } else {
                    append({&c, 1});
                }
            }
            append("\"");
        };
        append("digraph lexongine {\n  graph [label=");
        quoted(view == GraphView::Logical
                   ? "logical schedule before sharing"
                   : "physical producers; logical observation boundaries preserved");
        append("];\n");
        auto index = [&](std::uint32_t i) {
            return view == GraphView::Logical ? i : p.graph().representative(i);
        };
        for (std::uint32_t i = 0; i < p.nodes().size(); ++i) {
            if (index(i) != i) {
                continue;
            }
            const auto &node = p.nodes()[i];
            append("  n" + std::to_string(i) + " [label=");
            std::string label = node.spec.id + "\n" + node.spec.label + "\n" +
                                node.spec.operation.name_space + "." + node.spec.operation.name +
                                "\ncache=" + std::string{cache_decision(p, i)};
            if (view == GraphView::Physical) {
                for (std::uint32_t j = 0; j < p.nodes().size(); ++j) {
                    if (j != i && index(j) == i) {
                        label += "\nalias=" + p.nodes()[j].spec.id;
                    }
                }
            }
            quoted(label);
            append("];\n");
            for (std::size_t port = 0; port < p.graph().inputs(i).size(); ++port) {
                const auto e = p.graph().inputs(i)[port];
                append("  n" + std::to_string(index(e.producer)) + " -> n" + std::to_string(i) +
                       " [label=");
                quoted(node.spec.inputs[port].input_name + ":" +
                       node.spec.inputs[port].output_name);
                append("];\n");
            }
        }
        for (std::size_t i = 0; i < p.configuration().observations.size(); ++i) {
            const auto &request = p.configuration().observations[i];
            append("  r" + std::to_string(i) + " [shape=box,label=");
            quoted(request.id + "\n" + request.producer);
            append("];\n");
            auto producer = std::find_if(p.nodes().begin(), p.nodes().end(), [&](const auto &n) {
                return n.spec.id == request.producer;
            });
            if (producer != p.nodes().end()) {
                append("  n" +
                       std::to_string(
                           index(static_cast<std::uint32_t>(producer - p.nodes().begin()))) +
                       " -> r" + std::to_string(i) + " [style=dashed];\n");
            }
        }
        append("}\n");
        return EncodedXml{std::move(charge), std::move(out)};
    });
}
Result<wire::EncodedXml> export_report_xml(const RunReport &r, const PreparedPlan &p,
                                           std::shared_ptr<ResourceAccount> account,
                                           const MetadataLimits &limits) noexcept {
    return wire::protect([&]() -> Result<wire::EncodedXml> {
        auto d = checked(Document::create(account, limits.xml));
        auto *requests = d.list();
        for (const auto &request : p.configuration().observations) {
            auto found = std::find_if(r.observations.begin(), r.observations.end(),
                                      [&](const auto &o) { return o.id == request.id; });
            auto rejected = std::find_if(r.rejected.begin(), r.rejected.end(),
                                         [&](const auto &o) { return o.request == request.id; });
            const bool admitted = found != r.observations.end();
            const auto *cause = rejected != r.rejected.end() ? &rejected->cause
                                : r.cause                    ? &*r.cause
                                                             : nullptr;
            d.item(
                requests,
                d.record("RequestEvidenceV1",
                         {{"request", project_observation(d, request)},
                          {"admitted", d.boolean(admitted)},
                          {"delivered", d.u64(admitted ? found->delivered : 0)},
                          {"status",
                           d.text(!admitted                                    ? "rejected"
                                  : r.acceptance == AcceptanceStatus::Accepted ? "completed"
                                                                               : "incomplete")},
                          {"cause", d.optional(cause ? project_diagnostic(d, *cause) : nullptr)}}));
        }
        auto *value = d.record(
            "TerminalReceiptV1",
            {{"run_id", d.text(r.run_id)},
             {"numerical", d.text(name(r.numerical))},
             {"integration", d.text(name(r.integration))},
             {"acceptance", d.text(name(r.acceptance))},
             {"cache", d.text(name(r.cache))},
             {"commit_id", d.text(r.cache_commit_id)},
             {"record_digest", d.digest(r.cache_record_digest)},
             {"published_entries", d.u64(r.cache_published_entries)},
             {"bypassed_entries", d.u64(r.cache_bypassed_entries)},
             {"requests", requests},
             {"producers", list(d, r.producers, [&](const auto &s) { return statistics(d, s); })},
             {"complete_updates", d.u64(r.complete_updates)},
             {"gap_events", d.u64(r.gap_events)},
             {"missing_intervals", d.u64(r.missing_intervals)},
             {"current", project_resources(d, r.current)},
             {"peak", project_resources(d, r.peak)},
             {"store_current", project_resources(d, r.store_current)},
             {"store_peak", project_resources(d, r.store_peak)},
             {"cause", d.optional(r.cause ? project_diagnostic(d, *r.cause) : nullptr)},
             {"secondary_cause",
              d.optional(r.secondary_cause ? project_diagnostic(d, *r.secondary_cause) : nullptr)},
             {"evidence", list(d, r.evidence, [&](const auto &e) { return project(d, e); })}});
        validate_value(*value);
        auto *root = d.element("run_receipt");
        d.attribute(root, "version", std::uint64_t{1});
        root->InsertEndChild(value);
        d.dom().InsertEndChild(root);
        return d.render();
    });
}
int report_exit_code(const RunReport &r) noexcept {
    if (r.acceptance == AcceptanceStatus::Unconfirmed) {
        return 5;
    }
    if (r.acceptance != AcceptanceStatus::Accepted) {
        return 3;
    }
    return r.rejected.empty() ? 0 : 6;
}
} // namespace lexongine
