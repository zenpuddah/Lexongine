#include "xml_frontend.hpp"
#include "../../tests/lexongine_test_support.hpp"

#include <fstream>
#include <sstream>

namespace {
using namespace lexongine;
using namespace lexongine::test;
std::string example;
int factory_calls = 0;
Contract protocol(std::string name) {
    return {std::move(name), 1, {1, {}}};
}
TypeContract price_type() {
    return {"qaedalus.price",
            1,
            {1, {{"scale", std::uint64_t{9}, Origin::VersionedFact, "canonical"}}}};
}
TypeContract bar_type() {
    return {"qaedalus.bar", 1, {1, {}}};
}
Registry registry() {
    auto value = take(Registry::create());
    auto define = [&](std::string space, std::string name, std::string implementation,
                      std::string initialization, std::vector<ParameterField> parameters,
                      std::vector<InputContract> inputs, OutputContract output) {
        OperationContract contract{
            {std::move(space), std::move(name), 1, std::move(implementation)},
            {{}, {}, {}, "frontend-descriptor-test"},
            {1, std::move(parameters)},
            std::move(inputs),
            {std::move(output)},
            {SharingClass::StatefulDeclared,
             protocol(std::move(initialization)),
             protocol("same_step"),
             protocol("none"),
             true,
             true,
             true,
             {"descriptor-test"}},
            {"interval_v1"},
            {Alignment::SameStep},
            {MissingIntervalPolicy::Strict, MissingIntervalPolicy::Skip},
            {"count"},
            "schema fixture"};
        success(value.register_definition(
            std::move(contract), {},
            [](const ParamSet &, const PreparationFacts &) -> Result<PreparedKernel> {
                ++factory_calls;
                return diagnostic(FailureCode::UnsupportedContract,
                                  "frontend descriptor has no executable kernel");
            }));
    };
    auto output = [&](std::string name, TypeContract type) {
        return OutputContract{std::move(name),   std::move(type),        StorageKind::ScalarCopy,
                              protocol("codec"), protocol("validation"), true};
    };
    auto input = [&](std::string name, TypeContract type) {
        return InputContract{std::move(name), std::move(type), Alignment::SameStep,
                             MissingIntervalPolicy::Strict, AbsencePolicy::RequireValue};
    };
    auto text = [](std::string name, std::string allowed) {
        ParameterConstraints constraints;
        constraints.allowed_text.push_back(std::move(allowed));
        return ParameterField{std::move(name), ParamKind::Text, true, std::move(constraints)};
    };
    ParameterConstraints positive;
    positive.unsigned_min = 1;
    ParameterConstraints nonnegative;
    nonnegative.signed_min = 0;
    define("qaedalus", "fixture_bars", "fixture-generator-v1", "source_cursor", {}, {},
           output("bar", bar_type()));
    define("qaedalus", "bar_close", "builtin-v1", "stateless", {}, {input("bar", bar_type())},
           output("price", price_type()));
    define("statistics", "sma_price", "builtin-v1", "empty_at_step_0",
           {{"window", ParamKind::Unsigned64, true, positive},
            {"max_abs_input_raw", ParamKind::Signed64, true, nonnegative},
            text("sum_width", "signed64"),
            text("rounding", "nearest_ties_even"),
            text("overflow", "fail")},
           {input("price", price_type())}, output("mean", price_type()));
    define("qaedalus", "price_difference", "builtin-v1", "stateless", {text("overflow", "fail")},
           {input("left", price_type()), input("right", price_type())},
           output("difference", price_type()));
    success(value.freeze());
    return value;
}
ParseLimits limits() {
    return {1048576, 32, 16384, 1024,
            take(ResourceAccount::create(
                {{1048576, 1048576, 1048576, 1048576, 1048576, 8388608, 1048576},
                 128,
                 128,
                 2048,
                 128}))};
}
std::string replace(std::string text, std::string_view old, std::string_view replacement) {
    const auto position = text.find(old);
    require(position != std::string::npos, "test mutation did not match the fixture");
    text.replace(position, old.size(), replacement);
    return text;
}
void owned_example() {
    auto text = example;
    auto parsed = take(parse_xml(text, limits()));
    text.assign(text.size(), 'x');
    const auto &value = parsed.declaration();
    require(value.run.driver == Driver::Standalone && value.run.retention_warning_steps == 128,
            "run settings changed");
    require(value.sources.size() == 1 && value.nodes.size() == 5 && value.observations.size() == 4,
            "fixture composition counts changed");
    require(value.nodes[1].parameters.values[0].text == "20", "raw parameter borrowed XML memory");
    require(value.nodes[1].location.line > 1, "source location lost");
    require(std::get<LocalCacheConfiguration>(value.cache).max_entry_bytes == 1048576,
            "explicit cache limits lost");
    require(value.run.id == "fixture-run-v1" && value.run.clock == "bar-clock-v1" &&
                value.run.execution == "synchronous" && value.run.sharing == "exact" &&
                value.run.failure == "stop" && value.run.trace == "steps",
            "run field projection mismatch");
    const auto &log = value.run.retention_log;
    require(log.path == "/tmp/lexongine-fixture-retention.log" &&
                log.format == "key_value_lines_v1" && log.open == "create_new" &&
                log.flush == "on_event" && log.failure == "stop" && log.overflow == "stop" &&
                log.max_bytes == 1048576 && log.max_events == 4096,
            "retention field projection mismatch");
    const auto &bounds = value.limits;
    require(bounds.xml_bytes == 1048576 && bounds.xml_depth == 16 && bounds.xml_elements == 512 &&
                bounds.string_bytes == 256 && bounds.nodes == 32 && bounds.edges == 64 &&
                bounds.requests == 8 && bounds.source_updates == 120 &&
                bounds.managed_buffer_bytes == 8388608 && bounds.trace_events == 4096 &&
                bounds.trace_message_bytes == 256,
            "limit field projection mismatch");
    const auto &cache = std::get<LocalCacheConfiguration>(value.cache);
    require(cache.mode == "automatic_read_write" && cache.backend == "local_files" &&
                cache.directory == "/tmp/lexongine-fixture-cache-v1" &&
                cache.matching == "exact_full_scope" && cache.trust == "same_build_local_writer" &&
                cache.integrity == "sha256" && cache.writer == "single_process_lock" &&
                cache.corruption == "fail_preparation" && cache.capacity == "bypass_new_entries" &&
                cache.cleanup == "explicit_only" && cache.max_entries == 64 &&
                cache.max_store_bytes == 8388608 && cache.max_decoded_bytes == 1048576,
            "cache policy projection mismatch");
    const auto &source = value.sources[0];
    require(source.id == "bars" && source.ordinal == 0 &&
                source.operation ==
                    OperationKey{"qaedalus", "fixture_bars", 1, "fixture-generator-v1"} &&
                source.source_version == "fixture-bars-v1" &&
                source.scope == SequenceScope{"bar-clock-v1", 0, 120} &&
                source.completeness == "complete_only" && source.empty == "reject" &&
                source.missing == "fail" &&
                source.availability == "interval_end_zero_lag_fixture" &&
                source.missing_intervals == MissingIntervalPolicy::Strict &&
                source.parameters.values.empty(),
            "source field projection mismatch");
    require(value.nodes[0].ordinal == 1 && value.nodes[0].initialization == "stateless" &&
                value.nodes[0].alignment == "same_step" &&
                value.nodes[0].inputs[0].producer == "bars" &&
                value.nodes[0].inputs[0].output_name == "bar" &&
                value.nodes[0].inputs[0].prior_outputs == 0,
            "node binding projection mismatch");
    require(value.nodes[4].inputs[0].input_name == "left" &&
                value.nodes[4].inputs[0].producer == "fast_A" &&
                value.nodes[4].inputs[1].input_name == "right" &&
                value.nodes[4].inputs[1].producer == "slow",
            "input order was normalized away");
    for (const auto &request : value.observations) {
        require(request.evaluation == SequenceScope{"bar-clock-v1", 49, 120} &&
                    request.prior_outputs == 0 &&
                    request.missing_intervals == MissingIntervalPolicy::Strict &&
                    request.absence == AbsencePolicy::RequireValue && request.label == request.id,
                "observation field projection mismatch");
    }
}
void missing_unknown_duplicates() {
    rejects(parse_xml(replace(example, "driver=\"standalone\"", ""), limits()),
            FailureCode::InvalidConfig);
    rejects(parse_xml(
                replace(example, "driver=\"standalone\"", "driver=\"standalone\" surprise=\"yes\""),
                limits()),
            FailureCode::InvalidConfig);
    rejects(parse_xml(replace(example, "driver=\"standalone\"",
                              "driver=\"standalone\" driver=\"hosted\""),
                      limits()),
            FailureCode::InvalidConfig);
    rejects(parse_xml(replace(example, "id=\"fast_B\"", "id=\"fast_A\""), limits()),
            FailureCode::InvalidConfig);
    rejects(parse_xml(replace(example, "name=\"rounding\"", "name=\"window\""), limits()),
            FailureCode::InvalidConfig);
    rejects(parse_xml(replace(example, "missing_intervals=\"strict\"", ""), limits()),
            FailureCode::InvalidConfig);
    rejects(parse_xml(replace(example, "max_entries=\"64\"", ""), limits()),
            FailureCode::InvalidConfig);
}
void strict_xml_and_numbers() {
    rejects(parse_xml(replace(example, "begin_step=\"0\"", "begin_step=\"0junk\""), limits()),
            FailureCode::InvalidConfig);
    rejects(parse_xml(replace(example, "nodes=\"32\"", "nodes=\"-1\""), limits()),
            FailureCode::InvalidConfig);
    rejects(parse_xml(replace(example, "nodes=\"32\"", "nodes=\"18446744073709551616\""), limits()),
            FailureCode::InvalidConfig);
    rejects(parse_xml(replace(example, "composition version=\"1\"", "composition version=\"2\""),
                      limits()),
            FailureCode::UnsupportedContract);
    rejects(parse_xml(replace(example, "<composition version=\"1\">",
                              "<!DOCTYPE composition><composition version=\"1\">"),
                      limits()),
            FailureCode::InvalidConfig);
    rejects(parse_xml(replace(example, "<plugins/>", "<?unknown test?><plugins/>"), limits()),
            FailureCode::InvalidConfig);
    rejects(parse_xml(replace(example, "<plugins/>", "<plugins xmlns=\"urn:test\"/>"), limits()),
            FailureCode::InvalidConfig);
    rejects(parse_xml(replace(example, "fixture-run-v1", "&undeclared;"), limits()),
            FailureCode::InvalidConfig);
    rejects(parse_xml(replace(example, "fixture-run-v1", "&#0;"), limits()),
            FailureCode::InvalidConfig);
}
void explicit_limits() {
    auto small = limits();
    small.xml_bytes = 10;
    rejects(parse_xml(example, small), FailureCode::InvalidConfig);
    rejects(parse_xml(replace(example, "xml_depth=\"16\"", "xml_depth=\"2\""), limits()),
            FailureCode::InvalidConfig);
    rejects(parse_xml(replace(example, "xml_elements=\"512\"", "xml_elements=\"4\""), limits()),
            FailureCode::InvalidConfig);
    rejects(parse_xml(replace(example, "nodes=\"32\"", "nodes=\"2\""), limits()),
            FailureCode::InvalidConfig);
    rejects(parse_xml(replace(example, "string_bytes=\"256\"", "string_bytes=\"4\""), limits()),
            FailureCode::InvalidConfig);
}
void schema_resolution() {
    auto definitions = registry();
    auto composition = take(resolve_composition(take(parse_xml(example, limits())), definitions));
    const auto &values = composition.node_parameters()[1].parameters.values;
    const auto window = std::find_if(values.begin(), values.end(),
                                     [](const auto &value) { return value.name == "window"; });
    require(window != values.end() && std::get<std::uint64_t>(window->value) == 20,
            "registry schema did not produce the typed window");
    require(factory_calls == 0, "schema resolution invoked an executable factory");
    auto invalid = take(parse_xml(replace(example, "value=\"20\"", "value=\"20junk\""), limits()));
    rejects(resolve_composition(invalid, definitions), FailureCode::InvalidConfig);
    invalid =
        take(parse_xml(replace(example, "<param name=\"window\" value=\"20\"/>", ""), limits()));
    rejects(resolve_composition(invalid, definitions), FailureCode::InvalidConfig);
    invalid = take(parse_xml(replace(example, "<input port=\"bar\"",
                                     "<param name=\"extra\" value=\"1\"/><input port=\"bar\""),
                             limits()));
    rejects(resolve_composition(invalid, definitions), FailureCode::InvalidConfig);
    invalid = take(
        parse_xml(replace(example, "semantic_version=\"1\"", "semantic_version=\"2\""), limits()));
    rejects(resolve_composition(invalid, definitions), FailureCode::UnknownDefinition);
    invalid = take(parse_xml(replace(example, "producer=\"bars\" output=\"bar\"",
                                     "producer=\"bars\" output=\"unknown\""),
                             limits()));
    rejects(resolve_composition(invalid, definitions), FailureCode::InvalidConfig);
    require(factory_calls == 0, "rejected configuration invoked an executable factory");
    require(composition.source_parameters()[0].parameters.values.empty() &&
                composition.node_parameters()[0].parameters.values.empty(),
            "implicit source/close parameters added");
    require(composition.node_parameters()[1].parameters ==
                composition.node_parameters()[2].parameters,
            "equal SMA parameters differ by source location");
    for (const auto &parameter : values) {
        require(parameter.origin == Origin::ExplicitConfig &&
                    parameter.provenance.starts_with("<memory>:"),
                "parameter origin not exported");
    }
}
void declaration_and_entity_edges() {
    rejects(parse_xml(replace(example, "version=\"1.0\" encoding=\"UTF-8\"",
                              "version=\"1.1\" encoding=\"UTF-8\""),
                      limits()),
            FailureCode::UnsupportedContract);
    rejects(parse_xml(replace(example, "encoding=\"UTF-8\"", "encoding=\"ISO-8859-1\""), limits()),
            FailureCode::UnsupportedContract);
    rejects(
        parse_xml(replace(example, "encoding=\"UTF-8\"", "encoding=\"UTF-8\" encoding=\"UTF-8\""),
                  limits()),
        FailureCode::InvalidConfig);
    const auto escaped =
        take(parse_xml(replace(example, "fixture-run-v1", "fixture&amp;&quot;run"), limits()));
    require(escaped.declaration().run.id == "fixture&\"run", "ordinary XML escaping changed data");
    const auto hosted =
        take(parse_xml(replace(example, "driver=\"standalone\"", "driver=\"hosted\""), limits()));
    require(hosted.declaration().run.driver == Driver::Hosted, "explicit hosted driver lost");
    require(take(parse_xml(
                     replace(example, "trace_message_bytes=\"256\"", "trace_message_bytes=\"512\""),
                     limits()))
                    .declaration()
                    .limits.trace_message_bytes == 512,
            "trace ceiling was silently narrowed to the fixed diagnostic envelope");
    rejects(parse_xml(replace(example, "fixture-run-v1", "\xef\xbf\xbe"), limits()),
            FailureCode::InvalidConfig);
    rejects(parse_xml(replace(example, "fixture-run-v1", "&#x110000;"), limits()),
            FailureCode::InvalidConfig);
}
void policy_acceptance() {
    auto definitions = registry();
    auto changed = take(parse_xml(
        replace(example, "missing_intervals=\"strict\"", "missing_intervals=\"skip\""), limits()));
    rejects(resolve_composition(changed, definitions), FailureCode::UnsupportedContract);
    auto observer_mismatch = example;
    const auto last = observer_mismatch.rfind("missing_intervals=\"strict\"");
    require(last != std::string::npos, "observation mutation failed");
    observer_mismatch.replace(last, std::string_view{"missing_intervals=\"strict\""}.size(),
                              "missing_intervals=\"skip\"");
    rejects(resolve_composition(take(parse_xml(observer_mismatch, limits())), definitions),
            FailureCode::UnsupportedContract);
}
void disabled_cache_and_undeclared_nodes() {
    auto disabled = example;
    const auto begin = disabled.find("<cache ");
    const auto end = disabled.find("/>", begin);
    require(begin != std::string::npos && end != std::string::npos, "cache mutation did not match");
    disabled.replace(begin, end + 2 - begin, "<cache mode=\"disabled\"/>");
    auto parsed = take(parse_xml(disabled, limits()));
    require(std::holds_alternative<DisabledCache>(parsed.declaration().cache),
            "disabled cache acquired hidden configuration");
    rejects(parse_xml(replace(disabled, "<cache mode=\"disabled\"/>",
                              "<cache mode=\"disabled\" directory=\"/tmp/unused\"/>"),
                      limits()),
            FailureCode::InvalidConfig);
    rejects(parse_xml(replace(example, "producer=\"bars\"", "producer=\"undeclared\""), limits()),
            FailureCode::InvalidConfig);
    rejects(parse_xml(replace(example, "<plugins/>", "<unknown/><plugins/>"), limits()),
            FailureCode::InvalidConfig);
    rejects(
        parse_xml(
            replace(example, "<plugins/>",
                    "<plugins><plugin path=\"relative.dylib\" build_contract_id=\"x\"/></plugins>"),
            limits()),
        FailureCode::InvalidConfig);
}
} // namespace
int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "explicit example path required\n";
        return 1;
    }
    std::ifstream file{argv[1]};
    std::ostringstream buffer;
    buffer << file.rdbuf();
    if (!file || buffer.str().empty()) {
        std::cerr << "example unavailable\n";
        return 1;
    }
    example = buffer.str();
    const Case cases[]{
        {"complete owned XML example", owned_example},
        {"missing unknown duplicate fields", missing_unknown_duplicates},
        {"strict XML and integer grammar", strict_xml_and_numbers},
        {"explicit parsing limits", explicit_limits},
        {"registered schema resolution", schema_resolution},
        {"declaration and entity edges", declaration_and_entity_edges},
        {"explicit source and observation policy acceptance", policy_acceptance},
        {"disabled cache and declared references", disabled_cache_and_undeclared_nodes}};
    return lexongine::test::run(cases);
}
