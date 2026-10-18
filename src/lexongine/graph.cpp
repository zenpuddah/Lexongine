#include "lexongine/graph.hpp"
#include "lexongine/reuse.hpp"
#include "lexongine/plan.hpp"

#include <queue>

namespace lexongine {
namespace {
constexpr std::string_view domain = "lexongine.identity.v1";
constexpr std::size_t prefix_bytes = 8 + domain.size() + 2;

// Same traversal in counting and emitting mode. Counting allocates nothing;
// emission writes into a pre-sized, precharged buffer, including nested lengths.
class Encoder {
public:
    Encoder(std::uint64_t limit, std::uint64_t string_limit, std::vector<std::byte>* output = nullptr)
        : limit_(limit), string_limit_(string_limit), output_(output) {}
    [[nodiscard]] std::uint64_t size() const noexcept { return position_; }
    [[nodiscard]] const auto& error() const noexcept { return error_; }
    void fail(FailureCode code, std::string_view message) noexcept { if (!error_) { error_ = diagnostic(code, message); } }
    void raw(std::span<const std::byte> bytes) noexcept {
        if (error_) { return; }
        if (bytes.size() > limit_ - position_ ||
            (output_ && bytes.size() > output_->size() - static_cast<std::size_t>(position_))) {
            fail(FailureCode::ResourcePlanExceeded, "canonical identity exceeds admitted byte bound"); return;
        }
        if (output_) { std::copy(bytes.begin(), bytes.end(), output_->begin() + static_cast<std::ptrdiff_t>(position_)); }
        position_ += bytes.size();
    }
    void u8(std::uint8_t value) noexcept { const std::byte byte{value}; raw(std::span{&byte, 1}); }
    void u16(std::uint16_t value) noexcept {
        const std::array<std::byte, 2> bytes{static_cast<std::byte>(value >> 8), static_cast<std::byte>(value & 255)};
        raw(bytes);
    }
    void u64(std::uint64_t value) noexcept {
        std::array<std::byte, 8> bytes{};
        for (unsigned i = 0; i < 8; ++i) { bytes[i] = static_cast<std::byte>((value >> ((7 - i) * 8)) & 255); }
        raw(bytes);
    }
    void text(std::string_view value, bool required) noexcept {
        auto valid = validate_text(value, string_limit_, !required);
        if (const auto* error = std::get_if<Diagnostic>(&valid)) { fail(error->code, error->text()); return; }
        raw(std::as_bytes(std::span{value.data(), value.size()}));
    }
    template<class Function> void field(std::uint16_t tag, Function function) {
        if (error_) { return; }
        u16(tag);
        framed(function);
    }
    template<class Function> void list(std::size_t count, Function function) {
        u64(count);
        for (std::size_t i = 0; i < count && !error_; ++i) { framed([&] { function(i); }); }
    }
private:
    template<class Function> void framed(Function function) {
        const auto length_position = position_;
        u64(0);
        const auto begin = position_;
        if (!error_) { function(); }
        if (output_ && !error_) {
            const auto length = position_ - begin;
            for (unsigned i = 0; i < 8; ++i) {
                (*output_)[static_cast<std::size_t>(length_position) + i] = static_cast<std::byte>((length >> ((7 - i) * 8)) & 255);
            }
        }
    }
    std::uint64_t limit_, string_limit_, position_ = 0;
    std::vector<std::byte>* output_;
    std::optional<Diagnostic> error_;
};
void params(Encoder& e, const CanonicalParams& value) {
    if (value.schema_version == 0) { e.fail(FailureCode::InvalidConfig, "missing parameter schema version"); return; }
    for (std::size_t i = 1; i < value.values.size(); ++i) {
        if (!unsigned_text_less(value.values[i - 1].name, value.values[i].name)) {
            e.fail(FailureCode::InvalidConfig, "identity parameters must be sorted and unique"); return;
        }
    }
    e.field(1, [&] { e.u64(value.schema_version); });
    e.field(2, [&] { e.list(value.values.size(), [&](std::size_t i) {
        e.field(1, [&] { e.text(value.values[i].name, true); });
        e.field(2, [&] {
            const auto& parameter = value.values[i].value;
            e.u8(static_cast<std::uint8_t>(parameter.index() + 1));
            std::visit([&](const auto& scalar) {
                using T = std::decay_t<decltype(scalar)>;
                if constexpr (std::is_same_v<T, std::string>) { e.text(scalar, false); }
                else if constexpr (std::is_same_v<T, bool>) { e.u8(scalar ? 1 : 0); }
                else { e.u64(static_cast<std::uint64_t>(scalar)); }
            }, parameter);
        });
    }); });
}
void contract(Encoder& e, const Contract& value) {
    if (value.version == 0) { e.fail(FailureCode::InvalidConfig, "contract version must be explicit"); return; }
    e.field(1, [&] { e.text(value.id, true); });
    e.field(2, [&] { e.u64(value.version); });
    e.field(3, [&] { params(e, value.parameters); });
}
void type(Encoder& e, const TypeContract& value) {
    if (value.semantic_version == 0) { e.fail(FailureCode::InvalidConfig, "type version must be explicit"); return; }
    e.field(1, [&] { e.text(value.nominal_name, true); });
    e.field(2, [&] { e.u64(value.semantic_version); });
    e.field(3, [&] { params(e, value.meaning); });
}
void scope(Encoder& e, const SequenceScope& value) {
    if (value.end < value.begin) { e.fail(FailureCode::InvalidConfig, "reversed processing scope"); return; }
    e.field(1, [&] { e.text(value.clock, true); });
    e.field(2, [&] { e.u64(value.begin); });
    e.field(3, [&] { e.u64(value.end); });
}
void interval(Encoder& e, const Interval& value) {
    if (value.end_ns <= value.begin_ns) { e.fail(FailureCode::InvalidConfig, "invalid gap interval"); return; }
    e.field(1, [&] { e.u64(static_cast<std::uint64_t>(value.begin_ns)); });
    e.field(2, [&] { e.u64(static_cast<std::uint64_t>(value.end_ns)); });
}
void coverage(Encoder& e, const SourceCoverage& value) {
    e.field(1, [&] { scope(e, value.expected_scope); });
    e.field(2, [&] { e.list(value.complete_keys.size(), [&](std::size_t i) {
        e.field(1, [&] { e.text(value.complete_keys[i].clock, true); });
        e.field(2, [&] { e.u64(value.complete_keys[i].sequence); });
    }); });
    e.field(3, [&] { e.list(value.gaps.size(), [&](std::size_t i) {
        e.field(1, [&] { scope(e, value.gaps[i].missing_scope); });
        e.field(2, [&] { interval(e, value.gaps[i].interval); });
        e.field(3, [&] { e.u64(value.gaps[i].declared_reason); });
    }); });
    e.field(4, [&] { contract(e, value.expected_timeline_contract); });
}
void operation(Encoder& e, const OperationKey& key, const ImplementationIdentity& implementation) {
    if (key.semantic_version == 0) { e.fail(FailureCode::InvalidConfig, "operation version must be explicit"); return; }
    e.field(1, [&] { e.text(key.name_space, true); });
    e.field(2, [&] { e.text(key.name, true); });
    e.field(3, [&] { e.u64(key.semantic_version); });
    e.field(4, [&] { e.text(key.implementation_selector, true); });
    e.field(5, [&] { e.raw(implementation.binary_digest); });
    e.field(6, [&] { e.raw(implementation.dependencies_digest); });
    e.field(7, [&] { e.raw(implementation.source_build_digest); });
    e.field(8, [&] { e.text(implementation.sdk_build_contract_id, true); });
}
void source(Encoder& e, const SourceIdentity& value) {
    e.field(1, [&] { e.text(value.snapshot_name, true); });
    e.field(2, [&] { e.text(value.source_version, true); });
    e.field(3, [&] { e.raw(value.content_digest); });
    e.field(4, [&] { operation(e, value.provider_operation, value.provider_implementation); });
    e.field(5, [&] { type(e, value.interpretation); });
    e.field(6, [&] { contract(e, value.query_selection); });
    e.field(7, [&] { e.raw(value.reference_digest); });
    e.field(8, [&] { scope(e, value.validated_scope); });
    e.field(9, [&] { coverage(e, value.coverage); });
    e.field(10, [&] { contract(e, value.order); });
    e.field(11, [&] { contract(e, value.availability); });
    e.field(12, [&] { contract(e, value.validation); });
}
void missing(Encoder& e, MissingIntervalPolicy value) {
    if (value == MissingIntervalPolicy::Strict) { e.text("strict", true); }
    else if (value == MissingIntervalPolicy::Skip) { e.text("skip", true); }
    else { e.fail(FailureCode::UnsupportedContract, "unknown missing-interval policy"); }
}
struct IdentityBlob { Reservation charge; std::vector<std::byte> bytes; };
void computation(Encoder& e, const GraphNode& node, std::span<const ResolvedEdge> edges,
                 const std::vector<std::shared_ptr<const IdentityBlob>>& parents) {
    const auto& definition = node.definition->contract();
    e.u64(domain.size()); e.text(domain, true); e.u16(1);
    e.field(1, [&] { operation(e, definition.key, definition.implementation); });
    e.field(2, [&] { params(e, node.spec.parameters); });
    e.field(3, [&] { e.list(edges.size(), [&](std::size_t i) {
        const auto& binding = node.spec.inputs[i];
        e.field(1, [&] { e.text(binding.input_name, true); });
        e.field(2, [&] { e.raw(std::span<const std::byte>{parents[edges[i].producer]->bytes}.subspan(prefix_bytes)); });
        e.field(3, [&] { e.u64(edges[i].output); });
        e.field(4, [&] { e.u64(binding.prior_outputs); });
        e.field(5, [&] {
            e.field(1, [&] { e.text("same_step", true); });
            e.field(2, [&] { e.u64(1); });
            e.field(3, [&] { e.field(1, [&] { e.u64(1); }); e.field(2, [&] { e.u64(0); }); });
        });
        e.field(6, [&] { missing(e, binding.missing_intervals); });
    }); });
    e.field(4, [&] { e.list(definition.outputs.size(), [&](std::size_t i) {
        const auto& output = definition.outputs[i];
        e.field(1, [&] { e.text(output.name, true); });
        e.field(2, [&] { type(e, output.type); });
        e.field(3, [&] {
            if (output.storage == StorageKind::ScalarCopy) { e.text("scalar_copy", true); }
            else if (output.storage == StorageKind::ImmutableOwned) { e.text("immutable_owned", true); }
            else { e.fail(FailureCode::UnsupportedContract, "unsupported storage contract"); }
        });
        e.field(4, [&] { contract(e, output.codec); });
        e.field(5, [&] { contract(e, output.validation); });
    }); });
    e.field(5, [&] { e.list(node.facts.sources.size(), [&](std::size_t i) { source(e, node.facts.sources[i]); }); });
    e.field(6, [&] {
        e.field(1, [&] { contract(e, node.spec.initialization.contract); });
        e.field(2, [&] { scope(e, node.spec.initialization.actual_prefix); });
        e.field(3, [&] { params(e, node.spec.initialization.explicit_values); });
    });
    e.field(7, [&] { scope(e, node.facts.processing_scope); });
    e.field(8, [&] {
        if (const auto* count = std::get_if<CountHistory>(&node.facts.history)) {
            e.u8(1); e.field(1, [&] { e.u64(count->preceding); });
        } else { e.fail(FailureCode::UnsupportedContract, "only count history is supported"); }
    });
    e.field(9, [&] {
        e.field(1, [&] { contract(e, node.spec.update_policy); });
        e.field(2, [&] { contract(e, node.spec.availability_policy); });
    });
    e.field(10, [&] { contract(e, node.facts.numerical_policy); });
    e.field(11, [&] { missing(e, node.facts.missing_policy); });
    e.field(12, [&] {
        const auto& sharing = definition.sharing;
        e.field(1, [&] {
            if (sharing.classification == SharingClass::StatelessDeterministic) { e.text("stateless_deterministic", true); }
            else if (sharing.classification == SharingClass::StatefulDeclared) { e.text("stateful_declared", true); }
            else if (sharing.classification == SharingClass::Unique) { e.text("unique", true); }
            else { e.fail(FailureCode::UnsupportedContract, "unknown sharing class"); }
        });
        e.field(2, [&] { contract(e, sharing.initialization); });
        e.field(3, [&] { contract(e, sharing.update); });
        e.field(4, [&] { contract(e, sharing.effect); });
        e.field(5, [&] { e.u8(sharing.effect_free ? 1 : 0); });
        e.field(6, [&] { e.u8(sharing.replay_safe ? 1 : 0); });
        e.field(7, [&] { e.u8(sharing.cacheable ? 1 : 0); });
    });
    e.field(13, [&] { contract(e, node.facts.codec); });
    e.field(14, [&] { contract(e, node.facts.validation); });
}
void append(Diagnostic& error, std::string_view text) noexcept {
    const auto available = error.message.size() - error.length;
    const auto length = std::min(available, text.size());
    std::copy_n(text.data(), length, error.message.data() + error.length);
    error.length = static_cast<std::uint16_t>(error.length + length);
    error.truncated |= length != text.size();
}
}

struct LogicalGraph::Storage {
    Reservation charge;
    std::vector<std::string> ids;
    std::vector<DeclaredOrdinal> ordinals;
    std::vector<std::vector<ResolvedEdge>> edges;
    std::vector<std::uint32_t> logical;
};
struct FrozenGraph::Storage {
    Reservation charge;
    LogicalGraph structure;
    std::vector<std::uint32_t> physical, representative, physical_index;
    std::vector<std::shared_ptr<const IdentityBlob>> identities;
    std::vector<std::uint64_t> hashes;
    std::vector<bool> eligible;
    Storage(Reservation reservation, LogicalGraph graph) noexcept : charge(std::move(reservation)), structure(std::move(graph)) {}
};
std::uint64_t fnv1a64(std::span<const std::byte> bytes) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : bytes) { hash = (hash ^ std::to_integer<std::uint8_t>(byte)) * 1099511628211ULL; }
    return hash; // Intentional unsigned modulo-2^64 candidate hash, never equality authority.
}
Result<LogicalGraph> build_logical_graph(std::span<const GraphNode> nodes, const GraphLimits& limits,
                                         std::shared_ptr<ResourceAccount> account) noexcept {
    return guarded([&]() -> Result<LogicalGraph> {
        if (!account || nodes.size() > limits.max_nodes || nodes.size() > std::numeric_limits<std::uint32_t>::max()) {
            return diagnostic(FailureCode::ResourcePlanExceeded, "graph node count/account invalid");
        }
        std::uint64_t edge_count = 0, id_bytes = 0;
        for (const auto& node : nodes) {
            if (auto valid = validate_text(node.spec.id, limits.string_bytes, false); std::holds_alternative<Diagnostic>(valid)) { return std::get<Diagnostic>(valid); }
            auto total = checked_add(edge_count, node.spec.inputs.size());
            if (const auto* error = std::get_if<Diagnostic>(&total)) { return *error; }
            edge_count = std::get<std::uint64_t>(total);
            total = checked_add(id_bytes, node.spec.id.size() + 32);
            if (const auto* error = std::get_if<Diagnostic>(&total)) { return *error; }
            id_bytes = std::get<std::uint64_t>(total);
        }
        if (edge_count > limits.max_edges) { return diagnostic(FailureCode::ResourcePlanExceeded, "graph edge limit exceeded"); }
        auto node_bytes = checked_multiply(nodes.size(), sizeof(std::string) + sizeof(std::vector<ResolvedEdge>) + 128);
        auto edge_bytes = checked_multiply(edge_count, sizeof(ResolvedEdge));
        if (std::holds_alternative<Diagnostic>(node_bytes) || std::holds_alternative<Diagnostic>(edge_bytes)) {
            return diagnostic(FailureCode::ResourcePlanExceeded, "graph metadata size overflow");
        }
        auto size = checked_add(std::get<std::uint64_t>(node_bytes), std::get<std::uint64_t>(edge_bytes));
        if (const auto* error = std::get_if<Diagnostic>(&size)) { return *error; }
        size = checked_add(std::get<std::uint64_t>(size), id_bytes);
        if (const auto* error = std::get_if<Diagnostic>(&size)) { return *error; }
        size = checked_add(std::get<std::uint64_t>(size), sizeof(LogicalGraph::Storage) + 256);
        if (const auto* error = std::get_if<Diagnostic>(&size)) { return *error; }
        auto charge = account->reserve(ResourceKind::Graph, std::get<std::uint64_t>(size));
        if (const auto* error = std::get_if<Diagnostic>(&charge)) { return *error; }
        auto scratch_size = checked_multiply(nodes.size(), sizeof(std::uint64_t) + sizeof(std::pair<DeclaredOrdinal, std::uint32_t>));
        if (const auto* error = std::get_if<Diagnostic>(&scratch_size)) { return *error; }
        auto scratch = account->reserve(ResourceKind::Scratch, std::get<std::uint64_t>(scratch_size));
        if (const auto* error = std::get_if<Diagnostic>(&scratch)) { return *error; }
        auto storage = std::make_shared<LogicalGraph::Storage>();
        storage->charge = std::get<Reservation>(std::move(charge));
        storage->ids.reserve(nodes.size()); storage->ordinals.reserve(nodes.size());
        storage->edges.resize(nodes.size()); storage->logical.reserve(nodes.size());
        std::vector<std::uint64_t> indegree(nodes.size());
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const auto& node = nodes[i];
            for (std::size_t j = 0; j < i; ++j) {
                if (nodes[j].spec.id == node.spec.id || nodes[j].spec.ordinal == node.spec.ordinal) {
                    return diagnostic(FailureCode::InvalidConfig, "duplicate logical ID or declaration ordinal");
                }
            }
            if (!node.definition || !(node.definition->contract().key == node.spec.operation)) {
                return diagnostic(FailureCode::UnknownDefinition, "graph definition does not match its exact operation key");
            }
            const auto& definition = node.definition->contract();
            if (node.spec.inputs.size() != definition.inputs.size() || !(node.spec.update_policy == definition.sharing.update) ||
                !(node.spec.effect_policy == definition.sharing.effect) || !(node.spec.initialization.contract == definition.sharing.initialization)) {
                return diagnostic(FailureCode::UnsupportedContract, "graph policies/arity disagree with resolved definition");
            }
            if (std::find(definition.missing_protocols.begin(), definition.missing_protocols.end(), node.facts.missing_policy) == definition.missing_protocols.end()) {
                return diagnostic(FailureCode::UnsupportedContract, "operation does not accept selected missing policy");
            }
            storage->ids.push_back(node.spec.id); storage->ordinals.push_back(node.spec.ordinal);
            storage->edges[i].reserve(node.spec.inputs.size());
            for (std::size_t port = 0; port < node.spec.inputs.size(); ++port) {
                const auto& binding = node.spec.inputs[port];
                auto parent = std::find_if(nodes.begin(), nodes.end(), [&](const auto& candidate) { return candidate.spec.id == binding.producer; });
                if (parent == nodes.end() || binding.input_name != definition.inputs[port].name || !parent->definition) {
                    return diagnostic(FailureCode::InvalidConfig, "undeclared producer or wrong ordered input port");
                }
                const auto& outputs = parent->definition->contract().outputs;
                const auto output = std::find_if(outputs.begin(), outputs.end(), [&](const auto& candidate) { return candidate.name == binding.output_name; });
                if (output == outputs.end() || static_cast<std::size_t>(output - outputs.begin()) > std::numeric_limits<PortIndex>::max()) {
                    return diagnostic(FailureCode::InvalidConfig, "undeclared producer output");
                }
                if (binding.alignment != Alignment::SameStep || binding.missing_intervals != node.facts.missing_policy ||
                    binding.missing_intervals != parent->facts.missing_policy) {
                    return diagnostic(FailureCode::UnsupportedContract, "input binding protocol mismatch");
                }
                auto valid = validate_connection(definition.inputs[port], output->type, parent->facts.processing_scope.clock,
                                                  node.facts.processing_scope.clock, binding.missing_intervals);
                if (const auto* error = std::get_if<Diagnostic>(&valid)) { return *error; }
                storage->edges[i].push_back({static_cast<std::uint32_t>(parent - nodes.begin()), static_cast<PortIndex>(output - outputs.begin())});
            }
            indegree[i] = storage->edges[i].size();
        }
        using Ready = std::pair<DeclaredOrdinal, std::uint32_t>;
        std::vector<Ready> ready_storage; ready_storage.reserve(nodes.size());
        std::priority_queue<Ready, std::vector<Ready>, std::greater<Ready>> ready{std::greater<Ready>{}, std::move(ready_storage)};
        for (std::uint32_t i = 0; i < nodes.size(); ++i) { if (indegree[i] == 0) { ready.emplace(nodes[i].spec.ordinal, i); } }
        while (!ready.empty()) {
            const auto current = ready.top().second; ready.pop(); storage->logical.push_back(current);
            for (std::uint32_t child = 0; child < nodes.size(); ++child) {
                for (const auto& edge : storage->edges[child]) {
                    if (edge.producer == current && --indegree[child] == 0) { ready.emplace(nodes[child].spec.ordinal, child); }
                }
            }
        }
        if (storage->logical.size() != nodes.size()) {
            auto error = diagnostic(FailureCode::GraphCycle, "cycle/dependent logical nodes:");
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                if (indegree[i] != 0) {
                    if (!error.logical_ordinal) { error.logical_ordinal = nodes[i].spec.ordinal; }
                    append(error, " "); append(error, nodes[i].spec.id);
                }
            }
            return error;
        }
        return LogicalGraph{std::move(storage)};
    });
}
Result<FrozenGraph> build_graph(std::span<const GraphNode> nodes, const GraphLimits& limits,
                                std::shared_ptr<ResourceAccount> account, CandidateHasher hasher) noexcept {
    return guarded([&]() -> Result<FrozenGraph> {
        if (!account || !hasher) { return diagnostic(FailureCode::ResourcePlanExceeded, "graph account/hasher invalid"); }
        auto structure = build_logical_graph(nodes, limits, account);
        if (const auto* error = std::get_if<Diagnostic>(&structure)) { return *error; }
        auto size = checked_multiply(nodes.size(), sizeof(std::shared_ptr<const IdentityBlob>) + sizeof(std::uint64_t) + 3 * sizeof(std::uint32_t) + sizeof(bool));
        if (const auto* error = std::get_if<Diagnostic>(&size)) { return *error; }
        size = checked_add(std::get<std::uint64_t>(size), sizeof(FrozenGraph::Storage) + 256);
        if (const auto* error = std::get_if<Diagnostic>(&size)) { return *error; }
        auto charge = account->reserve(ResourceKind::Graph, std::get<std::uint64_t>(size));
        if (const auto* error = std::get_if<Diagnostic>(&charge)) { return *error; }
        auto storage = std::make_shared<FrozenGraph::Storage>(std::get<Reservation>(std::move(charge)), std::get<LogicalGraph>(std::move(structure)));
        storage->identities.resize(nodes.size()); storage->hashes.resize(nodes.size()); storage->eligible.resize(nodes.size());
        storage->representative.resize(nodes.size()); storage->physical_index.resize(nodes.size()); storage->physical.reserve(nodes.size());
        // One structural implementation freezes logical order before interning.
        for (const auto current : storage->structure.logical_order()) {
            const auto& node = nodes[current];
            Encoder counter{limits.max_identity_bytes, limits.string_bytes};
            computation(counter, node, storage->structure.inputs(current), storage->identities);
            if (counter.error()) { return *counter.error(); }
            if (counter.size() > std::vector<std::byte>{}.max_size()) { return diagnostic(FailureCode::ResourcePlanExceeded, "identity buffer exceeds vector representation"); }
            auto bytes = checked_add(counter.size(), sizeof(IdentityBlob) + 256);
            if (const auto* error = std::get_if<Diagnostic>(&bytes)) { return *error; }
            auto reserved = account->reserve(ResourceKind::Graph, std::get<std::uint64_t>(bytes));
            if (const auto* error = std::get_if<Diagnostic>(&reserved)) { return *error; }
            auto identity = std::make_shared<IdentityBlob>(IdentityBlob{std::get<Reservation>(std::move(reserved)),
                std::vector<std::byte>(static_cast<std::size_t>(counter.size()))});
            Encoder encoder{counter.size(), limits.string_bytes, &identity->bytes};
            computation(encoder, node, storage->structure.inputs(current), storage->identities);
            if (encoder.error()) { return *encoder.error(); }
            const auto& sharing = node.definition->contract().sharing;
            bool eligible = sharing.classification != SharingClass::Unique && sharing.effect_free && !sharing.correctness_evidence.empty();
            for (const auto& edge : storage->structure.inputs(current)) { eligible = eligible && storage->eligible[edge.producer]; }
            storage->eligible[current] = eligible;
            const auto hash = hasher(identity->bytes); storage->hashes[current] = hash;
            std::optional<std::uint32_t> representative;
            if (limits.sharing && eligible) {
                for (const auto candidate : storage->physical) {
                    if (storage->eligible[candidate] && storage->hashes[candidate] == hash && storage->identities[candidate]->bytes == identity->bytes) {
                        representative = candidate; break;
                    }
                }
            }
            if (representative) {
                storage->representative[current] = *representative;
                storage->physical_index[current] = storage->physical_index[*representative];
                storage->identities[current] = storage->identities[*representative];
            } else {
                storage->representative[current] = current;
                storage->physical_index[current] = static_cast<std::uint32_t>(storage->physical.size());
                storage->physical.push_back(current);
                storage->identities[current] = std::move(identity);
            }
        }
        return FrozenGraph{std::move(storage)};
    });
}
std::span<const std::uint32_t> LogicalGraph::logical_order() const noexcept { return storage_->logical; }
std::span<const ResolvedEdge> LogicalGraph::inputs(std::uint32_t logical) const { return storage_->edges.at(logical); }
DeclaredOrdinal LogicalGraph::ordinal(std::uint32_t logical) const { return storage_->ordinals.at(logical); }
std::string_view LogicalGraph::id(std::uint32_t logical) const { return storage_->ids.at(logical); }
std::span<const std::uint32_t> FrozenGraph::logical_order() const noexcept { return storage_->structure.logical_order(); }
std::span<const std::uint32_t> FrozenGraph::physical_order() const noexcept { return storage_->physical; }
std::span<const ResolvedEdge> FrozenGraph::inputs(std::uint32_t logical) const { return storage_->structure.inputs(logical); }
std::uint32_t FrozenGraph::representative(std::uint32_t logical) const { return storage_->representative.at(logical); }
std::uint32_t FrozenGraph::physical_index(std::uint32_t logical) const { return storage_->physical_index.at(logical); }
DeclaredOrdinal FrozenGraph::ordinal(std::uint32_t logical) const { return storage_->structure.ordinal(logical); }
std::string_view FrozenGraph::id(std::uint32_t logical) const { return storage_->structure.id(logical); }
const std::vector<std::byte>& FrozenGraph::identity(std::uint32_t logical) const { return storage_->identities.at(logical)->bytes; }
std::uint64_t FrozenGraph::candidate_hash(std::uint32_t logical) const { return storage_->hashes.at(logical); }
bool FrozenGraph::shareable(std::uint32_t logical) const { return storage_->eligible.at(logical); }

