#include "snapshot_evidence.hpp"
#include "native_files.hpp"
#include "quant/lexongine/components.hpp"
#include <fcntl.h>
#include <unistd.h>
namespace lexongine {
namespace {
using namespace wire;
struct Value {
    std::uint32_t port;
    std::uint8_t encoding;
    std::int64_t raw;
};
struct Outcome {
    std::uint32_t logical;
    UpdateKey key;
    TemporalExtent extent;
    std::int64_t available;
    OutcomeKind kind;
    std::uint64_t remaining;
    std::uint32_t reason;
    std::vector<ParentReference> parents;
    std::vector<Value> values;
};
using Event = std::variant<Outcome, SourceGap>;
template <class R, class F> Element *list(Document &d, const R &values, F function) {
    auto *result = d.list();
    for (const auto &value : values) {
        d.item(result, function(value));
    }
    return result;
}
void child(Document &d, Element *root, std::string_view name, Element *value) {
    auto *c = d.element(name);
    c->InsertEndChild(value);
    root->InsertEndChild(c);
}
std::string_view label(const std::array<char, 64> &value) {
    auto end = std::find(value.begin(), value.end(), '\0');
    return {value.data(), static_cast<std::size_t>(end - value.begin())};
}
Element *gap(Document &d, const SourceGap &g) {
    return d.record("GapV1", {{"missing_scope", project(d, g.missing_scope)},
                              {"interval", project(d, g.interval)},
                              {"declared_reason", d.u64(g.declared_reason)}});
}
std::vector<const Element *> items(const Element &e) {
    if (std::string_view{e.Name()} != "list") {
        fail("snapshot list expected");
    }
    attributes(e, {});
    auto result = children(e);
    for (auto *&i : result) {
        if (std::string_view{i->Name()} != "item") {
            fail("snapshot item expected");
        }
        attributes(*i, {});
        i = &only_child(*i);
    }
    return result;
}
bool present(const Element &e) {
    return attribute(e, "present") == "true";
}
void one_of(std::string_view value, std::initializer_list<std::string_view> choices) {
    if (std::find(choices.begin(), choices.end(), value) == choices.end()) {
        fail("unsupported snapshot enum value");
    }
}
void validate_events(const Element &events, std::size_t nodes) {
    for (const auto *event : items(events)) {
        const auto f = fields(*event, "SnapshotEventV1");
        const auto kind = text(*f[0]);
        one_of(kind, {"gap", "outcome"});
        if (present(*f[1]) != (kind == "outcome") || present(*f[2]) != (kind == "gap")) {
            fail("snapshot event alternative disagrees with kind");
        }
        if (kind == "gap") {
            const auto g = fields(only_child(*f[2]), "GapV1");
            static_cast<void>(scope(*g[0]));
            static_cast<void>(interval(*g[1]));
            continue;
        }
        const auto o = fields(only_child(*f[1]), "SnapshotOutcomeV1");
        const auto outcome = text(*o[4]);
        one_of(outcome, {"published", "warming", "valid_absence", "missing_required", "failure"});
        if (u64(*o[0]) >= nodes) {
            fail("snapshot outcome refers to an unknown logical node");
        }
        const auto update = key(*o[1]);
        static_cast<void>(extent(*o[2]));
        const auto available = i64(*o[3]);
        if ((outcome != "warming" && u64(*o[5])) || (outcome != "valid_absence" && u64(*o[6]))) {
            fail("snapshot outcome carries another kind's fields");
        }
        for (const auto *parent : items(*o[7])) {
            const auto p = fields(*parent, "ParentReference");
            const auto publication = fields(*p[1], "ParentPublication");
            const auto parent_key = key(*publication[1]);
            static_cast<void>(extent(*publication[3]));
            if (parent_key.clock != update.clock || parent_key.sequence > update.sequence ||
                i64(*publication[4]) > available) {
                fail("snapshot parent is outside causal boundary");
            }
        }
        const auto values = items(*o[8]);
        if ((outcome == "published") == values.empty()) {
            fail("snapshot value bundle disagrees with outcome");
        }
        for (std::size_t port = 0; port < values.size(); ++port) {
            const auto v = fields(*values[port], "SnapshotValueV1");
            const auto encoding = text(*v[2]);
            one_of(encoding, {"price9", "source_reference", "metadata_only"});
            if (u64(*v[0]) != port || present(*v[3]) != (encoding == "price9") ||
                present(*v[4]) != (encoding == "source_reference")) {
                fail("snapshot value encoding/port fields disagree");
            }
            if (encoding == "price9" && type(*v[1]) != quant::lexongine::price_contract()) {
                fail("snapshot price encoding has incompatible semantic type");
            }
            if (encoding == "source_reference" && u64(*o[0]) != 0) {
                fail("non-source snapshot value claims source encoding");
            }
        }
    }
}
} // namespace
struct SnapshotEvidence::Data {
    Reservation charge;
    std::shared_ptr<const PreparedPlan> plan;
    RequiredEvidenceSpec spec;
    std::shared_ptr<ResourceAccount> account;
    SnapshotLimits limits;
    Sha256 plan_identity;
    std::vector<Event> events;
    std::uint64_t used = 0;
    int fd = -1;
    bool opened = false, closed = false;
    ~Data() {
        if (fd >= 0) {
            static_cast<void>(::close(fd));
        }
    }
    Result<void> open() noexcept {
        return guarded([&]() -> Result<void> {
            if (opened) {
                return diagnostic(FailureCode::EvidenceIO, "snapshot open is single-use");
            }
            opened = true;
            fd = ::open(spec.absolute_path.c_str(),
                        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (fd < 0) {
                return diagnostic(FailureCode::EvidenceIO,
                                  "snapshot path cannot be created exclusively");
            }
            return ok();
        });
    }
    Result<void> capture(std::uint32_t logical, const OutcomeFrame &frame) noexcept {
        return protect([&]() -> Result<void> {
            if (fd < 0 || closed) {
                return diagnostic(FailureCode::EvidenceIO, "snapshot is not open");
            }
            if (plan->configuration().run.trace == "summary") {
                return ok();
            }
            if (events.size() == spec.max_records || logical >= plan->nodes().size() ||
                frame.parents.entries().size() > limits.max_parents) {
                return diagnostic(FailureCode::EvidenceQuota,
                                  "snapshot record/parent bound exceeded");
            }
            auto bytes = checked(
                checked_add(sizeof(Outcome) + frame.context.key.clock.size() + 64,
                            checked(checked_multiply(frame.outputs.size(), sizeof(Value)))));
            for (const auto &parent : frame.parents.entries()) {
                bytes = checked(checked_add(bytes, sizeof(ParentReference) +
                                                       parent.metadata().key.clock.size() + 32));
            }
            if (bytes > limits.capture_bytes - used) {
                return diagnostic(FailureCode::EvidenceQuota,
                                  "snapshot metadata capacity exceeded");
            }
            used += bytes;
            Outcome out{
                logical,    frame.context.key,         frame.context.extent,  frame.available_at_ns,
                frame.kind, frame.remaining_preceding, frame.declared_reason, {},
                {}};
            out.parents.reserve(frame.parents.entries().size());
            out.values.reserve(frame.outputs.size());
            for (const auto &parent : frame.parents.entries()) {
                if (!parent.input_port()) {
                    return diagnostic(FailureCode::OutputInvalid,
                                      "snapshot parent lacks input attribution");
                }
                out.parents.push_back({*parent.input_port(), parent.metadata()});
            }
            if (frame.kind == OutcomeKind::Published) {
                for (std::size_t i = 0; i < frame.outputs.size(); ++i) {
                    if (!frame.outputs[i]) {
                        return diagnostic(FailureCode::OutputInvalid,
                                          "snapshot received incomplete bundle");
                    }
                    if (logical == 0) {
                        out.values.push_back({static_cast<PortIndex>(i), 1, 0});
                    } else if (frame.outputs[i]->semantic() == quant::lexongine::price_contract() &&
                               frame.outputs[i]->control()->type() == typeid(quant::data::Price)) {
                        auto value = checked(frame.outputs[i]->share<quant::data::Price>(
                            "snapshot", "record_value"));
                        out.values.push_back({static_cast<PortIndex>(i), 0, value.get().raw()});
                    } else {
                        out.values.push_back({static_cast<PortIndex>(i), 2, 0});
                    }
                }
            }
            events.emplace_back(std::move(out));
            return ok();
        });
    }
    Result<void> capture_gap(const SourceGap &value) noexcept {
        return protect([&]() -> Result<void> {
            if (fd < 0 || closed) {
                return diagnostic(FailureCode::EvidenceIO, "snapshot is not open");
            }
            if (events.size() == spec.max_records) {
                return diagnostic(FailureCode::EvidenceQuota, "snapshot event limit exceeded");
            }
            auto bytes = sizeof(SourceGap) + value.missing_scope.clock.size() + 32;
            if (bytes > limits.capture_bytes - used) {
                return diagnostic(FailureCode::EvidenceQuota, "snapshot gap metadata exhausted");
            }
            used += bytes;
            events.emplace_back(value);
            return ok();
        });
    }
    Result<EvidenceReference> finalize(const RunReport &report) noexcept {
        return protect([&]() -> Result<EvidenceReference> {
            if (fd < 0 || closed) {
                return diagnostic(FailureCode::EvidenceIO, "snapshot finalize is single-use");
            }
            closed = true;
            auto d = checked(Document::create(account, limits.metadata.xml));
            auto *root = d.element("run_snapshot");
            d.dom().InsertEndChild(root);
            d.attribute(root, "version", std::uint64_t{1});
            d.attribute(root, "run_id", report.run_id);
            d.attribute(root, "plan_digest", hex(plan_identity));
            d.attribute(root, "numerical", name(report.numerical));
            d.attribute(root, "integration_outcome", name(report.integration));
            d.attribute(root, "host_contract_id", report.host_contract_id);
            d.attribute(root, "host_contract_version", report.host_contract_version);
            d.attribute(root, "host_experiment_identity", hex(report.host_experiment_identity));
            d.attribute(root, "acceptance", name(report.acceptance));
            auto *requests = d.list();
            for (const auto &request : plan->configuration().observations) {
                auto delivered =
                    std::find_if(report.observations.begin(), report.observations.end(),
                                 [&](const auto &v) { return v.id == request.id; });
                auto rejection =
                    std::find_if(report.rejected.begin(), report.rejected.end(),
                                 [&](const auto &v) { return v.request == request.id; });
                const bool admitted = delivered != report.observations.end();
                const auto count = admitted ? delivered->delivered : 0;
                const Diagnostic *cause = rejection != report.rejected.end() ? &rejection->cause
                                          : report.cause                     ? &*report.cause
                                                                             : nullptr;
                const auto status = !admitted ? "rejected"
                                    : report.numerical == NumericalOutcome::Completed &&
                                            report.integration == IntegrationOutcome::Completed
                                        ? "completed"
                                    : count ? "partial"
                                            : "not_reached";
                d.item(requests,
                       d.record("RequestEvidenceV1",
                                {{"request", project_observation(d, request)},
                                 {"admitted", d.boolean(admitted)},
                                 {"delivered", d.u64(count)},
                                 {"status", d.text(status)},
                                 {"cause",
                                  d.optional(cause ? project_diagnostic(d, *cause) : nullptr)}}));
            }
            child(d, root, "requests", requests);
            auto *outcomes = d.list();
            for (const auto &event : events) {
                if (const auto *g = std::get_if<SourceGap>(&event)) {
                    d.item(outcomes,
                           d.record("SnapshotEventV1", {{"kind", d.text("gap")},
                                                        {"outcome", d.optional(nullptr)},
                                                        {"gap", d.optional(gap(d, *g))}}));
                    continue;
                }
                const auto &o = std::get<Outcome>(event);
                auto *values = list(d, o.values, [&](const auto &v) {
                    return d.record(
                        "SnapshotValueV1",
                        {{"port", d.u64(v.port)},
                         {"type", project(d, plan->outputs(o.logical)[v.port].semantic)},
                         {"encoding", d.text(v.encoding == 0   ? "price9"
                                             : v.encoding == 1 ? "source_reference"
                                                               : "metadata_only")},
                         {"raw", d.optional(v.encoding == 0 ? d.i64(v.raw) : nullptr)},
                         {"source_reference",
                          d.optional(
                              v.encoding == 1
                                  ? d.digest(plan->source().metadata().source_reference_digest)
                                  : nullptr)}});
                });
                auto *outcome = d.record(
                    "SnapshotOutcomeV1",
                    {{"logical", d.u64(o.logical)},
                     {"key", project(d, o.key)},
                     {"extent", project(d, o.extent)},
                     {"available_at_ns", d.i64(o.available)},
                     {"kind", d.text(name(o.kind))},
                     {"remaining_preceding", d.u64(o.remaining)},
                     {"declared_reason", d.u64(o.reason)},
                     {"parents",
                      list(d, o.parents, [&](const auto &p) { return project_parent(d, p); })},
                     {"values", values}});
                d.item(outcomes, d.record("SnapshotEventV1", {{"kind", d.text("outcome")},
                                                              {"outcome", d.optional(outcome)},
                                                              {"gap", d.optional(nullptr)}}));
            }
            child(d, root, "outcomes", outcomes);
            auto *producers = list(d, report.producers, [&](const auto &p) {
                return d.record("ProducerStatisticsV1",
                                {{"id", d.text(p.id)},
                                 {"physical", d.u64(p.physical)},
                                 {"advances", d.u64(p.advances)},
                                 {"publications", d.u64(p.publications)},
                                 {"aliases", d.u64(p.aliases)},
                                 {"waiting", d.u64(p.waiting)},
                                 {"cached_deliveries", d.u64(p.cached_deliveries)}});
            });
            auto *reach = list(d, report.last_reach, [&](const auto &r) {
                return d.record("LogicalReachV1", {{"logical", d.u64(r.logical)},
                                                   {"reached", d.boolean(r.reached)},
                                                   {"advanced", d.boolean(r.advanced)},
                                                   {"published", d.boolean(r.published)},
                                                   {"alias", d.boolean(r.alias)},
                                                   {"outcome", d.text(name(r.outcome))}});
            });
            child(d, root, "counters",
                  d.record("RunCountersV1", {{"complete_updates", d.u64(report.complete_updates)},
                                             {"gap_events", d.u64(report.gap_events)},
                                             {"missing_intervals", d.u64(report.missing_intervals)},
                                             {"producers", producers},
                                             {"last_reach", reach}}));
            auto *origins = d.list();
            for (std::uint32_t i = 0; i < plan->nodes().size(); ++i) {
                if (const auto *a = plan->artifact(i)) {
                    const auto &origin = a->origin();
                    d.item(origins, d.record("ArtifactOriginV1",
                                             {{"producer", d.text(plan->nodes()[i].spec.id)},
                                              {"run_id", d.text(origin.run_id)},
                                              {"commit_id", d.text(origin.commit_id)},
                                              {"record_digest", d.digest(origin.record_digest)}}));
                }
            }
            child(d, root, "cache",
                  d.record("RunCacheV1",
                           {{"status", d.text(name(report.cache))},
                            {"commit_id", d.text(report.cache_commit_id)},
                            {"record_digest", d.digest(report.cache_record_digest)},
                            {"staged_entries", d.u64(report.cache_staged_entries)},
                            {"published_entries", d.u64(report.cache_published_entries)},
                            {"bypassed_entries", d.u64(report.cache_bypassed_entries)},
                            {"entries", list(d, report.cache_entries,
                                             [&](const auto &e) { return project_entry(d, e); })},
                            {"origins", origins}}));
            child(d, root, "resources",
                  d.record("RunResourcesV1",
                           {{"current", project_resources(d, report.current)},
                            {"peak", project_resources(d, report.peak)},
                            {"store_current", project_resources(d, report.store_current)},
                            {"store_peak", project_resources(d, report.store_peak)},
                            {"required_evidence",
                             list(d, report.required_evidence,
                                  [&](const auto &s) { return project_spec(d, s); })},
                            {"plan", project_plan_identity(d, *plan)}}));
            auto *causes = d.list();
            if (report.cause) {
                d.item(causes, project_diagnostic(d, *report.cause));
            }
            if (report.secondary_cause) {
                d.item(causes, project_diagnostic(d, *report.secondary_cause));
            }
            child(d, root, "causes", causes);
            child(d, root, "outstanding_holds",
                  list(d, report.outstanding_holds, [&](const auto &e) {
                      return d.record(
                          "RetentionEventV1",
                          {{"kind", d.text("outstanding_at_close")},
                           {"allocation", d.u64(e.allocation)},
                           {"bytes", d.u64(e.bytes)},
                           {"age_steps", d.u64(e.age_steps)},
                           {"holder", d.text(label(e.holder))},
                           {"reason", d.text(label(e.reason))},
                           {"resource_kind", d.u64(static_cast<std::uint64_t>(e.resource_kind))},
                           {"step", d.u64(e.step)},
                           {"producer", d.optional(e.owner ? d.u64(e.owner->producer) : nullptr)},
                           {"publication",
                            d.optional(e.owner ? d.u64(e.owner->publication) : nullptr)}});
                  }));
            for (const auto &e : report.evidence) {
                if (e.absolute_path == spec.absolute_path || e.name == spec.name) {
                    return diagnostic(FailureCode::EvidenceIO,
                                      "snapshot inventory contains itself");
                }
            }
            child(d, root, "precommit_evidence_inventory",
                  list(d, report.evidence, [&](const auto &e) { return project(d, e); }));
            auto encoded = checked(d.render());
            return files::finish_open(fd, spec.name, spec.absolute_path, encoded.bytes(), account,
                                      spec.max_bytes, FailureCode::EvidenceIO);
        });
    }
};
Result<SnapshotEvidence> SnapshotEvidence::create(std::shared_ptr<const PreparedPlan> plan,
                                                  RequiredEvidenceSpec spec,
                                                  std::shared_ptr<ResourceAccount> account,
                                                  SnapshotLimits limits) noexcept {
    return wire::protect([&]() -> Result<SnapshotEvidence> {
        if (!plan || !account || account != plan->evidence_account() ||
            spec.format != "run_snapshot_v1" || spec.flush_policy != "on_close" ||
            spec.open_policy != "create_new" || spec.failure != "stop" ||
            std::find(plan->required_evidence().begin(), plan->required_evidence().end(), spec) ==
                plan->required_evidence().end()) {
            return diagnostic(FailureCode::InvalidConfig,
                              "snapshot service differs from frozen evidence contract");
        }
        const auto base = wire::checked(checked_add(
            sizeof(Data) + 4096, wire::checked(checked_multiply(spec.max_records, sizeof(Event)))));
        if (base > limits.capture_bytes ||
            limits.capture_bytes > plan->evidence_resource_bounds().graph ||
            limits.metadata.xml.workspace_bytes > plan->evidence_resource_bounds().scratch ||
            spec.max_bytes > plan->evidence_resource_bounds().staging) {
            return diagnostic(FailureCode::ResourcePlanExceeded,
                              "snapshot bounds exceed declared preparation resources");
        }
        auto charge = wire::checked(account->reserve(ResourceKind::Graph, limits.capture_bytes));
        auto hash = wire::checked(plan_digest(*plan, account, limits.metadata));
        auto data = std::shared_ptr<Data>(new Data{std::move(charge),
                                                   std::move(plan),
                                                   std::move(spec),
                                                   account,
                                                   limits,
                                                   hash,
                                                   {},
                                                   base,
                                                   -1,
                                                   false,
                                                   false});
        data->events.reserve(static_cast<std::size_t>(data->spec.max_records));
        return SnapshotEvidence{std::move(data)};
    });
}
RequiredEvidenceSink SnapshotEvidence::callbacks() const {
    return {{},
            data_->spec,
            [d = data_] { return d->open(); },
            [d = data_](auto i, const auto &frame) { return d->capture(i, frame); },
            [d = data_](const auto &g) { return d->capture_gap(g); },
            [d = data_](const auto &r) { return d->finalize(r); }};
}
Result<OwnedMetadata<SnapshotSummary>> decode_snapshot(std::string_view xml,
                                                       std::shared_ptr<ResourceAccount> account,
                                                       const MetadataLimits &limits) noexcept {
    return wire::protect([&]() -> Result<OwnedMetadata<SnapshotSummary>> {
        auto required =
            wire::checked(checked_add(4096, wire::checked(checked_multiply(xml.size(), 2))));
        if (required > limits.owned_bytes) {
            return diagnostic(FailureCode::ResourcePlanExceeded,
                              "snapshot ownership bound exceeded");
        }
        auto charge = wire::checked(account->reserve(ResourceKind::Graph, limits.owned_bytes));
        auto d = wire::checked(wire::Document::parse(xml, account, limits.xml));
        const auto &r = d.root();
        if (std::string_view{r.Name()} != "run_snapshot") {
            wire::fail("snapshot root mismatch");
        }
        wire::attributes(r, {"version", "run_id", "plan_digest", "numerical", "integration_outcome",
                             "host_contract_id", "host_contract_version",
                             "host_experiment_identity", "acceptance"});
        if (wire::attribute(r, "version") != "1" ||
            wire::number_attribute<std::uint32_t>(r, "host_contract_version") == 0 ||
            wire::attribute(r, "host_contract_id").empty()) {
            wire::fail("invalid snapshot version/host binding");
        }
        auto cs = wire::children(r);
        constexpr std::string_view names[]{
            "requests",  "outcomes", "counters",          "cache",
            "resources", "causes",   "outstanding_holds", "precommit_evidence_inventory"};
        if (cs.size() != std::size(names)) {
            wire::fail("snapshot child count mismatch");
        }
        for (std::size_t i = 0; i < cs.size(); ++i) {
            if (cs[i]->Name() != names[i]) {
                wire::fail("snapshot child order mismatch");
            }
            wire::attributes(*cs[i], {});
            cs[i] = &wire::only_child(*cs[i]);
            wire::validate_value(*cs[i]);
        }
        auto cache = wire::fields(*cs[3], "RunCacheV1");
        auto resources = wire::fields(*cs[4], "RunResourcesV1");
        SnapshotSummary summary{std::string{wire::attribute(r, "run_id")},
                                std::string{wire::attribute(r, "numerical")},
                                std::string{wire::attribute(r, "integration_outcome")},
                                std::string{wire::attribute(r, "acceptance")},
                                wire::text(*cache[0]),
                                wire::text(*cache[1]),
                                wire::digest_hex(wire::attribute(r, "plan_digest")),
                                wire::digest_hex(wire::attribute(r, "host_experiment_identity")),
                                {},
                                {},
                                {},
                                {}};
        one_of(summary.numerical, {"completed", "failed", "aborted"});
        one_of(summary.integration, {"completed", "failed", "aborted", "not_finished"});
        one_of(summary.acceptance, {"pending", "rejected"});
        one_of(summary.cache_status,
               {"disabled", "no_new_entries", "bypassed", "staged", "not_published"});
        for (auto *s : items(*resources[4])) {
            summary.required.push_back(decode_spec(*s));
        }
        for (auto *e : items(*cs[7])) {
            summary.inventory.push_back(wire::evidence(*e));
        }
        for (auto *e : items(*cache[6])) {
            summary.entries.push_back(decode_entry(*e));
        }
        auto plan_bytes = wire::canonical(*resources[5], 3, limits.xml.bytes);
        if (wire::checked(files::digest(plan_bytes)) != summary.plan_digest) {
            wire::fail("snapshot plan digest mismatch");
        }
        auto plan_fields = wire::fields(*resources[5], "PlanIdentityV1");
        auto composition = wire::fields(*plan_fields[0], "EffectiveCompositionV1");
        auto run = wire::fields(*composition[0], "RunConfigurationV1");
        if (wire::text(*run[0]) != summary.run_id) {
            wire::fail("snapshot run and effective plan disagree");
        }
        std::vector<RequiredEvidenceSpec> frozen_required;
        for (auto *spec : items(*plan_fields[6])) {
            frozen_required.push_back(decode_spec(*spec));
        }
        if (summary.required != frozen_required) {
            wire::fail("snapshot evidence policy differs from frozen plan");
        }
        for (std::size_t i = 0; i < summary.required.size(); ++i) {
            const auto &spec = summary.required[i];
            if (spec.name.empty() || spec.absolute_path.empty() ||
                spec.absolute_path.front() != '/' || spec.open_policy != "create_new" ||
                spec.failure != "stop" || !spec.max_bytes || !spec.max_records) {
                wire::fail("invalid frozen snapshot evidence specification");
            }
            for (std::size_t j = 0; j < i; ++j) {
                if (spec.name == summary.required[j].name ||
                    spec.absolute_path == summary.required[j].absolute_path) {
                    wire::fail("duplicate frozen evidence specification");
                }
            }
        }
        for (auto *n : items(*composition[5])) {
            auto fields = wire::fields(*n, "PreparedNodeV1");
            auto identity = wire::unhex(wire::text(*fields[1]), limits.xml.bytes);
            static_cast<void>(computation_body(identity));
            summary.computations.push_back(std::move(identity));
        }
        validate_events(*cs[1], summary.computations.size());
        for (const auto *request : items(*cs[0])) {
            const auto f = wire::fields(*request, "RequestEvidenceV1");
            one_of(wire::text(*f[3]), {"rejected", "completed", "partial", "not_reached"});
        }
        static_cast<void>(wire::fields(*cs[2], "RunCountersV1"));
        for (const auto *cause : items(*cs[5])) {
            static_cast<void>(wire::fields(*cause, "Diagnostic"));
        }
        for (const auto *hold : items(*cs[6])) {
            static_cast<void>(wire::fields(*hold, "RetentionEventV1"));
        }
        return OwnedMetadata<SnapshotSummary>{std::move(charge), std::move(summary)};
    });
}
} // namespace lexongine
