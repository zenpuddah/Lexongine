#pragma once

#include "lexongine/registry.hpp"

namespace lexongine {

struct SourceIdentity {
    std::string snapshot_name, source_version;
    Sha256 content_digest;
    OperationKey provider_operation;
    ImplementationIdentity provider_implementation;
    TypeContract interpretation;
    Contract query_selection;
    Sha256 reference_digest;
    SequenceScope validated_scope;
    SourceCoverage coverage;
    Contract order, availability, validation;
};
struct IdentityFacts {
    SequenceScope processing_scope;
    HistoryRequirement history;
    std::vector<SourceIdentity> sources;
    Contract numerical_policy;
    MissingIntervalPolicy missing_policy;
    Contract codec, validation;
};
struct GraphNode {
    NodeSpec spec;
    std::shared_ptr<const OperationDefinition> definition;
    IdentityFacts facts;
};
struct GraphLimits {
    std::uint64_t max_nodes, max_edges, max_identity_bytes, string_bytes;
    bool sharing;
};
struct ResolvedEdge { std::uint32_t producer; PortIndex output; };
using CandidateHasher = std::uint64_t (*)(std::span<const std::byte>);
using DigestFunction = Result<Sha256> (*)(std::span<const std::byte>);
[[nodiscard]] std::uint64_t fnv1a64(std::span<const std::byte>) noexcept;

// Dependency/type/policy structure only. Preparation can use this before source
// validation without inventing content identities or making sharing decisions.
class LogicalGraph {
public:
    [[nodiscard]] std::span<const std::uint32_t> logical_order() const noexcept;
    [[nodiscard]] std::span<const ResolvedEdge> inputs(std::uint32_t logical) const;
    [[nodiscard]] DeclaredOrdinal ordinal(std::uint32_t logical) const;
    [[nodiscard]] std::string_view id(std::uint32_t logical) const;
private:
    struct Storage;
    explicit LogicalGraph(std::shared_ptr<const Storage> storage) noexcept : storage_(std::move(storage)) {}
    friend Result<LogicalGraph> build_logical_graph(std::span<const GraphNode>, const GraphLimits&, std::shared_ptr<ResourceAccount>) noexcept;
    std::shared_ptr<const Storage> storage_;
};
[[nodiscard]] Result<LogicalGraph> build_logical_graph(std::span<const GraphNode>, const GraphLimits&,
                                                      std::shared_ptr<ResourceAccount>) noexcept;

// Structural compilation does not attest source validity or authorize execution.
// Preparation supplies verified source/implementation facts and owns the full
// immutable composition/definitions; this result owns identities and schedule.
class FrozenGraph {
public:
    [[nodiscard]] std::span<const std::uint32_t> logical_order() const noexcept;
    [[nodiscard]] std::span<const std::uint32_t> physical_order() const noexcept;
    [[nodiscard]] std::span<const ResolvedEdge> inputs(std::uint32_t logical) const;
    [[nodiscard]] std::uint32_t representative(std::uint32_t logical) const;
    [[nodiscard]] std::uint32_t physical_index(std::uint32_t logical) const;
    [[nodiscard]] DeclaredOrdinal ordinal(std::uint32_t logical) const;
    [[nodiscard]] std::string_view id(std::uint32_t logical) const;
    [[nodiscard]] const std::vector<std::byte>& identity(std::uint32_t logical) const;
    [[nodiscard]] std::uint64_t candidate_hash(std::uint32_t logical) const;
    [[nodiscard]] bool shareable(std::uint32_t logical) const;
private:
    struct Storage;
    explicit FrozenGraph(std::shared_ptr<const Storage> storage) noexcept : storage_(std::move(storage)) {}
    friend Result<FrozenGraph> build_graph(std::span<const GraphNode>, const GraphLimits&,
                                          std::shared_ptr<ResourceAccount>, CandidateHasher) noexcept;
    std::shared_ptr<const Storage> storage_;
};
[[nodiscard]] Result<FrozenGraph> build_graph(std::span<const GraphNode>, const GraphLimits&,
                                             std::shared_ptr<ResourceAccount>, CandidateHasher) noexcept;

// One logical-order traversal, reusable by the executor. Aliases reach the
// callback at their own boundary; only their first physical occurrence resolves.
template<class Callback> Result<void> walk_logical(const FrozenGraph& graph, Callback&& callback) noexcept {
    return guarded([&]() -> Result<void> {
        for (const auto logical : graph.logical_order()) {
            auto result = callback(logical, graph.physical_index(logical), graph.representative(logical) == logical);
            if (auto* error = std::get_if<Diagnostic>(&result)) {
                if (!error->logical_ordinal) { error->logical_ordinal = graph.ordinal(logical); }
                return *error;
            }
        }
        return ok();
    });
}

} // namespace lexongine
