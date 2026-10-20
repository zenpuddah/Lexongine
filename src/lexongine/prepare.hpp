#pragma once

#include "lexongine/graph.hpp"

namespace lexongine {

struct SourceValidationLimits {
    std::uint64_t max_records, max_timeline_events, max_gap_entries, max_expected_updates;
};
struct CoverageSummary {
    std::uint64_t complete_updates, missing_intervals, timeline_events;
};

// Validates the host timeline/coverage association, not provider correctness or
// canonical payload validity. The source service must establish those separately
// before a snapshot is admitted. Payload indices are bounded physical references,
// never inferred acquisition ordinals or semantic event identities.
[[nodiscard]] inline Result<CoverageSummary>
validate_source_timeline(const SourceCoverage &coverage, std::span<const TimingDescriptor> timeline,
                         std::uint64_t payload_count, MissingIntervalPolicy policy,
                         const SourceValidationLimits &limits) noexcept {
    const auto &scope = coverage.expected_scope;
    if (scope.clock.empty() || scope.end < scope.begin ||
        coverage.expected_timeline_contract.id.empty() ||
        coverage.expected_timeline_contract.version == 0) {
        return diagnostic(FailureCode::SourceCoverage,
                          "source expected scope/timeline contract is invalid");
    }
    if (payload_count > limits.max_records || coverage.complete_keys.size() > limits.max_records ||
        timeline.size() > limits.max_timeline_events ||
        coverage.gaps.size() > limits.max_gap_entries ||
        scope.end - scope.begin > limits.max_expected_updates) {
        return diagnostic(FailureCode::ResourcePlanExceeded,
                          "source coverage exceeds explicit admission limits");
    }
    if (policy != MissingIntervalPolicy::Strict && policy != MissingIntervalPolicy::Skip) {
        return diagnostic(FailureCode::UnsupportedContract,
                          "unsupported source missing-interval policy");
    }
    if (policy == MissingIntervalPolicy::Strict && !coverage.gaps.empty()) {
        return diagnostic(FailureCode::SourceCoverage,
                          "Strict rejects a known required source gap");
    }
    auto expected = scope.begin;
    std::size_t complete = 0, gap_index = 0;
    std::uint64_t missing = 0;
    for (const auto &event : timeline) {
        if (event.key.clock != scope.clock || event.key.sequence != expected ||
            expected >= scope.end) {
            return diagnostic(
                FailureCode::SourceCoverage,
                "timeline keys are duplicated, reordered, out of scope or incomplete");
        }
        if (const auto *row = std::get_if<CompleteRecord>(&event.delivery)) {
            if (complete >= coverage.complete_keys.size() ||
                coverage.complete_keys[complete] != event.key ||
                row->payload_index >= payload_count) {
                return diagnostic(FailureCode::SourceCoverage,
                                  "complete timeline event disagrees with coverage/payload bounds");
            }
            if (const auto *interval = std::get_if<Interval>(&event.extent);
                interval && interval->end_ns <= interval->begin_ns) {
                return diagnostic(FailureCode::SourceCoverage,
                                  "complete record has invalid interval extent");
            }
            ++expected;
            ++complete;
        } else {
            const auto &gap = std::get<DeclaredGap>(event.delivery).gap;
            const auto *interval = std::get_if<Interval>(&event.extent);
            if (policy != MissingIntervalPolicy::Skip || gap_index >= coverage.gaps.size() ||
                !(coverage.gaps[gap_index] == gap) || gap.missing_scope.clock != scope.clock ||
                gap.missing_scope.begin != expected || gap.missing_scope.end <= expected ||
                gap.missing_scope.end > scope.end || !interval || *interval != gap.interval ||
                interval->end_ns <= interval->begin_ns) {
                return diagnostic(FailureCode::SourceCoverage,
                                  "declared gap disagrees with expected coverage/timing");
            }
            const auto added = checked_add(missing, gap.missing_scope.end - expected);
            if (const auto *error = std::get_if<Diagnostic>(&added)) {
                return *error;
            }
            missing = std::get<std::uint64_t>(added);
            expected = gap.missing_scope.end;
            ++gap_index;
        }
    }
    if (expected != scope.end || complete != coverage.complete_keys.size() ||
        gap_index != coverage.gaps.size()) {
        return diagnostic(FailureCode::SourceCoverage,
                          "source closed without exact complete-record/gap coverage");
    }
    return CoverageSummary{static_cast<std::uint64_t>(complete), missing,
                           static_cast<std::uint64_t>(timeline.size())};
}

[[nodiscard]] inline Result<void> validate_request_history(const ObservationRequest &request,
                                                           const SourceCoverage &coverage,
                                                           std::uint64_t preceding) noexcept {
    const auto &scope = coverage.expected_scope;
    if (scope.clock.empty() || scope.end < scope.begin || request.evaluation.clock != scope.clock ||
        request.evaluation.begin < scope.begin || request.evaluation.end > scope.end ||
        request.evaluation.end < request.evaluation.begin) {
        return diagnostic(FailureCode::SourceCoverage,
                          "evaluation exceeds the declared source scope");
    }
    if (request.absence != AbsencePolicy::RequireValue) {
        return diagnostic(FailureCode::UnsupportedContract,
                          "unsupported observation absence policy");
    }
    if (request.missing_intervals != MissingIntervalPolicy::Strict &&
        request.missing_intervals != MissingIntervalPolicy::Skip) {
        return diagnostic(FailureCode::UnsupportedContract,
                          "request has unsupported missing policy");
    }
    if (request.missing_intervals == MissingIntervalPolicy::Strict && !coverage.gaps.empty()) {
        return diagnostic(FailureCode::SourceCoverage,
                          "Strict request cannot consume a required source gap");
    }
    std::optional<std::uint64_t> previous;
    for (const auto &key : coverage.complete_keys) {
        if (key.clock != scope.clock || key.sequence < scope.begin || key.sequence >= scope.end ||
            (previous && key.sequence <= *previous)) {
            return diagnostic(FailureCode::SourceCoverage,
                              "readiness requires validated ordered complete keys");
        }
        previous = key.sequence;
    }
    const auto first = std::lower_bound(
        coverage.complete_keys.begin(), coverage.complete_keys.end(), request.evaluation.begin,
        [](const UpdateKey &key, std::uint64_t start) { return key.sequence < start; });
    if (first == coverage.complete_keys.end() || first->sequence >= request.evaluation.end ||
        static_cast<std::uint64_t>(first - coverage.complete_keys.begin()) < preceding) {
        return diagnostic(FailureCode::HistoryInsufficient,
                          "insufficient usable history at the declared evaluation boundary");
    }
    return ok();
}

struct CountHistoryPlan {
    Reservation charge;
    std::vector<std::uint64_t> preceding;
};
[[nodiscard]] inline Result<CountHistoryPlan>
plan_count_history(std::span<const std::vector<std::uint32_t>> parents,
                   std::span<const HistoryRequirement> local,
                   std::span<const std::uint32_t> logical_order,
                   std::shared_ptr<ResourceAccount> account) noexcept {
    return guarded([&]() -> Result<CountHistoryPlan> {
        if (!account || parents.size() != local.size() || logical_order.size() != local.size()) {
            return diagnostic(FailureCode::InvalidConfig,
                              "history planning requires matching frozen graph records");
        }
        auto bytes = checked_multiply(local.size(), sizeof(std::uint64_t) + sizeof(bool));
        if (const auto *error = std::get_if<Diagnostic>(&bytes)) {
            return *error;
        }
        bytes = checked_add(std::get<std::uint64_t>(bytes), sizeof(CountHistoryPlan) + 256);
        if (const auto *error = std::get_if<Diagnostic>(&bytes)) {
            return *error;
        }
        auto charged = account->reserve(ResourceKind::Graph, std::get<std::uint64_t>(bytes));
        if (const auto *error = std::get_if<Diagnostic>(&charged)) {
            return *error;
        }
        CountHistoryPlan result{std::get<Reservation>(std::move(charged)),
                                std::vector<std::uint64_t>(local.size())};
        std::vector<bool> resolved(local.size(), false);
        for (const auto index : logical_order) {
            if (index >= local.size() || resolved[index]) {
                return diagnostic(FailureCode::UpdateProtocol, "invalid frozen history schedule");
            }
            const auto *own = std::get_if<CountHistory>(&local[index]);
            if (!own) {
                return diagnostic(FailureCode::UnsupportedContract,
                                  "only declared count history is supported");
            }
            std::uint64_t parent_history = 0;
            for (const auto parent : parents[index]) {
                if (parent >= local.size() || !resolved[parent]) {
                    return diagnostic(FailureCode::UpdateProtocol,
                                      "history predecessor was not resolved in logical order");
                }
                parent_history = std::max(parent_history, result.preceding[parent]);
            }
            const auto total = checked_add(parent_history, own->preceding);
            if (const auto *error = std::get_if<Diagnostic>(&total)) {
                return *error;
            }
            result.preceding[index] = std::get<std::uint64_t>(total);
            resolved[index] = true;
        }
        return result;
    });
}

} // namespace lexongine
