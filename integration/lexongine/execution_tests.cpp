#include "lexongine/execution.hpp"
#include "lexongine/reuse.hpp"
#include "fixture_source.hpp"
#include "file_evidence.hpp"
#include "xml_frontend.hpp"
#include "test_memory_store.hpp"
#include "quant/storage/file_integrity.hpp"
#if defined(LEXONGINE_HOST_SOURCE_BUILD)
#include "lexongine/plugins.hpp"
#include "lexongine/build_contract.hpp"
#endif
#include "../../tests/lexongine_test_support.hpp"

#include <fstream>
#include <filesystem>
#include <sstream>
#include <unistd.h>

namespace {
using namespace lexongine;
using namespace lexongine::test;
namespace ql = quant::lexongine;
using Price = quant::data::Price;
std::string example;
std::filesystem::path scratch_root;
Result<Sha256> digest(std::span<const std::byte> bytes) {
    const auto hex = quant::storage::files::sha256(bytes);
    Sha256 value{};
    for (std::size_t i = 0; i < value.size(); ++i) {
        unsigned item = 0;
        const auto parsed = std::from_chars(hex.data() + 2 * i, hex.data() + 2 * i + 2, item, 16);
        if (parsed.ec != std::errc{}) {
            return diagnostic(FailureCode::InvalidConfig, "test digest conversion");
        }
        value[i] = static_cast<std::byte>(item);
    }
    return value;
}
std::string disabled(std::string value, Driver driver) {
    const auto begin = value.find("<cache ");
    const auto end = value.find("/>", begin);
    require(begin != std::string::npos && end != std::string::npos,
            "fixture cache declaration absent");
    value.replace(begin, end + 2 - begin, "<cache mode=\"disabled\"/>");
    if (driver == Driver::Hosted) {
        const auto position = value.find("driver=\"standalone\"");
        value.replace(position, 19, "driver=\"hosted\"");
    }
    return value;
}
std::string replace(std::string value, std::string_view from, std::string_view to) {
    const auto position = value.find(from);
    require(position != std::string::npos, "fixture mutation not found");
    value.replace(position, from.size(), to);
    return value;
}
struct Environment {
    std::shared_ptr<ResourceAccount> account = take(ResourceAccount::create(
        {{2097152, 1048576, 8388608, 2097152, 1048576, 8388608, 4194304}, 2048, 1024, 8192, 128}));
    Registry registry = take(Registry::create());
    ql::ComponentRegistration build{
        {Sha256{std::byte{1}}, Sha256{std::byte{2}}, Sha256{std::byte{3}}, "execution-test-build"},
        {},
        MissingIntervalPolicy::Strict};
    ql::ComponentTypes types = take(ql::register_components(registry, build));
    SourceResolver sources = take(SourceResolver::create(account, 65536, 4));
    AccountLimits runtime{
        {0, 262144, 131072, 524288, 131072, 524288, 2097152}, 1024, 1024, 8192, 128};
    std::vector<RequiredEvidenceSpec> required_evidence;
    ResourceBounds evidence_resources{};
    explicit Environment(bool constant = false, bool derived = false, bool failing_effect = false,
                         std::optional<std::byte> source_revision = {},
                         std::optional<ImplementationIdentity> verified_build = {})
        : build{verified_build.value_or(
                    ImplementationIdentity{Sha256{std::byte{1}}, Sha256{std::byte{2}},
                                           Sha256{std::byte{3}}, "execution-test-build"}),
                {},
                MissingIntervalPolicy::Strict} {
        auto source_build = build;
        if (source_revision) {
            source_build.implementation.binary_digest[0] = *source_revision;
        }
        success(fixture::register_source(registry, sources, types.bar, source_build,
                                         {120, 65536, 131072, 65536}));
        if (constant) {
            auto protocol = [](std::string name) { return Contract{std::move(name), 1, {1, {}}}; };
            OperationContract description{
                {"test", "constant", 1, "test-v1"},
                build.implementation,
                {1, {{"value", ParamKind::Signed64, true, {}}}},
                {},
                {{"price", types.price.policy.contract, StorageKind::ScalarCopy,
                  protocol("price-codec"), protocol("price-validation"), false}},
                {SharingClass::StatelessDeterministic,
                 protocol("stateless"),
                 protocol("same_step"),
                 protocol("none"),
                 true,
                 true,
                 false,
                 {"constant-source-clock-test"}},
                {"interval_v1"},
                {Alignment::SameStep},
                {MissingIntervalPolicy::Strict},
                {"count"},
                "Explicit test constant on the declared source clock"};
            TypedOperation<std::int64_t, std::monostate, std::tuple<>, std::tuple<Price>> operation{
                description,
                {},
                {{"price", types.price}},
                [](const ParamSet &parameters) -> Result<std::int64_t> {
                    return parse_integer<std::int64_t>(parameters.values.at(0).text);
                },
                [](const std::int64_t &value) -> Result<CanonicalParams> {
                    return CanonicalParams{1, {{"value", value, Origin::ExplicitConfig, "test"}}};
                },
                [](const std::int64_t &, const PreparationFacts &) -> Result<HistoryRequirement> {
                    return HistoryRequirement{CountHistory{0}};
                },
                [](const std::int64_t &, const PreparationFacts &) -> Result<ResourceBounds> {
                    return ResourceBounds{0, 0, 0, 8192, 4096, 8192, 0};
                },
                [](const std::int64_t &, const InitializationSpec &,
                   StateAllocator &) -> Result<std::monostate> { return std::monostate{}; },
                [](std::monostate &, const std::int64_t &value, const std::tuple<> &,
                   const UpdateContext &, PublicationAllocator &) -> StepResult<std::tuple<Price>> {
                    return Published{std::tuple{Price::from_raw(value)}, ParentSelections::none()};
                },
                [](const std::int64_t &, const std::tuple<Price> &,
                   const UpdateContext &) -> Result<void> { return ok(); },
                {}};
            if (derived) {
                operation.canonicalize = [](const std::int64_t &value) -> Result<CanonicalParams> {
                    return CanonicalParams{
                        1,
                        {{"scale", std::uint64_t{9}, Origin::VersionedFact, "test Price9 fact"},
                         {"value", value, Origin::ExplicitConfig, "test"}}};
                };
            }
            if (failing_effect) {
                operation.contract.sharing.classification = SharingClass::Unique;
                operation.contract.sharing.effect_free = false;
                operation.contract.sharing.effect = protocol("test.required_effect");
                operation.history = [](const std::int64_t &,
                                       const PreparationFacts &) -> Result<HistoryRequirement> {
                    return diagnostic(FailureCode::InvalidConfig,
                                      "injected required-effect preparation failure");
                };
            }
            success(registry.register_definition(std::move(operation)));
        }
        success(registry.freeze());
        success(sources.freeze());
    }
    Result<PrepareResult> submit_text(std::string text, std::vector<EffectScope> effects = {},
                                      std::shared_ptr<ResultStore> store = {}) {
        auto composition = take(resolve_composition(
            take(parse_xml(text, {1048576, 32, 16384, 1024, account})), registry));
        PrepareOptions options{composition.declaration().run.id,
                               take(fixture::source_request(composition.declaration().sources[0])),
                               {account,
                                {{120, 120, 0, 120}, 1024, 32768, 131072, 32768},
                                MissingIntervalPolicy::Strict,
                                {1048576, 1048576, 1048576, 1048576, 1048576, 1048576, 1048576}},
                               runtime,
                               {1048576, 1048576, 1048576, 1048576, 1048576, 16384, 1048576},
                               131072,
                               1048576,
                               digest,
                               std::move(effects),
                               required_evidence,
                               required_evidence.empty() ? nullptr : account,
                               evidence_resources};
        return lexongine::prepare(composition, registry, sources, std::move(store), options);
    }
    PrepareResult prepare_text(std::string text, std::shared_ptr<ResultStore> store = {}) {
        return take(submit_text(std::move(text), {}, std::move(store)));
    }
};
struct ObservedPublication {
    ParentPublication publication;
    std::vector<ParentReference> parents;
    friend bool operator==(const ObservedPublication &, const ObservedPublication &) = default;
};
struct Seen {
    std::array<std::vector<std::int64_t>, 4> values;
    std::array<std::vector<ObservedPublication>, 4> metadata;
    std::uint64_t events = 0;
    bool opened = false, finalized = false;
};
RunServices services(const PreparedPlan &plan, const ql::ComponentTypes &types,
                     std::shared_ptr<Seen> seen, bool fail_close = false,
                     bool capture_metadata = false) {
    std::vector<ObservationAdapter> observers;
    for (std::size_t i = 0; i < plan.observations().size(); ++i) {
        seen->values[i].reserve(120);
        observers.push_back(
            make_observer<Price>(plan.observations()[i].request.id, types.price,
                                 [seen, i](const ReadValue<Price> &value,
                                           const RequestContext &context) -> Result<void> {
                                     require(context.phase == Phase::Evaluation,
                                             "observer received an unintended warmup action");
                                     seen->values[i].push_back(value.value.raw());
                                     return ok();
                                 }));
        if (capture_metadata) {
            seen->metadata[i].reserve(120);
            auto deliver = std::move(observers.back().deliver);
            observers.back().deliver = [deliver = std::move(deliver), seen,
                                        i](const RequestContext &request, const ResultSlot &slot,
                                           PortIndex port,
                                           const UpdateContext &context) -> Result<void> {
                auto result = deliver(request, slot, port, context);
                if (const auto *error = std::get_if<Diagnostic>(&result)) {
                    return *error;
                }
                const auto *frame = slot.current();
                ObservedPublication metadata{frame->certificates[port]->publication, {}};
                for (const auto &parent : frame->parents.entries()) {
                    metadata.parents.push_back({*parent.input_port(), parent.metadata()});
                }
                seen->metadata[i].push_back(std::move(metadata));
                return ok();
            };
        }
    }
    ExecutionEvidence sink{
        {},
        plan.evidence_specification(),
        [seen]() -> Result<void> {
            seen->opened = true;
            return ok();
        },
        [seen](const RetentionEvent &) -> Result<void> {
            require(seen->opened && !seen->finalized, "evidence outside open lifetime");
            ++seen->events;
            return ok();
        },
        [seen, fail_close](const RunReport &) -> Result<void> {
            seen->finalized = true;
            return fail_close
                       ? Result<void>{diagnostic(FailureCode::EvidenceIO, "injected close failure")}
                       : ok();
        }};
    const auto hosted = plan.configuration().run.driver == Driver::Hosted;
    return {std::move(observers),
            std::move(sink),
            {IntegrationOutcome::Completed, std::nullopt,
             hosted ? "test.hosted" : "test.standalone", 1,
             Sha256{hosted ? std::byte{10} : std::byte{9}}}};
}
void configured_fixture_executes_once() {
    Environment environment;
    auto result = environment.prepare_text(disabled(example, Driver::Standalone));
    require(result.plan && result.rejected.empty(), "fixture preparation rejected requests");
    require(result.plan->graph().logical_order().size() == 6 &&
                result.plan->graph().physical_order().size() == 5,
            "fixture six/five sharing failed");
    auto seen = std::make_shared<Seen>();
    auto run = take(instantiate(result.plan, services(*result.plan, environment.types, seen)));
    auto report = take(execute(run));
    require(report.numerical == NumericalOutcome::Completed &&
                report.acceptance == AcceptanceStatus::Accepted && seen->finalized,
            "fixture run did not complete accepted evidence");
    for (const auto &values : seen->values) {
        require(values.size() == 71,
                "fixture observation count differs from independent reference");
    }
    require(seen->values[0].front() == 100395000000 && seen->values[2].front() == 100245000000 &&
                seen->values[0].back() == 101095000000 && seen->values[2].back() == 100945000000,
            "fixture means differ from independent reference");
    require(seen->values[0] == seen->values[1] &&
                std::all_of(seen->values[3].begin(), seen->values[3].end(),
                            [](auto value) { return value == 150000000; }),
            "alias or spread changed values");
    require(report.producers[0].publications == 120 && report.producers[2].advances == 120 &&
                report.producers[2].publications == 101 && report.producers[4].publications == 71,
            "actual producer advance/publication counters changed");
    std::cout << "FIXTURE logical=" << result.plan->graph().logical_order().size()
              << " physical=" << result.plan->graph().physical_order().size()
              << " source=" << report.complete_updates << " observations=" << seen->values[0].size()
              << ',' << seen->values[1].size() << ',' << seen->values[2].size() << ','
              << seen->values[3].size() << " spread_raw=" << seen->values[3].front() << '\n';
    rejects(execute(run), FailureCode::AlreadyExecuted);
}
void source_bound_rejection_is_attributed() {
    Environment environment;
    auto text = disabled(example, Driver::Standalone);
    const auto bound = text.find("max_abs_input_raw\" value=\"102000000000");
    require(bound != std::string::npos, "SMA bound missing");
    const auto value = text.find("102000000000", bound);
    text.replace(value, 12, "100500000000");
    auto result = environment.prepare_text(std::move(text));
    require(result.plan && result.rejected.size() == 2 && result.plan->observations().size() == 2,
            "known bound failure was not confined to dependent requests");
    for (const auto &rejected : result.rejected) {
        require(rejected.cause.code == FailureCode::SourceKnownBoundViolation &&
                    rejected.cause.key && rejected.cause.key->sequence == 51,
                "source-known rejection lost its original key or was classified as corruption");
    }
}
void hosted_matches_standalone_and_close_failure_rejects() {
    Environment first, second;
    auto a = first.prepare_text(disabled(example, Driver::Standalone));
    auto b = second.prepare_text(disabled(example, Driver::Hosted));
    auto seen_a = std::make_shared<Seen>(), seen_b = std::make_shared<Seen>();
    auto standalone = take(instantiate(a.plan, services(*a.plan, first.types, seen_a)));
    auto hosted = take(instantiate(b.plan, services(*b.plan, second.types, seen_b)));
    static_cast<void>(take(execute(standalone)));
    std::uint64_t processed = 0;
    for (;;) {
        auto step = take(advance_one(hosted));
        if (step.status == StepStatus::Exhausted) {
            break;
        }
        require(step.status == StepStatus::Processed && step.source.has_value(),
                "host lost a source/warmup update");
        ++processed;
    }
    auto report = take(finish(hosted, {IntegrationOutcome::Completed, std::nullopt, "test.hosted",
                                       1, Sha256{std::byte{10}}}));
    require(processed == 120 && report.acceptance == AcceptanceStatus::Accepted &&
                seen_a->values == seen_b->values,
            "hosted driver changed values or reset state");
    rejects(advance_one(hosted), FailureCode::AlreadyExecuted);
    auto failed_seen = std::make_shared<Seen>();
    auto failed = take(instantiate(a.plan, services(*a.plan, first.types, failed_seen, true)));
    auto failed_report = take(execute(failed));
    require(failed_report.numerical == NumericalOutcome::Completed &&
                failed_report.acceptance == AcceptanceStatus::Rejected && failed_report.cause &&
                failed_report.cause->code == FailureCode::EvidenceIO,
            "evidence close failure was accepted");
}
void sharing_and_lag_match_reference() {
    Environment shared_environment, unshared_environment, lag_environment;
    // Extra physical producers emit more ownership evidence. Compare successful
    // runs under the same explicitly enlarged evidence ceiling; the original
    // insufficient-ceiling failure is retained as a separate fail-stop test.
    const auto comparison =
        replace(disabled(example, Driver::Standalone), "retention_log_max_events=\"4096\"",
                "retention_log_max_events=\"8192\"");
    auto shared_plan = shared_environment.prepare_text(comparison);
    auto unshared_plan = unshared_environment.prepare_text(
        replace(comparison, "sharing=\"exact\"", "sharing=\"disabled\""));
    auto a = std::make_shared<Seen>(), b = std::make_shared<Seen>();
    auto shared = take(
        instantiate(shared_plan.plan, services(*shared_plan.plan, shared_environment.types, a)));
    auto unshared = take(instantiate(unshared_plan.plan,
                                     services(*unshared_plan.plan, unshared_environment.types, b)));
    const auto shared_report = take(execute(shared)), unshared_report = take(execute(unshared));
    if (shared_report.cause) {
        throw std::runtime_error(std::string{shared_report.cause->text()});
    }
    if (unshared_report.cause) {
        throw std::runtime_error(std::string{unshared_report.cause->text()});
    }
    require(a->values == b->values && shared_report.producers[3].advances == 0 &&
                unshared_report.producers[3].advances == 120,
            "sharing changed observations or failed to remove duplicate advances");
    auto text = comparison;
    const auto insertion = text.find("    <node id=\"spread\"");
    require(insertion != std::string::npos, "spread declaration missing");
    text.insert(insertion, "    <node id=\"lagged_slow\" operation=\"series.lag_price\" "
                           "semantic_version=\"1\" implementation_id=\"builtin-v1\" "
                           "alignment=\"same_step\" initialization=\"empty_at_step_0\">\n"
                           "      <param name=\"lag_steps\" value=\"1\"/><param "
                           "name=\"count_basis\" value=\"complete_bar_updates\"/>\n"
                           "      <input port=\"price\" producer=\"slow\" output=\"mean\" "
                           "prior_outputs=\"0\" missing_intervals=\"strict\"/>\n"
                           "    </node>\n");
    text = replace(std::move(text), "port=\"right\" producer=\"slow\" output=\"mean\"",
                   "port=\"right\" producer=\"lagged_slow\" output=\"lagged\"");
    for (auto position = text.find("begin_step=\"49\""); position != std::string::npos;
         position = text.find("begin_step=\"49\"")) {
        text.replace(position, 15, "begin_step=\"50\"");
    }
    auto prepared = lag_environment.prepare_text(std::move(text));
    require(prepared.rejected.empty() && prepared.plan,
            "lag fixture did not admit its required history");
    auto lag_seen = std::make_shared<Seen>();
    auto lagged =
        take(instantiate(prepared.plan, services(*prepared.plan, lag_environment.types, lag_seen)));
    const auto report = take(execute(lagged));
    if (report.cause) {
        throw std::runtime_error(std::string{report.cause->text()});
    }
    require(report.acceptance == AcceptanceStatus::Accepted && lag_seen->values[3].size() == 70 &&
                std::all_of(lag_seen->values[3].begin(), lag_seen->values[3].end(),
                            [](auto value) { return value == 160000000; }),
            "integrated lag differs from the independent 0.16 reference");
}
void retention_quota_is_fail_stop() {
    Environment environment;
    auto prepared = environment.prepare_text(replace(disabled(example, Driver::Standalone),
                                                     "sharing=\"exact\"", "sharing=\"disabled\""));
    auto seen = std::make_shared<Seen>();
    auto run = take(instantiate(prepared.plan, services(*prepared.plan, environment.types, seen)));
    const auto report = take(execute(run));
    require(report.acceptance == AcceptanceStatus::Rejected && report.cause &&
                report.cause->code == FailureCode::EvidenceQuota && report.complete_updates < 120 &&
                seen->events == 4096,
            "required retention quota did not stop the admitted run");
}
void observer_failure_preserves_logical_prefix() {
    Environment environment;
    auto prepared = environment.prepare_text(disabled(example, Driver::Hosted));
    auto seen = std::make_shared<Seen>();
    auto callbacks = services(*prepared.plan, environment.types, seen);
    callbacks.observers[0] = make_observer<Price>(
        "fast_A_trial", environment.types.price,
        [](const ReadValue<Price> &, const RequestContext &) -> Result<void> {
            return diagnostic(FailureCode::CallbackException, "injected first observer failure");
        });
    auto run = take(instantiate(prepared.plan, std::move(callbacks)));
    for (std::uint64_t key = 0; key < 49; ++key) {
        require(take(advance_one(run)).status == StepStatus::Processed,
                "warmup failed before injected observer");
    }
    auto stopped = take(advance_one(run));
    require(stopped.status == StepStatus::Stopped && stopped.key->sequence == 49 &&
                !stopped.observations[1].delivered && !stopped.observations[1].value.frame(),
            "failure eagerly exposed a later logical alias");
    require(stopped.reach[2].reached && stopped.reach[2].published && !stopped.reach[3].reached &&
                !stopped.reach[4].reached,
            "receipt lost the logical failure boundary after the first physical publication");
    auto report = take(finish(
        run, {IntegrationOutcome::Failed, stopped.cause, "test.hosted", 1, Sha256{std::byte{10}}}));
    require(report.acceptance == AcceptanceStatus::Rejected &&
                report.producers[2].publications == 31 && report.observations[1].delivered == 0,
            "observer failure lost physical publication or delivered later alias");
}
void early_completion_and_host_failure_reject() {
    Environment environment;
    auto prepared = environment.prepare_text(disabled(example, Driver::Hosted));
    auto seen = std::make_shared<Seen>();
    auto run = take(instantiate(prepared.plan, services(*prepared.plan, environment.types, seen)));
    const IntegrationCompletion completed{
        IntegrationOutcome::Completed, {}, "test.hosted", 1, Sha256{std::byte{10}}};
    rejects(finish(run, completed), FailureCode::SourceCoverage);
    auto first = take(advance_one(run));
    rejects(first.source->read<quant::data::Bar>(0, *first.available_at_ns - 1),
            FailureCode::FutureRead);
    auto retained_source = std::move(*first.source);
    first.source.reset();
    const auto error = diagnostic(FailureCode::CallbackException, "injected host decision failure");
    success(fail_host(run, error));
    rejects(advance_one(run), FailureCode::UpdateProtocol);
    auto report = take(
        finish(run, {IntegrationOutcome::Failed, error, "test.hosted", 1, Sha256{std::byte{10}}}));
    require(report.acceptance == AcceptanceStatus::Rejected && report.complete_updates == 1 &&
                !report.outstanding_holds.empty(),
            "host failure accepted partial scope or lost external holds");
    require(
        take(retained_source.read<quant::data::Bar>(0, *first.available_at_ns)).value.close.raw() ==
            100000000000,
        "source receipt was invalidated by terminal cleanup");
    rejects(fail_host(run, error), FailureCode::AlreadyExecuted);
}
void history_and_global_resource_admission() {
    Environment early, small, enormous;
    auto text = disabled(example, Driver::Standalone);
    for (auto position = text.find("begin_step=\"49\""); position != std::string::npos;
         position = text.find("begin_step=\"49\"")) {
        text.replace(position, 15, "begin_step=\"48\"");
    }
    auto admitted = early.prepare_text(std::move(text));
    require(admitted.plan && admitted.rejected.size() == 2 &&
                admitted.plan->observations().size() == 2,
            "insufficient history was shifted or rejected independent fast requests");
    for (const auto &request : admitted.plan->observations()) {
        require(request.request.evaluation.begin == 48,
                "preparation shifted an evaluation boundary");
    }
    for (const auto &rejected : admitted.rejected) {
        require(rejected.cause.code == FailureCode::HistoryInsufficient,
                "history rejection lost its domain code");
    }
    rejects(small.submit_text(replace(disabled(example, Driver::Standalone),
                                      "managed_buffer_bytes=\"8388608\"",
                                      "managed_buffer_bytes=\"1048576\"")),
            FailureCode::ResourcePlanExceeded);
    auto huge = replace(disabled(example, Driver::Standalone), "name=\"window\" value=\"20\"",
                        "name=\"window\" value=\"9223372036854775807\"");
    huge = replace(std::move(huge), "name=\"max_abs_input_raw\" value=\"102000000000\"",
                   "name=\"max_abs_input_raw\" value=\"1\"");
    rejects(enormous.submit_text(std::move(huge)), FailureCode::ResourcePlanExceeded);
    Environment baseline, report_limited;
    const auto complete = baseline.prepare_text(disabled(example, Driver::Standalone));
    report_limited.runtime.bytes.evidence = complete.plan->required_resources().evidence - 1;
    rejects(report_limited.submit_text(disabled(example, Driver::Standalone)),
            FailureCode::ResourcePlanExceeded);
}
void reentrant_driver_calls_are_rejected() {
    Environment environment;
    auto prepared = environment.prepare_text(disabled(example, Driver::Hosted));
    auto seen = std::make_shared<Seen>();
    auto callbacks = services(*prepared.plan, environment.types, seen);
    RunInstance *active = nullptr;
    bool checked_reentry = false;
    callbacks.observers[0] = make_observer<Price>(
        "fast_A_trial", environment.types.price,
        [&](const ReadValue<Price> &, const RequestContext &) -> Result<void> {
            if (!checked_reentry) {
                checked_reentry = true;
                rejects(advance_one(*active), FailureCode::UpdateProtocol);
                rejects(finish(*active, {IntegrationOutcome::Failed,
                                         diagnostic(FailureCode::CallbackException, "nested close"),
                                         "test.hosted", 1, Sha256{std::byte{10}}}),
                        FailureCode::UpdateProtocol);
            }
            return ok();
        });
    auto run = take(instantiate(prepared.plan, std::move(callbacks)));
    active = &run;
    for (;;) {
        auto receipt = take(advance_one(run));
        if (receipt.status == StepStatus::Exhausted) {
            break;
        }
        require(receipt.status == StepStatus::Processed,
                "reentry rejection corrupted the active update");
    }
    auto report = take(
        finish(run, {IntegrationOutcome::Completed, {}, "test.hosted", 1, Sha256{std::byte{10}}}));
    require(checked_reentry && report.acceptance == AcceptanceStatus::Accepted &&
                report.complete_updates == 120,
            "reentrant call advanced or finalized the active run");
}
std::string constant_configuration() {
    auto text = disabled(example, Driver::Standalone);
    const auto nodes_begin = text.find("<nodes>"), nodes_end = text.find("</nodes>");
    text.replace(
        nodes_begin, nodes_end + 8 - nodes_begin,
        "<nodes><node id=\"constant\" operation=\"test.constant\" semantic_version=\"1\" "
        "implementation_id=\"test-v1\" alignment=\"same_step\" initialization=\"stateless\"><param "
        "name=\"value\" value=\"42\"/></node></nodes>");
    const auto begin = text.find("<observations>"), end = text.find("</observations>");
    text.replace(begin, end + 15 - begin,
                 "<observations><observe id=\"constant_trial\" producer=\"constant\" "
                 "output=\"price\" begin_step=\"0\" end_step=\"120\" absence=\"require_value\" "
                 "prior_outputs=\"0\" missing_intervals=\"strict\"/></observations>");
    return text;
}
void source_clock_survives_independent_computation() {
    Environment environment{true};
    auto prepared = environment.prepare_text(constant_configuration());
    require(prepared.plan && prepared.plan->needed(0),
            "admission removed the required source clock when no numeric edge referenced it");
    auto seen = std::make_shared<Seen>();
    auto run = take(instantiate(prepared.plan, services(*prepared.plan, environment.types, seen)));
    auto report = take(execute(run));
    require(report.acceptance == AcceptanceStatus::Accepted && report.complete_updates == 120 &&
                seen->values[0].size() == 120 &&
                std::all_of(seen->values[0].begin(), seen->values[0].end(),
                            [](auto value) { return value == 42; }),
            "independent computation lost source-clock advancement");
}
void zero_input_identity_includes_driver_source() {
    Environment first{true}, changed{true, false, false, std::byte{7}}, masked{true};
    auto a = first.prepare_text(constant_configuration()),
         b = changed.prepare_text(constant_configuration());
    auto mask = constant_configuration();
    const auto at = mask.find("<observe ");
    mask.replace(at, mask.size() - at,
                 replace(mask.substr(at), "begin_step=\"0\"", "begin_step=\"1\""));
    auto c = masked.prepare_text(std::move(mask));
    require(a.plan->identity(0) != b.plan->identity(0),
            "source mutation did not alter source identity");
    require(a.plan->identity(1) != b.plan->identity(1),
            "zero-input identity omitted its driver-source dependency");
    require(a.plan->identity(1) == c.plan->identity(1),
            "observation mask became a pure producer input");
}
void effective_prepared_parameters_enter_identity() {
    Environment ordinary{true}, derived{true, true};
    auto a = ordinary.prepare_text(constant_configuration()),
         b = derived.prepare_text(constant_configuration());
    require(b.plan->nodes()[1].spec.parameters == b.plan->kernel(1)->parameters() &&
                b.plan->nodes()[1].spec.parameters.values.size() == 2,
            "prepared canonical facts were lost before identity freezing");
    require(a.plan->identity(1) != b.plan->identity(1),
            "derived semantic fact did not affect computation identity");
}
void failed_required_effect_cannot_disappear() {
    Environment environment{true, false, true};
    auto text = disabled(example, Driver::Standalone);
    text.insert(text.find("</nodes>"),
                "<node id=\"bad_effect\" operation=\"test.constant\" semantic_version=\"1\" "
                "implementation_id=\"test-v1\" alignment=\"same_step\" "
                "initialization=\"stateless\"><param name=\"value\" value=\"42\"/></node>");
    auto result =
        environment.submit_text(std::move(text), {{"bad_effect", {"bar-clock-v1", 49, 120}}});
    rejects(std::move(result), FailureCode::InvalidConfig);
}
void receipt_workspace_is_admitted_before_execution() {
    Environment environment;
    environment.runtime.bytes.staging = 131072;
    rejects(environment.submit_text(disabled(example, Driver::Standalone)),
            FailureCode::ResourcePlanExceeded);
}
void provenance_geometry_is_admitted_before_execution() {
    Environment baseline;
    auto prepared = baseline.prepare_text(disabled(example, Driver::Standalone));
    auto author_and_receipt = prepared.plan->metadata_bytes() + 8192;
    for (const auto i : prepared.plan->graph().physical_order()) {
        if (const auto *kernel = prepared.plan->kernel(i)) {
            author_and_receipt += kernel->resources().staging;
        }
    }
    require(prepared.plan->required_resources().staging > author_and_receipt,
            "admission omitted host publication frames and retained provenance beyond "
            "author/receipt budgets");
    Environment environment;
    environment.runtime.bytes.staging = author_and_receipt;
    rejects(environment.submit_text(disabled(example, Driver::Standalone)),
            FailureCode::ResourcePlanExceeded);
}
void complete_artifact_identity_and_collision_comparison() {
    Environment environment;
    auto prepared = environment.prepare_text(disabled(example, Driver::Standalone));
    const Contract codec{"qaedalus.price9_xml", 1, {1, {}}},
        validation{"lexongine.price_payload", 1, {1, {}}};
    const IdentityLimits limits{1048576, 1024, 16384, 4194304};
    auto first = take(ArtifactIdentity::create(*prepared.plan, 2, codec, validation, digest,
                                               environment.account, limits));
    auto alias = take(ArtifactIdentity::create(*prepared.plan, 3, codec, validation, digest,
                                               environment.account, limits));
    require(first == alias && first.digest() == alias.digest() &&
                first.processing_scope() == prepared.plan->nodes()[2].facts.processing_scope,
            "artifact identity changed between exact aliases");
    // Independently assemble the complete wire record from the schema's fixed
    // fields; the already-tested computation record is embedded without a prefix.
    using Bytes = std::vector<std::byte>;
    auto number = [](std::uint64_t value) {
        Bytes out;
        for (int i = 7; i >= 0; --i) {
            out.push_back(static_cast<std::byte>((value >> (i * 8)) & 255));
        }
        return out;
    };
    auto text = [](std::string_view value) {
        const auto bytes = std::as_bytes(std::span{value.data(), value.size()});
        return Bytes{bytes.begin(), bytes.end()};
    };
    auto append = [](Bytes &into, const Bytes &bytes) {
        into.insert(into.end(), bytes.begin(), bytes.end());
    };
    auto field = [&](Bytes &into, std::uint16_t tag, const Bytes &bytes) {
        into.push_back(static_cast<std::byte>(tag >> 8));
        into.push_back(static_cast<std::byte>(tag & 255));
        append(into, number(bytes.size()));
        append(into, bytes);
    };
    Bytes empty_params;
    field(empty_params, 1, number(1));
    field(empty_params, 2, number(0));
    auto contract_bytes = [&](std::string_view name) {
        Bytes out;
        field(out, 1, text(name));
        field(out, 2, number(1));
        field(out, 3, empty_params);
        return out;
    };
    Bytes scope;
    field(scope, 1, text("bar-clock-v1"));
    field(scope, 2, number(0));
    field(scope, 3, number(120));
    Bytes scale_value{std::byte{2}};
    append(scale_value, number(9));
    Bytes scale;
    field(scale, 1, text("scale"));
    field(scale, 2, scale_value);
    Bytes values = number(1);
    append(values, number(scale.size()));
    append(values, scale);
    Bytes meaning;
    field(meaning, 1, number(1));
    field(meaning, 2, values);
    Bytes price;
    field(price, 1, text("qaedalus.price"));
    field(price, 2, number(1));
    field(price, 3, meaning);
    Bytes types = number(1);
    append(types, number(price.size()));
    append(types, price);
    constexpr std::string_view domain = "lexongine.identity.v1";
    Bytes expected = number(domain.size());
    append(expected, text(domain));
    expected.push_back(std::byte{0});
    expected.push_back(std::byte{2});
    const auto &computation = prepared.plan->graph().identity(2);
    field(expected, 1,
          Bytes{computation.begin() + static_cast<std::ptrdiff_t>(8 + domain.size() + 2),
                computation.end()});
    field(expected, 2, scope);
    field(expected, 3, types);
    field(expected, 4, contract_bytes("qaedalus.price9_xml"));
    field(expected, 5, contract_bytes("lexongine.price_payload"));
    require(std::ranges::equal(first.canonical_bytes(), expected) &&
                first.digest() == take(digest(expected)),
            "artifact canonical bytes differ from independent schema vector");
    auto collision =
        +[](std::span<const std::byte>) -> Result<Sha256> { return Sha256{std::byte{99}}; };
    auto a = take(ArtifactIdentity::create(*prepared.plan, 2, codec, validation, collision,
                                           environment.account, limits));
    auto b = take(ArtifactIdentity::create(*prepared.plan, 4, codec, validation, collision,
                                           environment.account, limits));
    require(a.digest() == b.digest() && !(a == b),
            "artifact digest collision bypassed full canonical equality");
    auto changed_codec = codec;
    changed_codec.version = 2;
    auto c = take(ArtifactIdentity::create(*prepared.plan, 2, changed_codec, validation, digest,
                                           environment.account, limits));
    require(!(first == c), "artifact codec version omitted from identity");
    auto small = limits;
    small.canonical_bytes = 1;
    rejects(ArtifactIdentity::create(*prepared.plan, 2, codec, validation, digest,
                                     environment.account, small),
            FailureCode::ResourcePlanExceeded);
    small = limits;
    small.metadata_bytes = 1;
    rejects(ArtifactIdentity::create(*prepared.plan, 2, codec, validation, digest,
                                     environment.account, small),
            FailureCode::ResourcePlanExceeded);
    small = limits;
    small.digest_scratch_bytes = 1;
    rejects(ArtifactIdentity::create(*prepared.plan, 2, codec, validation, digest,
                                     environment.account, small),
            FailureCode::ResourcePlanExceeded);
}
#if defined(LEXONGINE_HOST_SOURCE_BUILD)
void actual_builtin_build_binding() {
    auto bootstrap = take(ResourceAccount::create(
        {{1048576, 1048576, 268435456, 1048576, 1048576, 4194304, 1048576}, 256, 128, 4096, 128}));
    const HostBuildLimits limits{67108864, 1048576, 512};
    auto host =
        take(inspect_host_build(build_contract_id, LEXONGINE_HOST_SOURCE_BUILD, bootstrap, limits));
    require(host.identity().binary_digest != Sha256{} &&
                host.identity().source_build_digest != Sha256{} &&
                host.identity().dependencies_digest != Sha256{} && !host.dependencies().empty() &&
                host.platform_identity().find("kern.osversion=") != std::string_view::npos,
            "actual builtin build evidence is incomplete");
    auto repeated =
        take(inspect_host_build(build_contract_id, LEXONGINE_HOST_SOURCE_BUILD, bootstrap, limits));
    require(repeated.identity() == host.identity(), "unchanged host build identity is unstable");
    rejects(inspect_host_build("wrong-sdk", LEXONGINE_HOST_SOURCE_BUILD, bootstrap, limits),
            FailureCode::UnsupportedContract);
    rejects(inspect_host_build(build_contract_id, std::string(64, '0'), bootstrap, limits),
            FailureCode::SourceChanged);
    auto small = limits;
    small.file_bytes = 1;
    rejects(inspect_host_build(build_contract_id, LEXONGINE_HOST_SOURCE_BUILD, bootstrap, small),
            FailureCode::InvalidConfig);
    Environment environment{false, false, false, {}, host.identity()};
    auto prepared = environment.prepare_text(disabled(example, Driver::Standalone));
    require(prepared.plan->source().metadata().provider_implementation == host.identity() &&
                prepared.plan->nodes()[2].definition->contract().implementation == host.identity(),
            "builtin/source registration lost verified build binding");
    auto seen = std::make_shared<Seen>();
    auto run = take(instantiate(prepared.plan, services(*prepared.plan, environment.types, seen)));
    auto report = take(execute(run));
    require(report.acceptance == AcceptanceStatus::Accepted && seen->values[3].size() == 71 &&
                seen->values[3][0] == 150000000,
            "actual-build-bound fixture changed accepted results");
}
#endif
std::string small_cache_configuration(std::uint64_t end = 3) {
    auto text =
        replace(example, "managed_buffer_bytes=\"8388608\"", "managed_buffer_bytes=\"33554432\"");
    for (const auto &[from, to] : std::array<std::pair<std::string, std::string>, 4>{
             {{"name=\"window\" value=\"20\"", "name=\"window\" value=\"2\""},
              {"name=\"window\" value=\"50\"", "name=\"window\" value=\"3\""},
              {"begin_step=\"49\"", "begin_step=\"2\""},
              {"end_step=\"120\"", "end_step=\"" + std::to_string(end) + "\""}}}) {
        for (auto position = text.find(from); position != std::string::npos;
             position = text.find(from)) {
            text.replace(position, from.size(), to);
        }
    }
    return text;
}
void in_memory_full_scope_replay() {
    Environment first, second, extended;
    const auto config = small_cache_configuration();
    auto parsed = take(parse_xml(config, {1048576, 32, 16384, 1024, first.account}));
    auto memory = MemoryStore::make(std::get<LocalCacheConfiguration>(parsed.declaration().cache),
                                    first.types.price);
    auto cold_plan = first.prepare_text(config, memory.store());
    auto cold_seen = std::make_shared<Seen>();
    auto cold = take(instantiate(cold_plan.plan,
                                 services(*cold_plan.plan, first.types, cold_seen, false, true)));
    auto cold_report = take(execute(cold));
    require(cold_report.acceptance == AcceptanceStatus::Accepted &&
                cold_report.cache == CachePublicationStatus::Published && memory.entries() == 4,
            "completed cold run did not publish four physical numerical artifacts");
    auto warm_plan = second.prepare_text(config, memory.store());
    auto warm_seen = std::make_shared<Seen>();
    auto warm = take(instantiate(warm_plan.plan,
                                 services(*warm_plan.plan, second.types, warm_seen, false, true)));
    auto warm_report = take(execute(warm));
    require(warm_report.acceptance == AcceptanceStatus::Accepted &&
                warm_report.cache == CachePublicationStatus::NoNewEntries &&
                warm_seen->values == cold_seen->values &&
                warm_seen->values[3] == std::vector<std::int64_t>{5000000},
            "cache replay changed logical results or wrote an unnecessary origin");
    require(warm_seen->metadata == cold_seen->metadata,
            "replay changed original timing, availability or attributed parent lineage");
    for (const auto producer : {1U, 2U, 4U, 5U}) {
        require(warm_report.producers[producer].advances == 0 &&
                    warm_report.producers[producer].cached_deliveries == 3,
                "cached startup or dependency waiting invoked math or lost a delivery");
    }
    require(warm_report.producers[0].advances == 3 && warm_report.producers[3].aliases == 3,
            "reuse suppressed source delivery or logical aliases");
    auto extended_plan = extended.prepare_text(small_cache_configuration(4), memory.store());
    require(extended_plan.plan->source().metadata().content_digest ==
                cold_plan.plan->source().metadata().content_digest,
            "scope extension changed the fixed source snapshot");
    auto extended_seen = std::make_shared<Seen>();
    auto extension = take(instantiate(
        extended_plan.plan, services(*extended_plan.plan, extended.types, extended_seen)));
    const auto extended_report = take(execute(extension));
    require(extended_report.acceptance == AcceptanceStatus::Accepted &&
                extended_report.producers[2].advances == 4 &&
                extended_report.producers[2].cached_deliveries == 0 &&
                extended_seen->values[3] == std::vector<std::int64_t>({5000000, 5000000}),
            "scope extension reused an old mean as state or changed recomputed results");
}
void memory_store_evidence_and_corruption_gates() {
    Environment first, second;
    const auto config = small_cache_configuration();
    auto parsed = take(parse_xml(config, {1048576, 32, 16384, 1024, first.account}));
    auto memory = MemoryStore::make(std::get<LocalCacheConfiguration>(parsed.declaration().cache),
                                    first.types.price);
    auto plan = first.prepare_text(config, memory.store());
    auto seen = std::make_shared<Seen>();
    auto failed = take(instantiate(plan.plan, services(*plan.plan, first.types, seen, true)));
    auto failure = take(execute(failed));
    require(failure.numerical == NumericalOutcome::Completed &&
                failure.acceptance == AcceptanceStatus::Rejected && memory.entries() == 0,
            "required evidence failure exposed reusable entries");
    auto success_seen = std::make_shared<Seen>();
    auto completed = take(instantiate(plan.plan, services(*plan.plan, first.types, success_seen)));
    require(take(execute(completed)).acceptance == AcceptanceStatus::Accepted &&
                memory.entries() == 4,
            "fresh retry did not reconstruct and publish complete streams");
    memory.corrupt_first_payload();
    rejects(second.submit_text(config, {}, memory.store()), FailureCode::CacheCorrupt);
}
void memory_store_preflight_pinning_and_disabled_access() {
    Environment cold_environment, warm_environment, bad_environment, disabled_environment;
    const auto config = small_cache_configuration();
    auto parsed = take(parse_xml(config, {1048576, 32, 16384, 1024, cold_environment.account}));
    auto memory = MemoryStore::make(std::get<LocalCacheConfiguration>(parsed.declaration().cache),
                                    cold_environment.types.price);
    auto cold_plan = cold_environment.prepare_text(config, memory.store());
    auto cold_seen = std::make_shared<Seen>();
    auto cold = take(
        instantiate(cold_plan.plan, services(*cold_plan.plan, cold_environment.types, cold_seen)));
    require(take(execute(cold)).acceptance == AcceptanceStatus::Accepted,
            "could not seed pinning test store");
    auto warm_plan = warm_environment.prepare_text(config, memory.store());
    memory.corrupt_first_payload();
    auto warm_seen = std::make_shared<Seen>();
    auto warm = take(
        instantiate(warm_plan.plan, services(*warm_plan.plan, warm_environment.types, warm_seen)));
    require(take(execute(warm)).acceptance == AcceptanceStatus::Accepted &&
                warm_seen->values == cold_seen->values,
            "validated bytes were replaced after preparation");
    const auto before = memory.lookups();
    auto bad = config;
    constexpr std::string_view bound = "name=\"max_abs_input_raw\" value=\"102000000000\"";
    for (auto position = bad.find(bound); position != std::string::npos;
         position = bad.find(bound)) {
        bad.replace(position, bound.size(), "name=\"max_abs_input_raw\" value=\"0\"");
    }
    auto rejected = bad_environment.prepare_text(std::move(bad), memory.store());
    require(!rejected.plan && rejected.rejected.size() == 4 && memory.lookups() == before,
            "prior artifacts bypassed full source-known admission");
    auto off =
        disabled_environment.prepare_text(disabled(config, Driver::Standalone), memory.store());
    require(!off.plan->store() && memory.lookups() == before,
            "disabled cache accessed the supplied store");
}
void memory_publication_failures_and_reconciliation() {
    Environment environment;
    const auto config = small_cache_configuration();
    auto parsed = take(parse_xml(config, {1048576, 32, 16384, 1024, environment.account}));
    const auto store_config = std::get<LocalCacheConfiguration>(parsed.declaration().cache);
    auto memory = MemoryStore::make(store_config, environment.types.price);
    auto plan = environment.prepare_text(config, memory.store());
    auto run_once = [&](std::shared_ptr<const PreparedPlan> prepared,
                        const ql::ComponentTypes &types) {
        auto seen = std::make_shared<Seen>();
        auto run = take(instantiate(prepared, services(*prepared, types, seen)));
        return take(execute(run));
    };
    memory.fail_publication(true);
    const auto failed = run_once(plan.plan, environment.types);
    require(failed.numerical == NumericalOutcome::Completed &&
                failed.acceptance == AcceptanceStatus::Rejected &&
                failed.cache == CachePublicationStatus::NotPublished && memory.entries() == 0,
            "failed cache publication was accepted");
    memory.fail_publication(false);
    memory.lose_acknowledgement(true, false);
    const auto reconciled = run_once(plan.plan, environment.types);
    require(reconciled.acceptance == AcceptanceStatus::Accepted &&
                reconciled.cache_record_digest != Sha256{} && memory.entries() == 4,
            "confirmed authoritative memory publication did not reconcile");
    Environment uncertain_environment, recovered_environment;
    auto uncertain = MemoryStore::make(store_config, uncertain_environment.types.price);
    uncertain.lose_acknowledgement(true, true);
    auto uncertain_plan = uncertain_environment.prepare_text(config, uncertain.store());
    const auto unconfirmed = run_once(uncertain_plan.plan, uncertain_environment.types);
    require(unconfirmed.acceptance == AcceptanceStatus::Unconfirmed &&
                unconfirmed.cache == CachePublicationStatus::Unconfirmed &&
                !unconfirmed.cache_commit_id.empty() && unconfirmed.cache_published_entries == 0,
            "unresolved marker visibility was reported as success or proven rejection");
    uncertain.lose_acknowledgement(false, false);
    auto recovered_plan = recovered_environment.prepare_text(config, uncertain.store());
    const auto recovered = run_once(recovered_plan.plan, recovered_environment.types);
    require(recovered.acceptance == AcceptanceStatus::Accepted &&
                recovered.producers[2].cached_deliveries == 3,
            "later validated origin could not recover an unacknowledged publication");
}
void memory_capacity_bypass_and_mixed_replay() {
    Environment cold_environment, warm_environment, tiny_environment;
    const auto config =
        replace(small_cache_configuration(), "max_entries=\"64\"", "max_entries=\"1\"");
    auto parsed = take(parse_xml(config, {1048576, 32, 16384, 1024, cold_environment.account}));
    auto memory = MemoryStore::make(std::get<LocalCacheConfiguration>(parsed.declaration().cache),
                                    cold_environment.types.price);
    auto cold_plan = cold_environment.prepare_text(config, memory.store());
    auto cold_seen = std::make_shared<Seen>();
    auto cold = take(
        instantiate(cold_plan.plan, services(*cold_plan.plan, cold_environment.types, cold_seen)));
    auto first = take(execute(cold));
    require(first.acceptance == AcceptanceStatus::Accepted && first.cache_published_entries == 1 &&
                first.cache_bypassed_entries == 3 && memory.entries() == 1,
            "entry capacity was silently enlarged or treated as an I/O failure");
    auto warm_plan = warm_environment.prepare_text(config, memory.store());
    auto warm_seen = std::make_shared<Seen>();
    auto warm = take(
        instantiate(warm_plan.plan, services(*warm_plan.plan, warm_environment.types, warm_seen)));
    auto second = take(execute(warm));
    require(second.acceptance == AcceptanceStatus::Accepted &&
                second.cache == CachePublicationStatus::Bypassed &&
                second.cache_published_entries == 0 && memory.publications() == 1 &&
                second.producers[1].cached_deliveries == 3 && second.producers[2].advances == 3 &&
                warm_seen->values == cold_seen->values,
            "full-store bypass wrote a hidden marker or mixed replay changed outputs");
    auto tiny_config = replace(small_cache_configuration(), "max_entry_bytes=\"1048576\"",
                               "max_entry_bytes=\"512\"");
    auto tiny_parsed =
        take(parse_xml(tiny_config, {1048576, 32, 16384, 1024, tiny_environment.account}));
    auto tiny =
        MemoryStore::make(std::get<LocalCacheConfiguration>(tiny_parsed.declaration().cache),
                          tiny_environment.types.price);
    auto tiny_plan = tiny_environment.prepare_text(tiny_config, tiny.store());
    auto tiny_seen = std::make_shared<Seen>();
    auto tiny_run = take(
        instantiate(tiny_plan.plan, services(*tiny_plan.plan, tiny_environment.types, tiny_seen)));
    const auto bypass = take(execute(tiny_run));
    require(
        bypass.acceptance == AcceptanceStatus::Accepted &&
            bypass.cache == CachePublicationStatus::Bypassed && tiny.entries() == 0 &&
            tiny.publications() == 0,
        "oversized entry was encoded/published instead of selecting configured pre-write bypass");
}
void full_fixture_cache_capacity_and_equivalence() {
    Environment first, second;
    auto config =
        replace(example, "managed_buffer_bytes=\"8388608\"", "managed_buffer_bytes=\"33554432\"");
    config = replace(std::move(config), "retention_log_max_events=\"4096\"",
                     "retention_log_max_events=\"8192\"");
    auto parsed = take(parse_xml(config, {1048576, 32, 16384, 1024, first.account}));
    auto memory = MemoryStore::make(std::get<LocalCacheConfiguration>(parsed.declaration().cache),
                                    first.types.price);
    auto cold_plan = first.prepare_text(config, memory.store());
    auto cold_seen = std::make_shared<Seen>();
    auto cold = take(instantiate(cold_plan.plan,
                                 services(*cold_plan.plan, first.types, cold_seen, false, true)));
    const auto cold_report = take(execute(cold));
    if (cold_report.cause) {
        throw std::runtime_error(std::string{cold_report.cause->text()});
    }
    require(
        cold_report.acceptance == AcceptanceStatus::Accepted &&
            cold_report.cache_published_entries == 4 && cold_report.cache_bypassed_entries == 0,
        "full fixture capacity: published=" + std::to_string(cold_report.cache_published_entries) +
            " bypassed=" + std::to_string(cold_report.cache_bypassed_entries));
    const auto sizes = memory.entry_sizes();
    require(
        sizes.size() == 4 &&
            std::all_of(sizes.begin(), sizes.end(), [](auto bytes) { return bytes <= 1048576; }) &&
            cold_seen->metadata[2][0].parents.size() == 50,
        "full fixture entry measurement or complete SMA50 parent window is wrong");
    auto warm_plan = second.prepare_text(config, memory.store());
    auto warm_seen = std::make_shared<Seen>();
    auto warm = take(instantiate(warm_plan.plan,
                                 services(*warm_plan.plan, second.types, warm_seen, false, true)));
    const auto warm_report = take(execute(warm));
    if (warm_report.cause) {
        throw std::runtime_error(std::string{warm_report.cause->text()});
    }
    require(warm_report.acceptance == AcceptanceStatus::Accepted &&
                warm_seen->values == cold_seen->values &&
                warm_seen->metadata == cold_seen->metadata,
            "full cached/fresh fixture changed values or complete parent provenance");
    require(warm_report.producers[1].cached_deliveries == 120 &&
                warm_report.producers[2].cached_deliveries == 120 &&
                warm_report.producers[5].cached_deliveries == 120 &&
                warm_report.producers[4].cached_deliveries == 120 &&
                warm_report.producers[4].advances == 0 && warm_seen->values[3].size() == 71,
            "full fixture cached delivery counters differ");
    std::cout << "CACHE_FIXTURE source=120 observations=71,71,71,71 stored=4 cached_fast_advances="
              << warm_report.producers[2].advances
              << " cached_slow_advances=" << warm_report.producers[4].advances
              << " entry_bytes=" << sizes[0] << ',' << sizes[1] << ',' << sizes[2] << ','
              << sizes[3] << '\n';
}
void cached_lag_preserves_displaced_parent() {
    Environment first, second;
    auto config = small_cache_configuration(4);
    config.insert(config.find("    <node id=\"spread\""),
                  "<node id=\"lagged_slow\" operation=\"series.lag_price\" semantic_version=\"1\" "
                  "implementation_id=\"builtin-v1\" alignment=\"same_step\" "
                  "initialization=\"empty_at_step_0\">"
                  "<param name=\"lag_steps\" value=\"1\"/><param name=\"count_basis\" "
                  "value=\"complete_bar_updates\"/>"
                  "<input port=\"price\" producer=\"slow\" output=\"mean\" prior_outputs=\"0\" "
                  "missing_intervals=\"strict\"/></node>");
    config = replace(std::move(config), "port=\"right\" producer=\"slow\" output=\"mean\"",
                     "port=\"right\" producer=\"lagged_slow\" output=\"lagged\"");
    config = replace(std::move(config), "id=\"slow_trial\" producer=\"slow\" output=\"mean\"",
                     "id=\"slow_trial\" producer=\"lagged_slow\" output=\"lagged\"");
    for (auto position = config.find("begin_step=\"2\""); position != std::string::npos;
         position = config.find("begin_step=\"2\"")) {
        config.replace(position, 14, "begin_step=\"3\"");
    }
    auto parsed = take(parse_xml(config, {1048576, 32, 16384, 1024, first.account}));
    auto memory = MemoryStore::make(std::get<LocalCacheConfiguration>(parsed.declaration().cache),
                                    first.types.price);
    auto a = first.prepare_text(config, memory.store());
    auto seen_a = std::make_shared<Seen>();
    auto cold = take(instantiate(a.plan, services(*a.plan, first.types, seen_a, false, true)));
    require(take(execute(cold)).acceptance == AcceptanceStatus::Accepted, "cold lag stream failed");
    auto b = second.prepare_text(config, memory.store());
    auto seen_b = std::make_shared<Seen>();
    auto warm = take(instantiate(b.plan, services(*b.plan, second.types, seen_b, false, true)));
    const auto report = take(execute(warm));
    require(report.acceptance == AcceptanceStatus::Accepted && report.producers[5].advances == 0 &&
                report.producers[5].cached_deliveries == 4 && seen_b->values == seen_a->values &&
                seen_b->metadata == seen_a->metadata &&
                seen_b->values[3] == std::vector<std::int64_t>{15000000},
            "cached lag lost startup or displaced-parent semantics");
    const auto &lag = seen_b->metadata[2][0];
    require(lag.publication.key.sequence == 3 && lag.publication.available_at_ns == 240000000000 &&
                lag.parents[0].publication.key.sequence == 2 &&
                lag.parents[0].publication.available_at_ns == 180000000000,
            "cached lag backdated its current activation");
}
void cached_host_failure_preserves_prior_origin() {
    Environment first, second;
    const auto config = small_cache_configuration();
    auto parsed = take(parse_xml(config, {1048576, 32, 16384, 1024, first.account}));
    auto memory = MemoryStore::make(std::get<LocalCacheConfiguration>(parsed.declaration().cache),
                                    first.types.price);
    auto initial = first.prepare_text(config, memory.store());
    auto seen = std::make_shared<Seen>();
    auto cold = take(instantiate(initial.plan, services(*initial.plan, first.types, seen)));
    require(take(execute(cold)).acceptance == AcceptanceStatus::Accepted,
            "host failure fixture could not seed store");
    auto hosted_config = replace(config, "driver=\"standalone\"", "driver=\"hosted\"");
    auto prepared = second.prepare_text(hosted_config, memory.store());
    auto hosted_seen = std::make_shared<Seen>();
    auto hosted =
        take(instantiate(prepared.plan, services(*prepared.plan, second.types, hosted_seen)));
    for (;;) {
        const auto receipt = take(advance_one(hosted));
        if (receipt.status == StepStatus::Exhausted) {
            break;
        }
        require(receipt.status == StepStatus::Processed, "cached hosted update failed");
    }
    const auto error =
        diagnostic(FailureCode::CallbackException, "host failed after numerical exhaustion");
    success(fail_host(hosted, error));
    const auto report = take(finish(
        hosted, {IntegrationOutcome::Failed, error, "test.hosted", 1, Sha256{std::byte{10}}}));
    require(report.numerical == NumericalOutcome::Completed &&
                report.acceptance == AcceptanceStatus::Rejected &&
                report.cache == CachePublicationStatus::NotPublished &&
                report.producers[2].cached_deliveries == 3 && memory.publications() == 1 &&
                memory.entries() == 4,
            "host failure accepted the new run or invalidated the prior accepted origin");
}
void artifact_cursor_checks_activation_before_delivery() {
    Environment first, second;
    const auto config = small_cache_configuration();
    auto parsed = take(parse_xml(config, {1048576, 32, 16384, 1024, first.account}));
    auto memory = MemoryStore::make(std::get<LocalCacheConfiguration>(parsed.declaration().cache),
                                    first.types.price);
    auto cold_plan = first.prepare_text(config, memory.store());
    auto seen = std::make_shared<Seen>();
    auto cold = take(instantiate(cold_plan.plan, services(*cold_plan.plan, first.types, seen)));
    require(take(execute(cold)).acceptance == AcceptanceStatus::Accepted,
            "cursor fixture could not seed store");
    auto prepared = second.prepare_text(config, memory.store());
    auto resources = take(ResourceAccount::create(prepared.plan->runtime_limits()));
    auto input = take(ResultSlot::create(resources, {prepared.plan->outputs(1)[0]}, 0,
                                         prepared.plan->identity(1), {}));
    auto history = std::make_shared<PublicationHistory>(
        take(PublicationHistory::create(resources, *input, 1)));
    auto cursor = take(prepared.plan->artifact(2)->cursor(prepared.plan, 2, resources,
                                                          {{input, 0, 0}}, {history}));
    const UpdateContext current{
        {"bar-clock-v1", 0}, Interval{0, 60000000000}, 60000000000, 60000000000};
    auto wrong = current;
    wrong.activation_available_at_ns -= 1;
    rejects(cursor.next(wrong), FailureCode::UpdateProtocol);
    wrong = current;
    wrong.decision_time_ns -= 1;
    rejects(cursor.next(wrong), FailureCode::FutureRead);
    wrong = current;
    wrong.key.sequence = 1;
    rejects(cursor.next(wrong), FailureCode::UpdateProtocol);
    auto pending = take(PendingOutcome::create(resources, current, 1));
    PublicationAllocator allocator{resources, 0};
    auto price = take(allocator.freeze(Price::from_raw(100000000000), second.types.price));
    success(pending.set_output(
        0, take(ErasedValue::from(std::move(price), second.types.price.policy.contract))));
    input->commit(take(validate_pending(std::move(pending), *input, current)));
    success(history->capture(*input));
    auto output = take(ResultSlot::create(resources, {prepared.plan->outputs(2)[0]}, 0,
                                          prepared.plan->identity(2), {}));
    output->commit(take(validate_pending(take(cursor.next(current)), *output, current)));
    require(output->current()->kind == OutcomeKind::Warming &&
                output->current()->parents.entries().size() == 1,
            "invalid API calls consumed the first cached warming outcome");
}
void required_snapshot_precedes_acceptance_marker() {
    Environment environment;
    environment.required_evidence = {{"domain", "test_domain_v1", "/tmp/test-domain-evidence",
                                      "create_new", 1024, 128, "on_close", "stop"},
                                     {"snapshot", "run_snapshot_v1", "/tmp/test-snapshot-evidence",
                                      "create_new", 65536, 128, "on_close", "stop"}};
    environment.evidence_resources = {0, 0, 4096, 4096, 0, 4096, 4096};
    const auto config = small_cache_configuration();
    auto parsed = take(parse_xml(config, {1048576, 32, 16384, 1024, environment.account}));
    auto memory = MemoryStore::make(std::get<LocalCacheConfiguration>(parsed.declaration().cache),
                                    environment.types.price);
    auto prepared = environment.prepare_text(config, memory.store());
    auto seen = std::make_shared<Seen>();
    auto callbacks = services(*prepared.plan, environment.types, seen);
    bool domain_closed = false, snapshot_closed = false;
    std::uint64_t outcomes = 0;
    callbacks.required.push_back(
        {{},
         environment.required_evidence[0],
         [] { return ok(); },
         {},
         {},
         [&](const RunReport &report) -> Result<EvidenceReference> {
             require(seen->finalized && report.acceptance == AcceptanceStatus::Pending,
                     "domain evidence finalized outside precommit");
             domain_closed = true;
             return EvidenceReference{"domain", "/tmp/test-domain-evidence", 1,
                                      Sha256{std::byte{1}}, true};
         }});
    callbacks.required.push_back(
        {{},
         environment.required_evidence[1],
         [] { return ok(); },
         [&](std::uint32_t, const OutcomeFrame &) -> Result<void> {
             ++outcomes;
             return ok();
         },
         {},
         [&](const RunReport &report) -> Result<EvidenceReference> {
             require(domain_closed && report.evidence.size() == 1 &&
                         report.cache == CachePublicationStatus::Staged &&
                         !report.cache_commit_id.empty() && report.cache_staged_entries == 4 &&
                         memory.publications() == 0,
                     "snapshot lacks finalized inventory/capacity or follows accepted publication");
             snapshot_closed = true;
             return EvidenceReference{"snapshot", "/tmp/test-snapshot-evidence", 2,
                                      Sha256{std::byte{2}}, true};
         }});
    auto run = take(instantiate(prepared.plan, std::move(callbacks)));
    const auto report = take(execute(run));
    require(report.acceptance == AcceptanceStatus::Accepted && snapshot_closed && outcomes == 15 &&
                report.evidence.size() == 2 && memory.publications() == 1,
            "required evidence set did not complete before the single marker");
}
void file_backed_required_evidence() {
    auto pattern = (scratch_root / "lexongine-file-run.XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    require(::mkdtemp(writable.data()) != nullptr, "could not create explicit file-run directory");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{writable.data()};
    const auto path = (cleanup.path / "retention.log").string();
    Environment environment;
    auto prepared = environment.prepare_text(replace(disabled(example, Driver::Standalone),
                                                     "/tmp/lexongine-fixture-retention.log", path));
    auto seen = std::make_shared<Seen>();
    auto callbacks = services(*prepared.plan, environment.types, seen);
    std::vector<std::string> producers;
    for (const auto &node : prepared.plan->nodes()) {
        producers.push_back(node.spec.id);
    }
    auto file = take(FileEvidence::create(
        prepared.plan->evidence_specification(), prepared.plan->configuration().run.id,
        prepared.plan->configuration().run.clock, std::move(producers), environment.account,
        {65536, 8192, 8388608}));
    callbacks.evidence = file.callbacks();
    auto run = take(instantiate(prepared.plan, std::move(callbacks)));
    auto report = take(execute(run));
    if (report.cause) {
        throw std::runtime_error(std::string{report.cause->text()});
    }
    require(report.acceptance == AcceptanceStatus::Accepted && report.evidence.size() == 1 &&
                seen->values[3].size() == 71,
            "file-backed evidence did not gate the complete fixture");
    std::ifstream input{path};
    std::ostringstream output;
    output << input.rdbuf();
    const auto text = output.str();
    require(report.evidence[0].digest ==
                    take(digest(std::as_bytes(std::span{text.data(), text.size()}))) &&
                report.evidence[0].byte_count == text.size(),
            "reported finalized evidence differs from actual file bytes");
    require(text.find("producer=\"fast_A\"") != std::string::npos &&
                text.find("publication=\"bar-clock-v1:49\"") != std::string::npos,
            "file log omitted actual producer/activation ownership");
}
void every_fixture_driver_split_preserves_state() {
    Environment environment;
    auto prepared = environment.prepare_text(disabled(example, Driver::Hosted));
    std::array<std::vector<std::int64_t>, 4> expected;
    std::array<std::vector<ObservedPublication>, 4> parents;
    for (std::uint64_t split = 0; split <= 120; ++split) {
        auto seen = std::make_shared<Seen>();
        auto run = take(instantiate(
            prepared.plan, services(*prepared.plan, environment.types, seen, false, true)));
        auto consume = [&](std::uint64_t begin, std::uint64_t end) {
            for (auto i = begin; i < end; ++i) {
                auto receipt = take(advance_one(run));
                require(receipt.status == StepStatus::Processed && receipt.key->sequence == i,
                        "host batch split compressed or repeated a source update");
            }
        };
        consume(0, split);
        consume(split, 120);
        require(take(advance_one(run)).status == StepStatus::Exhausted,
                "split run failed exact closure");
        auto report = take(finish(
            run, {IntegrationOutcome::Completed, {}, "test.hosted", 1, Sha256{std::byte{10}}}));
        require(report.acceptance == AcceptanceStatus::Accepted && report.complete_updates == 120 &&
                    report.producers[2].advances == 120 && report.producers[4].advances == 120,
                "split reset/replayed state");
        if (split == 0) {
            expected = seen->values;
            parents = seen->metadata;
        } else {
            require(seen->values == expected && seen->metadata == parents,
                    "driver split changed values or complete lineage");
        }
    }
}
} // namespace
int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "explicit fixture configuration and scratch root required\n";
        return 1;
    }
    scratch_root = argv[2];
    std::ifstream input{argv[1]};
    std::ostringstream content;
    content << input.rdbuf();
    example = content.str();
    const Case cases[]{
        {"configured six/five fixture execution", configured_fixture_executes_once},
        {"source-known bounds reject only dependent requests",
         source_bound_rejection_is_attributed},
        {"hosted equivalence and evidence close failure",
         hosted_matches_standalone_and_close_failure_rejects},
        {"shared/unshared equivalence and integrated lag", sharing_and_lag_match_reference},
        {"retention quota stops execution", retention_quota_is_fail_stop},
        {"observer failure preserves logical prefix", observer_failure_preserves_logical_prefix},
        {"early completion and host failure", early_completion_and_host_failure_reject},
        {"history and global resource admission", history_and_global_resource_admission},
        {"reentrant driver calls", reentrant_driver_calls_are_rejected},
        {"source clock for independent computation", source_clock_survives_independent_computation},
        {"file-backed required evidence", file_backed_required_evidence},
        {"zero-input identity includes driver source", zero_input_identity_includes_driver_source},
        {"effective prepared parameters enter identity",
         effective_prepared_parameters_enter_identity},
        {"failed required effect cannot disappear", failed_required_effect_cannot_disappear},
        {"receipt workspace admitted before execution",
         receipt_workspace_is_admitted_before_execution},
        {"provenance geometry admitted before execution",
         provenance_geometry_is_admitted_before_execution},
        {"complete artifact identity and collision comparison",
         complete_artifact_identity_and_collision_comparison}
#if defined(LEXONGINE_HOST_SOURCE_BUILD)
        ,
        {"actual builtin build binding", actual_builtin_build_binding}
#endif
        ,
        {"in-memory exact-scope full outcome replay", in_memory_full_scope_replay},
        {"memory-store evidence and corruption gates", memory_store_evidence_and_corruption_gates},
        {"cache preflight, pinned bytes and disabled access",
         memory_store_preflight_pinning_and_disabled_access},
        {"cache publication failure and reconciliation",
         memory_publication_failures_and_reconciliation},
        {"capacity bypass and mixed cached/fresh execution",
         memory_capacity_bypass_and_mixed_replay},
        {"full fixture cache capacity and equivalence",
         full_fixture_cache_capacity_and_equivalence},
        {"cached lag retains displaced-parent timing", cached_lag_preserves_displaced_parent},
        {"cached host failure preserves prior origin", cached_host_failure_preserves_prior_origin},
        {"artifact cursor activation preconditions",
         artifact_cursor_checks_activation_before_delivery},
        {"required snapshot before acceptance marker",
         required_snapshot_precedes_acceptance_marker},
        {"all 121 fixture driver split positions", every_fixture_driver_split_preserves_state}};
    return lexongine::test::run(cases);
}
