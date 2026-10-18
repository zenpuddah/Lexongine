#include "lexongine_test_support.hpp"
#include "lexongine/prepare.hpp"

namespace {
using namespace lexongine;
using namespace lexongine::test;
SourceCoverage coverage() {
    return {{"clock", 0, 4}, {{"clock", 0}, {"clock", 2}, {"clock", 3}},
        {{{"clock", 1, 2}, {60, 120}, 1}}, {"test_intervals", 1, {1, {}}}};
}
std::vector<TimingDescriptor> timeline() {
    return {{{"clock", 0}, Interval{0, 60}, 60, CompleteRecord{0}},
        {{"clock", 1}, Interval{60, 120}, 120, DeclaredGap{{{"clock", 1, 2}, {60, 120}, 1}}},
        {{"clock", 2}, Interval{120, 180}, 180, CompleteRecord{1}},
        {{"clock", 3}, Interval{180, 240}, 240, CompleteRecord{2}}};
}
SourceValidationLimits limits() { return {120, 120, 120, 120}; }
void strict_skip_partition() {
    const auto source = coverage(); const auto events = timeline();
    rejects(validate_source_timeline(source, events, 3, MissingIntervalPolicy::Strict, limits()), FailureCode::SourceCoverage);
    const auto summary = take(validate_source_timeline(source, events, 3, MissingIntervalPolicy::Skip, limits()));
    require(summary.complete_updates == 3 && summary.missing_intervals == 1 && summary.timeline_events == 4, "source gap was compressed or treated as data");
    auto changed = source; changed.gaps.clear();
    rejects(validate_source_timeline(changed, events, 3, MissingIntervalPolicy::Skip, limits()), FailureCode::SourceCoverage);
    changed = source; changed.complete_keys[1].sequence = 1;
    rejects(validate_source_timeline(changed, events, 3, MissingIntervalPolicy::Skip, limits()), FailureCode::SourceCoverage);
    auto reordered = events; std::swap(reordered[0], reordered[2]);
    rejects(validate_source_timeline(source, reordered, 3, MissingIntervalPolicy::Skip, limits()), FailureCode::SourceCoverage);
    reordered = events; reordered.pop_back();
    rejects(validate_source_timeline(source, reordered, 3, MissingIntervalPolicy::Skip, limits()), FailureCode::SourceCoverage);
}
void point_time_and_bounds() {
    SourceCoverage source{{"points", 0, 3}, {{"points", 0}, {"points", 1}, {"points", 2}}, {}, {"returned_succession", 1, {1, {}}}};
    const std::vector<TimingDescriptor> points{{{"points", 0}, PointTime{100}, 110, CompleteRecord{0}},
        {{"points", 1}, PointTime{100}, 111, CompleteRecord{1}}, {{"points", 2}, PointTime{95}, 112, CompleteRecord{2}}};
    require(take(validate_source_timeline(source, points, 3, MissingIntervalPolicy::Strict, limits())).complete_updates == 3,
            "point events were forced into sorted event times/intervals");
    auto small = limits(); small.max_records = 2;
    rejects(validate_source_timeline(source, points, 3, MissingIntervalPolicy::Strict, small), FailureCode::ResourcePlanExceeded);
    auto bad = points; std::get<CompleteRecord>(bad[2].delivery).payload_index = 3;
    rejects(validate_source_timeline(source, bad, 3, MissingIntervalPolicy::Strict, limits()), FailureCode::SourceCoverage);
    auto bad_gap = timeline(); bad_gap[1].extent = PointTime{60};
    rejects(validate_source_timeline(coverage(), bad_gap, 3, MissingIntervalPolicy::Skip, limits()), FailureCode::SourceCoverage);
}
void readiness_counts_actual_inputs() {
    const auto source = coverage();
    ObservationRequest request{MissingIntervalPolicy::Skip, "mean", "sma", "mean", {"clock", 2, 4}, AbsencePolicy::RequireValue, 0, "mean"};
    success(validate_request_history(request, source, 1));
    rejects(validate_request_history(request, source, 2), FailureCode::HistoryInsufficient);
    request.evaluation.begin = 1; // The declared gap remains inside evaluation; first actual value is still key 2.
    success(validate_request_history(request, source, 1));
    require(request.evaluation.begin == 1, "evaluation boundary silently shifted");
    request.evaluation.end = 5;
    rejects(validate_request_history(request, source, 1), FailureCode::SourceCoverage);
    SourceCoverage all_gap{{"clock", 0, 4}, {}, {{{"clock", 0, 4}, {0, 240}, 1}}, {"test_intervals", 1, {1, {}}}};
    request.evaluation = {"clock", 0, 4};
    rejects(validate_request_history(request, all_gap, 0), FailureCode::HistoryInsufficient);
    request.missing_intervals = MissingIntervalPolicy::Strict;
    rejects(validate_request_history(request, source, 0), FailureCode::SourceCoverage);
}
void reference_history_recurrence() {
    // source, close, SMA50(close), SMA20(SMA50), lag1(SMA50), difference.
    const std::vector<std::vector<std::uint32_t>> parents{{}, {0}, {1}, {2}, {2}, {3, 4}};
    const std::vector<HistoryRequirement> own{CountHistory{0}, CountHistory{0}, CountHistory{49}, CountHistory{19}, CountHistory{1}, CountHistory{0}};
    auto account = take(ResourceAccount::create({{65536, 65536, 65536, 65536, 65536, 65536, 65536}, 64, 64, 128, 128}));
    const std::array<std::uint32_t, 6> order{0, 1, 2, 3, 4, 5};
    auto plan = take(plan_count_history(parents, own, order, account));
    require(plan.preceding == std::vector<std::uint64_t>{0, 0, 49, 68, 50, 68}, "history composed by sum/fan-out instead of max-parent plus local count");
    auto unsupported = own; unsupported[2] = TimeHistory{60};
    rejects(plan_count_history(parents, unsupported, order, account), FailureCode::UnsupportedContract);
    auto overflow = own; overflow[3] = CountHistory{std::numeric_limits<std::uint64_t>::max()};
    rejects(plan_count_history(parents, overflow, order, account), FailureCode::ResourcePlanExceeded);
    auto wrong = order; std::swap(wrong[2], wrong[3]);
    rejects(plan_count_history(parents, own, wrong, account), FailureCode::UpdateProtocol);
}
}
int main() {
    const Case cases[]{{"Strict/Skip exact expected-scope partition", strict_skip_partition}, {"point timing and source bounds", point_time_and_bounds},
        {"readiness counts actual inputs", readiness_counts_actual_inputs}, {"declared count-history recurrence", reference_history_recurrence}};
    return lexongine::test::run(cases);
}
