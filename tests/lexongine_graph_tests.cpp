#include "lexongine_test_support.hpp"
#include "lexongine/graph.hpp"

namespace {
using namespace lexongine;
using namespace lexongine::test;
Contract protocol(std::string name) { return {std::move(name), 1, {1, {}}}; }
TypeContract integer() { return {"test.integer", 1, {1, {{"units", std::string{"count"}, Origin::VersionedFact, "test"}}}}; }
std::shared_ptr<ResourceAccount> account() {
    return take(ResourceAccount::create({{1048576, 1048576, 1048576, 1048576, 1048576, 8388608, 1048576}, 256, 64, 2048, 128}));
}
GraphLimits limits(bool share = true) { return {64, 256, 1048576, 256, share}; }
std::uint64_t collision(std::span<const std::byte>) { return 7; }
GraphNode node(std::string id, std::uint32_t ordinal, std::string name, std::vector<std::string> parents,
               SharingClass sharing = SharingClass::StatelessDeterministic) {
    OperationContract definition{{"test", name, 1, "impl-v1"}, {{}, {}, {}, "graph-test-build"}, {1, {}}, {},
        {{"out", integer(), StorageKind::ScalarCopy, protocol("integer-codec"), protocol("integer-valid"), true}},
        {sharing, protocol("empty"), protocol("same_step"), protocol(sharing == SharingClass::Unique ? "effect" : "none"),
         sharing != SharingClass::Unique, true, true, {"graph-fixture"}},
        {"point_v1"}, {Alignment::SameStep}, {MissingIntervalPolicy::Strict, MissingIntervalPolicy::Skip}, {"count"}, "description"};
    NodeSpec spec{id, id, ordinal, definition.key, {1, {}}, {}, {protocol("empty"), {"clock", 0, 0}, {1, {}}},
                  definition.sharing.update, protocol("max_inputs_activation"), definition.sharing.effect};
    for (std::size_t index = 0; index < parents.size(); ++index) {
        const auto port = "in" + std::to_string(index);
        definition.inputs.push_back({port, integer(), Alignment::SameStep, MissingIntervalPolicy::Strict, AbsencePolicy::RequireValue});
        spec.inputs.push_back({port, parents[index], "out", 0, Alignment::SameStep, MissingIntervalPolicy::Strict});
    }
    auto resolved = take(make_definition(std::move(definition), {}, {})); // Descriptors only; no numerical factory runs.
    IdentityFacts facts{{"clock", 0, 120}, CountHistory{0}, {}, protocol("signed_integer"),
                        MissingIntervalPolicy::Strict, protocol("bundle-v1"), protocol("bundle-check-v1")};
    if (parents.empty()) {
        facts.sources.push_back({"snapshot", "v1", Sha256{std::byte{1}}, resolved->contract().key,
            resolved->contract().implementation, integer(), protocol("query-selection"), Sha256{std::byte{2}},
            {"clock", 0, 120}, {{"clock", 0, 120}, {{"clock", 0}, {"clock", 1}}, {}, protocol("test-timeline")},
            protocol("source-order"), protocol("explicit-availability"), protocol("source-validation")});
        auto& keys = facts.sources.back().coverage.complete_keys;
        keys.clear();
        for (std::uint64_t key = 0; key < 120; ++key) { keys.push_back({"clock", key}); }
    }
    return {std::move(spec), std::move(resolved), std::move(facts)};
}
void exact_sharing() {
    std::vector<GraphNode> nodes;
    nodes.push_back(node("source", 0, "source", {}));
    nodes.push_back(node("close", 1, "identity", {"source"}));
    nodes.push_back(node("fast_A", 2, "window", {"close"}));
    nodes.push_back(node("fast_B", 3, "window", {"close"}));
    nodes.push_back(node("slow", 4, "window", {"close"}));
    nodes.push_back(node("spread", 5, "difference", {"fast_A", "slow"}));
    for (auto index : {2U, 3U, 4U}) {
        nodes[index].spec.parameters.values.push_back({"window", std::uint64_t{index == 4 ? 50U : 20U}, Origin::ExplicitConfig, "XML"});
    }
    auto graph = take(build_graph(nodes, limits(), account(), fnv1a64));
    require(graph.logical_order().size() == 6 && graph.physical_order().size() == 5, "six/five structural mapping mismatch");
    require(graph.representative(2) == graph.representative(3), "exact duplicate did not share");
    require(graph.representative(2) != graph.representative(4), "different window shared");
    nodes[3].spec.label = "different consumer label";
    nodes[3].spec.parameters.values[0].origin = Origin::Derived;
    nodes[3].spec.parameters.values[0].provenance = "different display origin";
    auto collision_graph = take(build_graph(nodes, limits(), account(), collision));
    require(collision_graph.physical_order().size() == 5, "collision/full equality or display metadata handling failed");
    auto unshared = take(build_graph(nodes, limits(false), account(), fnv1a64));
    require(unshared.physical_order().size() == 6 && unshared.identity(2) == graph.identity(2), "sharing mode altered pure semantic identity");
}
void full_lineage_and_selector() {
    std::vector<GraphNode> nodes{node("A", 0, "source", {}), node("B", 1, "identity", {"A"}), node("C", 2, "identity", {"B"})};
    auto original = take(build_graph(nodes, limits(), account(), collision));
    nodes[0].facts.sources[0].source_version = "v2";
    auto changed = take(build_graph(nodes, limits(), account(), collision));
    require(original.identity(1) != changed.identity(1) && original.identity(2) != changed.identity(2), "upstream change did not propagate transitively");
    nodes.push_back(node("B2", 3, "identity", {"A"}));
    auto second_definition = nodes.back().definition->contract();
    second_definition.key.implementation_selector = "other-factory-in-the-same-binary";
    nodes.back().spec.operation = second_definition.key;
    nodes.back().definition = take(make_definition(std::move(second_definition), {}, {}));
    auto selected = take(build_graph(nodes, limits(), account(), collision));
    require(selected.identity(1) != selected.identity(3) && selected.representative(1) != selected.representative(3),
            "implementation selector was omitted from exact identity");
}
void ordered_ports_and_effects() {
    std::vector<GraphNode> nodes{node("left", 0, "source", {}), node("right", 1, "source", {}),
        node("subtract", 2, "difference", {"left", "right"}), node("reverse", 3, "difference", {"right", "left"}),
        node("action-A", 4, "action", {"left"}, SharingClass::Unique), node("action-B", 5, "action", {"left"}, SharingClass::Unique)};
    nodes[1].facts.sources[0].content_digest[0] = std::byte{99};
    auto graph = take(build_graph(nodes, limits(), account(), collision));
    require(graph.identity(2) != graph.identity(3), "noncommutative port order was normalized away");
    require(graph.identity(4) == graph.identity(5) && graph.representative(4) != graph.representative(5), "unique effects were deduplicated");
    nodes.push_back(node("after-A", 6, "identity", {"action-A"}));
    nodes.push_back(node("after-B", 7, "identity", {"action-B"}));
    auto descendants = take(build_graph(nodes, limits(), account(), collision));
    require(!descendants.shareable(6) && descendants.representative(6) != descendants.representative(7), "unique upstream eligibility was lost");
}
void frozen_logical_order() {
    std::vector<GraphNode> nodes{node("S", 0, "source", {}), node("p1", 1, "identity", {"S"}),
        node("B", 2, "action", {"p2"}, SharingClass::Unique), node("A", 3, "action", {"S"}, SharingClass::Unique),
        node("p2", 4, "identity", {"S"})};
    auto graph = take(build_graph(nodes, limits(), account(), collision));
    require(std::vector<std::uint32_t>(graph.logical_order().begin(), graph.logical_order().end()) == std::vector<std::uint32_t>{0, 1, 3, 4, 2}, "logical Kahn tie order changed");
    require(std::vector<std::uint32_t>(graph.physical_order().begin(), graph.physical_order().end()) == std::vector<std::uint32_t>{0, 1, 3, 2}, "physical graph was independently sorted");
    std::vector<std::string> actions;
    std::vector<bool> reached(5, false);
    std::size_t producer_advances = 0;
    auto failed = walk_logical(graph, [&](std::uint32_t logical, std::uint32_t, bool first) -> Result<void> {
        reached[logical] = true;
        if (logical == 1 || logical == 4) { producer_advances += first ? 1 : 0; }
        if (logical == 2) { return diagnostic(FailureCode::NumericOverflow, "B fails"); }
        if (logical == 3) { actions.push_back("A"); }
        return ok();
    });
    rejects(std::move(failed), FailureCode::NumericOverflow);
    require(actions == std::vector<std::string>{"A"} && producer_advances == 1, "B failure suppressed A or repeated a shared producer");
    reached.assign(5, false);
    rejects(walk_logical(graph, [&](std::uint32_t logical, std::uint32_t, bool) -> Result<void> {
        reached[logical] = true;
        if (logical == 3) { return diagnostic(FailureCode::CallbackException, "failure after A before alias"); }
        return ok();
    }), FailureCode::CallbackException);
    require(reached[1] && !reached[4] && !reached[2], "later alias was broadcast before its logical boundary");
}
void rejects_invalid_graphs() {
    std::vector<GraphNode> nodes{node("a", 0, "identity", {"b"}), node("b", 1, "identity", {"a"})};
    rejects(build_graph(nodes, limits(), account(), fnv1a64), FailureCode::GraphCycle);
    nodes = {node("a", 0, "identity", {"a"})};
    rejects(build_graph(nodes, limits(), account(), fnv1a64), FailureCode::GraphCycle);
    nodes = {node("a", 0, "source", {}), node("b", 1, "identity", {"missing"})};
    rejects(build_graph(nodes, limits(), account(), fnv1a64), FailureCode::InvalidConfig);
    nodes = {node("a", 0, "source", {}), node("a", 1, "source", {})};
    rejects(build_graph(nodes, limits(), account(), fnv1a64), FailureCode::InvalidConfig);
    nodes = {node("a", 0, "source", {}), node("b", 0, "source", {})};
    rejects(build_graph(nodes, limits(), account(), fnv1a64), FailureCode::InvalidConfig);
    nodes = {node("a", 0, "source", {})};
    auto bounded = limits(); bounded.max_identity_bytes = 16;
    rejects(build_graph(nodes, bounded, account(), fnv1a64), FailureCode::ResourcePlanExceeded);
    nodes[0].facts.history = TimeHistory{60};
    rejects(build_graph(nodes, limits(), account(), fnv1a64), FailureCode::UnsupportedContract);
}
void canonical_bytes() {
    const std::string hello = "hello";
    require(fnv1a64(std::as_bytes(std::span{hello.data(), hello.size()})) == 0xa430d84680aabd0bULL, "FNV-1a64 known answer mismatch");
    std::vector<GraphNode> nodes{node("a", 0, "source", {})};
    auto graph = take(build_graph(nodes, limits(), account(), fnv1a64));
    const auto& bytes = graph.identity(0);
    const std::string domain = "lexongine.identity.v1";
    require(bytes.size() > 31 && bytes[0] == std::byte{0} && bytes[7] == std::byte{21}, "canonical domain length framing mismatch");
    for (std::size_t i = 0; i < domain.size(); ++i) { require(bytes[8 + i] == static_cast<std::byte>(domain[i]), "canonical domain mismatch"); }
    require(bytes[29] == std::byte{0} && bytes[30] == std::byte{1}, "canonical record kind mismatch");
}
void connection_policies() {
    std::vector<GraphNode> nodes{node("source", 0, "source", {}), node("consumer", 1, "identity", {"source"})};
    nodes[0].facts.missing_policy = MissingIntervalPolicy::Skip;
    rejects(build_graph(nodes, limits(), account(), fnv1a64), FailureCode::UnsupportedContract);
    nodes[0].facts.missing_policy = MissingIntervalPolicy::Strict;
    nodes[1].facts.processing_scope.clock = "another-clock";
    rejects(build_graph(nodes, limits(), account(), fnv1a64), FailureCode::UnsupportedContract);
    nodes[1].facts.processing_scope.clock = "clock";
    auto description = nodes[0].definition->contract();
    description.outputs[0].type.meaning.values[0].value = std::string{"other_units"};
    nodes[0].definition = take(make_definition(std::move(description), {}, {}));
    rejects(build_graph(nodes, limits(), account(), fnv1a64), FailureCode::TypeMismatch);
}
void semantic_field_mutations() {
    const std::vector<GraphNode> base{node("A", 0, "source", {}), node("B", 1, "identity", {"A"}), node("C", 2, "identity", {"B"})};
    auto original = take(build_graph(base, limits(), account(), collision));
    const std::vector<std::function<void(GraphNode&)>> changes{
        [](auto& value) { value.spec.parameters.values.push_back({"x", std::int64_t{1}, Origin::ExplicitConfig, "test"}); },
        [](auto& value) { value.facts.sources[0].content_digest[3] = std::byte{8}; },
        [](auto& value) { value.facts.sources[0].reference_digest[5] = std::byte{8}; },
        [](auto& value) { value.facts.sources[0].source_version = "new-version"; },
        [](auto& value) { value.facts.sources[0].query_selection.parameters.values.push_back({"filter", true, Origin::ExplicitConfig, "test"}); },
        [](auto& value) { value.facts.sources[0].provider_operation.implementation_selector = "other-provider-factory"; },
        [](auto& value) { value.facts.sources[0].provider_implementation.binary_digest[0] = std::byte{7}; },
        [](auto& value) { value.facts.sources[0].order.version = 2; },
        [](auto& value) { value.facts.sources[0].availability.version = 2; },
        [](auto& value) { value.facts.sources[0].validation.version = 2; },
        [](auto& value) { std::swap(value.facts.sources[0].coverage.complete_keys[0], value.facts.sources[0].coverage.complete_keys[1]); },
        [](auto& value) {
            auto& coverage = value.facts.sources[0].coverage;
            coverage.complete_keys.erase(coverage.complete_keys.begin() + 1);
            coverage.gaps.push_back({{"clock", 1, 2}, {60, 120}, 1});
        },
        [](auto& value) { value.spec.initialization.actual_prefix.end = 1; },
        [](auto& value) { value.facts.processing_scope.end = 100; },
        [](auto& value) { value.facts.history = CountHistory{1}; },
        [](auto& value) { value.facts.numerical_policy.version = 2; },
        [](auto& value) { value.facts.codec.version = 2; },
        [](auto& value) { value.facts.validation.version = 2; },
        [](auto& value) {
            auto definition = value.definition->contract(); definition.implementation.dependencies_digest[2] = std::byte{9};
            value.definition = take(make_definition(std::move(definition), {}, {}));
        },
    };
    // Some altered source facts intentionally would fail source admission. Graph
    // encoding must still retain those distinctions; this is not source validation.
    for (const auto& change : changes) {
        auto altered = base;
        change(altered[0]);
        auto graph = take(build_graph(altered, limits(), account(), collision));
        require(graph.identity(0) != original.identity(0) && graph.identity(1) != original.identity(1) && graph.identity(2) != original.identity(2),
                "a semantic field was omitted from ordered transitive identity");
    }
    auto strict = base[0]; auto skip = base[0];
    skip.facts.missing_policy = MissingIntervalPolicy::Skip;
    auto strict_graph = take(build_graph(std::span{&strict, 1}, limits(), account(), collision));
    auto skip_graph = take(build_graph(std::span{&skip, 1}, limits(), account(), collision));
    require(strict_graph.identity(0) != skip_graph.identity(0), "Strict and Skip identities collapsed");
}
void structure_precedes_source_identity() {
    std::vector<GraphNode> nodes{node("S", 0, "source", {}), node("p1", 1, "identity", {"S"}),
        node("B", 2, "action", {"p2"}, SharingClass::Unique), node("A", 3, "action", {"S"}, SharingClass::Unique),
        node("p2", 4, "identity", {"S"})};
    nodes[0].facts.sources.clear(); // Source validation has not happened yet.
    auto bounded = limits(); bounded.max_identity_bytes = 0;
    auto logical = take(build_logical_graph(nodes, bounded, account()));
    require(std::vector<std::uint32_t>(logical.logical_order().begin(), logical.logical_order().end()) == std::vector<std::uint32_t>{0, 1, 3, 4, 2},
        "preparation structure altered frozen declaration ordering");
    require(logical.inputs(2)[0].producer == 4 && logical.id(4) == "p2", "logical graph lost owned ordered dependency resolution");
    rejects(build_graph(nodes, bounded, account(), collision), FailureCode::ResourcePlanExceeded);
    nodes[0] = node("S", 0, "source", {});
    auto frozen = take(build_graph(nodes, limits(), account(), collision));
    require(std::equal(logical.logical_order().begin(), logical.logical_order().end(), frozen.logical_order().begin(), frozen.logical_order().end()),
        "source identity compilation changed the logical schedule");
}
}
int main() {
    const Case cases[]{{"exact sharing and forced collisions", exact_sharing}, {"transitive lineage and implementation selector", full_lineage_and_selector},
        {"ordered ports and unique eligibility", ordered_ports_and_effects}, {"frozen logical order and failure prefix", frozen_logical_order},
        {"invalid graph and bounds", rejects_invalid_graphs}, {"canonical wire prefix and production hash", canonical_bytes},
        {"declared connection policy gates", connection_policies}, {"independent semantic field mutations", semantic_field_mutations},
        {"logical structure before source identity", structure_precedes_source_identity}};
    return lexongine::test::run(cases);
}