struct ArtifactIdentity::Data {
    Reservation metadata_charge, bytes_charge;
    SequenceScope processing_scope;
    std::vector<TypeContract> outputs;
    Contract codec, validation;
    std::vector<std::byte> bytes;
    Sha256 digest;
    std::size_t computation_size;
};
namespace {
struct IdentityFailure { Diagnostic error; };
template<class T> T identity_checked(std::variant<T, Diagnostic> result) {
    if (const auto* error = std::get_if<Diagnostic>(&result)) { throw IdentityFailure{*error}; } return std::get<T>(std::move(result));
}
std::uint64_t contract_storage_bytes(const Contract& value) {
    auto bytes = identity_checked(checked_add(sizeof(Contract) + 64, value.id.size()));
    for (const auto& parameter : value.parameters.values) {
        bytes = identity_checked(checked_add(bytes, sizeof(NamedParam) + 96));
        bytes = identity_checked(checked_add(bytes, parameter.name.size()));
        bytes = identity_checked(checked_add(bytes, parameter.provenance.size()));
        if (const auto* text = std::get_if<std::string>(&parameter.value)) { bytes = identity_checked(checked_add(bytes, text->size())); }
    }
    return bytes;
}
void artifact_record(Encoder& encoder, const PreparedPlan& plan, std::uint32_t logical, const Contract& codec, const Contract& validation) {
    const auto& computation_bytes = plan.graph().identity(logical);
    const auto& node = plan.nodes()[logical];
    encoder.u64(domain.size()); encoder.text(domain, true); encoder.u16(2);
    encoder.field(1, [&] { encoder.raw(std::span<const std::byte>{computation_bytes}.subspan(prefix_bytes)); });
    encoder.field(2, [&] { scope(encoder, node.facts.processing_scope); });
    encoder.field(3, [&] {
        const auto& outputs = node.definition->contract().outputs;
        encoder.list(outputs.size(), [&](std::size_t i) { type(encoder, outputs[i].type); });
    });
    encoder.field(4, [&] { contract(encoder, codec); });
    encoder.field(5, [&] { contract(encoder, validation); });
}
}
Result<ArtifactIdentity> ArtifactIdentity::create(const PreparedPlan& plan, std::uint32_t logical, const Contract& codec,
    const Contract& validation, DigestFunction digest_function, std::shared_ptr<ResourceAccount> account, const IdentityLimits& limits) noexcept {
    return guarded([&]() -> Result<ArtifactIdentity> {
        try {
            if (!account || !digest_function || logical >= plan.nodes().size()) { return diagnostic(FailureCode::InvalidConfig, "artifact identity needs a prepared producer and explicit services"); }
            Encoder counter{limits.canonical_bytes, limits.string_bytes}; artifact_record(counter, plan, logical, codec, validation);
            if (counter.error()) { return *counter.error(); }
            if (counter.size() > std::vector<std::byte>{}.max_size()) { return diagnostic(FailureCode::ResourcePlanExceeded, "artifact canonical identity cannot fit its buffer"); }
            const auto& node = plan.nodes()[logical];
            auto metadata_bytes = identity_checked(checked_add(sizeof(Data) + 1024, node.facts.processing_scope.clock.size()));
            metadata_bytes = identity_checked(checked_add(metadata_bytes, contract_storage_bytes(codec)));
            metadata_bytes = identity_checked(checked_add(metadata_bytes, contract_storage_bytes(validation)));
            for (const auto& output : node.definition->contract().outputs) {
                metadata_bytes = identity_checked(checked_add(metadata_bytes, identity_checked(type_storage_bytes(output.type))));
            }
            const auto workspace = identity_checked(checked_add(4096, identity_checked(checked_multiply(counter.size(), 3))));
            if (metadata_bytes > limits.metadata_bytes || workspace > limits.digest_scratch_bytes) { return diagnostic(FailureCode::ResourcePlanExceeded, "artifact identity metadata/digest exceeds explicit bounds"); }
            auto metadata_charge = identity_checked(account->reserve(ResourceKind::Graph, limits.metadata_bytes));
            auto bytes_charge = identity_checked(account->reserve(ResourceKind::Graph, counter.size()));
            auto scratch = identity_checked(account->reserve(ResourceKind::Scratch, workspace));
            Data data{std::move(metadata_charge), std::move(bytes_charge), node.facts.processing_scope, {}, codec, validation,
                std::vector<std::byte>(static_cast<std::size_t>(counter.size())), {}, plan.graph().identity(logical).size() - prefix_bytes};
            data.outputs.reserve(node.definition->contract().outputs.size());
            for (const auto& output : node.definition->contract().outputs) { data.outputs.push_back(output.type); }
            Encoder writer{counter.size(), limits.string_bytes, &data.bytes}; artifact_record(writer, plan, logical, codec, validation);
            if (writer.error()) { return *writer.error(); }
            data.digest = identity_checked(digest_function(data.bytes));
            if (data.digest == Sha256{}) { return diagnostic(FailureCode::InvalidConfig, "artifact digest service returned an empty identity"); }
            return ArtifactIdentity{std::make_shared<const Data>(std::move(data))};
        } catch (const IdentityFailure& failure) { return failure.error; }
    });
}
std::span<const std::byte> ArtifactIdentity::canonical_bytes() const noexcept { return data_->bytes; }
const Sha256& ArtifactIdentity::digest() const noexcept { return data_->digest; }
const SequenceScope& ArtifactIdentity::processing_scope() const noexcept { return data_->processing_scope; }
std::span<const TypeContract> ArtifactIdentity::output_types() const noexcept { return data_->outputs; }
const Contract& ArtifactIdentity::codec() const noexcept { return data_->codec; }
const Contract& ArtifactIdentity::validation() const noexcept { return data_->validation; }
bool ArtifactIdentity::matches(const PreparedPlan& plan, std::uint32_t logical) const noexcept {
    if (!data_ || logical >= plan.nodes().size() || data_->processing_scope != plan.nodes()[logical].facts.processing_scope) { return false; }
    const auto computation = std::span<const std::byte>{plan.graph().identity(logical)}.subspan(prefix_bytes);
    return computation.size() == data_->computation_size && std::ranges::equal(computation,
        std::span<const std::byte>{data_->bytes}.subspan(prefix_bytes + 10, data_->computation_size));
}
bool operator==(const ArtifactIdentity& left, const ArtifactIdentity& right) noexcept {
    return left.data_->digest == right.data_->digest && left.data_->bytes == right.data_->bytes;
}
} // namespace lexongine
