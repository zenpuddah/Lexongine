#include "qaedalus_replay_source.hpp"
#include "../../tests/lexongine_test_support.hpp"

namespace {
using namespace lexongine;
using namespace lexongine::test;
using namespace lexongine::qaedalus;
namespace q = quant;
q::data::Timestamp time(std::int64_t value) {
    return q::data::Timestamp::from_unix_nanos(value);
}
q::data::Trade trade(std::int64_t event, std::int64_t price) {
    using namespace q::data;
    return {{InstrumentId{42}, VenueId{7}, time(event), std::nullopt, std::uint64_t{7},
             std::nullopt, 0, SourceId{9}},
            Price::from_raw(price),
            Quantity::from_raw(1000000),
            std::nullopt};
}
class FakeReader final : public q::storage::MarketDataReader {
  public:
    std::vector<q::integrity::BoundObservation> rows{{q::data::StreamId{1}, 10, trade(100, 7)},
                                                     {q::data::StreamId{1}, 12, trade(100, 8)},
                                                     {q::data::StreamId{1}, 13, trade(95, 9)}};
    bool fail_after = false, unchecked_only = false, wrong_metadata = false,
         expected_partial = false;
    std::size_t split = 0;
    mutable std::size_t unchecked_calls = 0, checked_calls = 0;
    arrow::Result<q::storage::DatasetLocalMetadata>
    read_metadata(const q::storage::DataFragment &fragment) const override {
        return q::storage::DatasetLocalMetadata{fragment.data_kind,
                                                fragment.stream,
                                                fragment.time_range,
                                                q::ingestion::AcquisitionMode::Historical,
                                                wrong_metadata ? "unsupported"
                                                               : fragment.schema_version,
                                                "test-adapter-v1",
                                                "test-mapping-v1",
                                                {},
                                                {},
                                                std::nullopt};
    }
    arrow::Status read_batches(const q::storage::DataFragment &, q::storage::BatchProjection,
                               const q::storage::BatchConsumer &) const override {
        ++unchecked_calls;
        return arrow::Status::Invalid("unchecked access forbidden");
    }
    arrow::Status stream_events(const q::storage::DataFragment &,
                                const q::storage::EventConsumer &) const override {
        ++unchecked_calls;
        return arrow::Status::Invalid("unchecked access forbidden");
    }
    arrow::Status
    stream_ordered_events(std::span<const q::storage::DataFragment>,
                          q::integrity::ConsumerRequirements requirements,
                          const q::storage::SequencedEventConsumer &consumer) const override {
        ++checked_calls;
        if (unchecked_only) {
            return arrow::Status::NotImplemented("no checked capability");
        }
        if (!requirements.ordering || !requirements.groups || requirements.initial_state ||
            requirements.allow_degraded || requirements.explicit_partial != expected_partial) {
            return arrow::Status::Invalid("explicit requirements changed");
        }
        for (const auto bounds :
             {std::pair{std::size_t{0}, split}, std::pair{split, rows.size()}}) {
            for (auto i = bounds.first; i < bounds.second; ++i) {
                auto transient = rows[i];
                ARROW_RETURN_NOT_OK(consumer(transient));
                transient.event = trade(199, 999); // Callback-scoped ownership must not escape.
            }
        }
        return fail_after ? arrow::Status::Invalid("unresolved ordering at source close")
                          : arrow::Status::OK();
    }
};
ReplaySourceBinding binding() {
    q::ingestion::MarketDataQuery query{q::data::InstrumentId{42},
                                        q::data::VenueId{7},
                                        q::ingestion::MarketDataLevel::L3,
                                        q::ingestion::TimeRange{time(90), time(200)},
                                        std::nullopt,
                                        q::ingestion::TimeBasis::EventTime,
                                        q::ingestion::FetchPolicy::DeriveProviderRange,
                                        std::nullopt};
    q::storage::DataFragment fragment{
        q::storage::DataKind::Trades,
        {q::data::InstrumentId{42}, q::data::VenueId{7}, q::data::SourceId{9}},
        {time(90), time(200)},
        "quant-observations-v2",
        "/test/trades",
        "v1",
        "acquisition-1",
        "/test/package",
        q::data::StreamId{1},
        std::string(64, 'a')};
    return {query,
            {fragment},
            {true, true, false, false, false},
            ReplayDeliveryMode::OrderedEvent,
            q::storage::DataKind::Trades,
            "test-trades",
            {"trade-clock", 0, 3},
            {"qaedalus.ordinal_availability_table", 1, {1, {}}},
            {{10, 110}, {12, 111}, {13, 112}},
            MissingIntervalPolicy::Strict,
            {16, 65536, 65536, 65536},
            {"qaedalus", "checked_trade_replay", 1, "test-adapter"},
            {Sha256{std::byte{1}}, Sha256{std::byte{2}}, Sha256{std::byte{3}}, "test-build"},
            {}};
}
std::shared_ptr<ResourceAccount> account() {
    return take(ResourceAccount::create(
        {{1048576, 1048576, 1048576, 1048576, 1048576, 1048576, 1048576}, 128, 128, 2048, 128}));
}
void checked_capture_and_current_delivery() {
    auto reader = std::make_shared<FakeReader>();
    reader->split = 1;
    auto resources = account();
    auto snapshot = take(prepare_trade_replay(reader, binding(), resources));
    require(reader->checked_calls == 1 && reader->unchecked_calls == 0,
            "reader capability or capture count wrong");
    require(snapshot.statistics().selected == 3 && snapshot.acquisition_id() == "acquisition-1",
            "capture identity lost");
    auto cursor = take(snapshot.cursor(resources));
    const auto first = *cursor.descriptor();
    require(cursor.descriptor()->key.sequence == 0, "descriptor read advanced delivery");
    rejects(cursor.next({{"trade-clock", 1}, PointTime{100}, 111, 111}),
            FailureCode::UpdateProtocol);
    rejects(cursor.next({first.key, first.extent, 109, first.available_at_ns}),
            FailureCode::FutureRead);
    reader->rows[0].event = trade(100, 777);
    for (std::size_t index = 0; index < 3; ++index) {
        const auto descriptor = *cursor.descriptor();
        auto result = take(cursor.next({descriptor.key, descriptor.extent,
                                        descriptor.available_at_ns, descriptor.available_at_ns}));
        const auto &row = std::get<TradeReplayRecord>(result);
        require(row.value.price.raw() == static_cast<std::int64_t>(7 + index),
                "source mutated or callback view escaped");
        require(row.acquisition_ordinal == std::array<std::uint64_t, 3>{10, 12, 13}[index],
                "original succession was renumbered");
        if (index == 2) {
            require(std::get<PointTime>(row.timing.extent).event_time_ns == 95,
                    "event time was sorted or changed");
        }
    }
    require(cursor.descriptor() == nullptr, "future data available after closure");
    require(take(cursor.finish()).completed_scope == SequenceScope{"trade-clock", 0, 3},
            "source closure scope wrong");
    rejects(cursor.finish(), FailureCode::AlreadyExecuted);
}
void errors_block_admission() {
    auto reader = std::make_shared<FakeReader>();
    auto selected = binding();
    reader->unchecked_only = true;
    rejects(prepare_trade_replay(reader, selected, account()), FailureCode::UnsupportedContract);
    require(reader->unchecked_calls == 0,
            "unsupported checked reader fell back to unchecked access");
    reader->unchecked_only = false;
    reader->fail_after = true;
    rejects(prepare_trade_replay(reader, selected, account()), FailureCode::SourceInvalid);
    reader->fail_after = false;
    reader->wrong_metadata = true;
    rejects(prepare_trade_replay(reader, selected, account()), FailureCode::SourceChanged);
    reader->wrong_metadata = false;
    reader->rows[1].stream_id = q::data::StreamId{2};
    rejects(prepare_trade_replay(reader, selected, account()), FailureCode::SourceInvalid);
    reader->rows[1].stream_id = q::data::StreamId{1};
    std::get<q::data::Trade>(reader->rows[2].event).quantity = q::data::Quantity::from_raw(0);
    rejects(prepare_trade_replay(reader, selected, account()), FailureCode::SourceInvalid);
    reader->rows[2].event = trade(95, 9);
    selected.consumer_requirements.ordering = false;
    rejects(prepare_trade_replay(reader, selected, account()), FailureCode::UnsupportedContract);
    selected = binding();
    selected.selected_fragments[0].schema_version = "unsupported";
    rejects(prepare_trade_replay(reader, selected, account()), FailureCode::UnsupportedContract);
    selected = binding();
    selected.selected_fragments.push_back(selected.selected_fragments[0]);
    selected.selected_fragments[1].acquisition_id = "other-acquisition";
    rejects(prepare_trade_replay(reader, selected, account()), FailureCode::UnsupportedContract);
}
void bounds_and_identity() {
    auto reader = std::make_shared<FakeReader>();
    auto selected = binding();
    selected.limits.max_records = 2;
    rejects(prepare_trade_replay(reader, selected, account()), FailureCode::ResourcePlanExceeded);
    selected = binding();
    selected.limits.decoded_bytes = 1;
    rejects(prepare_trade_replay(reader, selected, account()), FailureCode::ResourcePlanExceeded);
    selected = binding();
    selected.availability.pop_back();
    rejects(prepare_trade_replay(reader, selected, account()), FailureCode::SourceCoverage);
    selected = binding();
    selected.expected_scope.end = 4;
    rejects(prepare_trade_replay(reader, selected, account()), FailureCode::SourceCoverage);
    const auto original =
        take(prepare_trade_replay(reader, binding(), account())).metadata().content_digest;
    for (std::size_t split = 0; split <= reader->rows.size(); ++split) {
        reader->split = split;
        require(
            take(prepare_trade_replay(reader, binding(), account())).metadata().content_digest ==
                original,
            "physical transport split changed source identity");
    }
    selected = binding();
    selected.availability[0].available_at_ns = 109;
    require(take(prepare_trade_replay(reader, selected, account())).metadata().content_digest !=
                original,
            "availability policy data omitted from identity");
    selected = binding();
    selected.implementation.binary_digest[0] = std::byte{99};
    require(take(prepare_trade_replay(reader, selected, account())).metadata().content_digest !=
                original,
            "adapter build omitted from source identity");
}
void explicit_query_and_cursor_lifetimes() {
    auto reader = std::make_shared<FakeReader>();
    auto selected = binding();
    selected.original_query.event_time_range = q::ingestion::TimeRange{time(100), time(101)};
    selected.expected_scope.end = 2;
    auto resources = account();
    auto snapshot = take(prepare_trade_replay(reader, selected, resources));
    require(snapshot.statistics().scanned == 3 && snapshot.statistics().filtered == 1 &&
                snapshot.statistics().selected == 2,
            "explicit query filtering was hidden or changed scope");
    auto first = take(snapshot.cursor(resources));
    auto second = take(snapshot.cursor(resources));
    rejects(first.finish(), FailureCode::SourceCoverage);
    const auto descriptor = *first.descriptor();
    auto row = take(first.next({descriptor.key, descriptor.extent, descriptor.available_at_ns,
                                descriptor.available_at_ns}));
    require(std::get<TradeReplayRecord>(row).acquisition_ordinal == 10 &&
                second.descriptor()->key.sequence == 0,
            "fresh cursors shared mutable position");
    std::weak_ptr<FakeReader> weak = reader;
    reader.reset();
    require(weak.expired(), "validated source retained a reader capability");
    row = take(second.next({descriptor.key, descriptor.extent, descriptor.available_at_ns,
                            descriptor.available_at_ns}));
    require(std::get<TradeReplayRecord>(row).value.price.raw() == 7,
            "delivery required a live reader");

    reader = std::make_shared<FakeReader>();
    selected = binding();
    selected.original_query.source_receive_time_range =
        q::ingestion::TimeRange{time(90), time(200)};
    rejects(prepare_trade_replay(reader, selected, account()), FailureCode::SourceInvalid);
    selected = binding();
    reader->rows[1].ordinal = 10;
    rejects(prepare_trade_replay(reader, selected, account()), FailureCode::SourceInvalid);
    reader->rows[1].ordinal = 12;
    selected.selected_fragments[0].evidence_digest.clear();
    rejects(prepare_trade_replay(reader, selected, account()), FailureCode::SourceInvalid);
    selected = binding();
    selected.adapter_operation.semantic_version = 0;
    rejects(prepare_trade_replay(reader, selected, account()), FailureCode::SourceInvalid);
}
void selected_coverage_is_not_query_coverage() {
    auto reader = std::make_shared<FakeReader>();
    auto selected = binding();
    selected.original_query.event_time_range = q::ingestion::TimeRange{time(80), time(250)};
    rejects(prepare_trade_replay(reader, selected, account()), FailureCode::SourceCoverage);
    require(reader->checked_calls == 0, "known uncovered scope reached replay callbacks");
    selected.consumer_requirements.explicit_partial = true;
    reader->expected_partial = true;
    auto partial = take(prepare_trade_replay(reader, selected, account()));
    require(partial.dataset_metadata().time_range == q::ingestion::TimeRange{time(90), time(200)} &&
                partial.binding().original_query.event_time_range ==
                    selected.original_query.event_time_range,
            "explicit partial selection was relabeled as full coverage");
    selected = binding();
    selected.original_query.event_time_range.reset();
    selected.original_query.source_receive_time_range =
        q::ingestion::TimeRange{time(90), time(200)};
    rejects(prepare_trade_replay(reader, selected, account()), FailureCode::UnsupportedContract);
}
struct FixtureData {
    Reservation charge;
    SourceMetadata metadata;
    std::vector<TimingDescriptor> timeline;
    std::vector<q::data::Trade> values;
};
using FixtureProvider = TypedSourceProvider<q::data::Trade, std::shared_ptr<const FixtureData>,
                                            std::shared_ptr<const FixtureData>, std::size_t>;
FixtureProvider fixture_provider(RegisteredType<q::data::Trade> type,
                                 std::shared_ptr<const FixtureData> input) {
    return {
        "trade-fixture",
        type,
        [input](const SourceRequest &, SourceValidationServices &)
            -> Result<std::shared_ptr<const FixtureData>> { return input; },
        [](const auto &data) -> Result<SourceBounds> {
            return SourceBounds{data->metadata.validated_scope,
                                {65536, 65536, 65536, 65536, 65536, 65536, 0}};
        },
        [](auto &&data, const SourceRequest &,
           SourceValidationServices &) -> Result<std::shared_ptr<const FixtureData>> {
            for (const auto &value : data->values) {
                if (!q::data::validate(value).empty()) {
                    return diagnostic(FailureCode::SourceInvalid, "invalid fixture Trade");
                }
            }
            return std::move(data);
        },
        [](const auto &data) -> Result<SourceMetadata> { return data->metadata; },
        [](const auto &data) -> Result<std::span<const TimingDescriptor>> {
            return std::span<const TimingDescriptor>{data->timeline};
        },
        [](const auto &, SourceCursorServices &) -> Result<std::size_t> { return std::size_t{0}; },
        [](std::size_t &index, const auto &data, const UpdateContext &,
           PublicationAllocator &allocator) -> Result<SourceDelivery<q::data::Trade>> {
            auto value = allocator.copy_output(0, data->values.at(index));
            if (const auto *error = std::get_if<Diagnostic>(&value)) {
                return *error;
            }
            return SourceDelivery<q::data::Trade>{SourceRecord<q::data::Trade>{
                data->timeline.at(index++), std::get<Immutable<q::data::Trade>>(std::move(value))}};
        },
        [](std::size_t &index, const auto &data) -> Result<SourceEnd> {
            if (index != data->values.size()) {
                return diagnostic(FailureCode::SourceCoverage, "fixture scope incomplete");
            }
            return SourceEnd{data->metadata.validated_scope};
        },
        {}};
}
void fixture_and_reader_share_source_publication() {
    auto reader = std::make_shared<FakeReader>();
    auto selected = binding();
    auto prepared = take(prepare_trade_replay(reader, selected, account()));
    auto fixture_account = account();
    auto charged = take(fixture_account->reserve(ResourceKind::Source, 16384));
    auto expected =
        std::make_shared<FixtureData>(FixtureData{std::move(charged), prepared.metadata(), {}, {}});
    expected->metadata.provider_implementation = {Sha256{std::byte{4}}, Sha256{std::byte{5}},
                                                  Sha256{std::byte{6}}, "test-fixture-build"};
    for (std::size_t index = 0; index < reader->rows.size(); ++index) {
        expected->values.push_back(std::get<q::data::Trade>(reader->rows[index].event));
        expected->timeline.push_back({{"trade-clock", index},
                                      PointTime{index == 2 ? 95 : 100},
                                      static_cast<std::int64_t>(110 + index),
                                      CompleteRecord{index}});
    }
    auto registry = take(Registry::create());
    auto type = take(register_trade_type(registry, {}));
    SourceRequest request{"checked-trades",       selected.snapshot_name,  "v1",
                          trade_interpretation(), selected.expected_scope, {1, {}}};
    for (std::size_t split = 0; split <= reader->rows.size(); ++split) {
        reader->split = split;
        SourceValidationServices validation{
            account(),
            {{16, 16, 0, 16}, 1024, 16384, 32768, 16384},
            MissingIntervalPolicy::Strict,
            {1048576, 1048576, 1048576, 1048576, 1048576, 1048576, 1048576}};
        auto resolver = take(SourceResolver::create(validation.account, 65536, 4));
        success(register_trade_replay_source(resolver, request.provider_name, type, reader,
                                             selected, validation.account));
        success(resolver.register_provider(fixture_provider(type, expected)));
        success(resolver.freeze());
        auto candidate = take(resolver.inspect(request, validation));
        auto replay = take(resolver.validate(std::move(candidate), request, validation));
        auto fixture_request = request;
        fixture_request.provider_name = "trade-fixture";
        candidate = take(resolver.inspect(fixture_request, validation));
        auto synthetic = take(resolver.validate(std::move(candidate), fixture_request, validation));
        require(replay.metadata().content_digest == prepared.metadata().content_digest &&
                    replay.metadata().source_reference_digest ==
                        synthetic.metadata().source_reference_digest &&
                    replay.metadata().provider_implementation !=
                        synthetic.metadata().provider_implementation,
                "source transport changed content/association facts or hid provider identity");
        auto replay_account = account(), fixture_run = account();
        PublicationAllocator replay_allocator{replay_account, 0}, fixture_allocator{fixture_run, 0};
        auto replay_cursor = take(replay.cursor(replay_allocator)),
             fixture_cursor = take(synthetic.cursor(fixture_allocator));
        auto replay_slot = take(ResultSlot::create(replay_account, {replay.output_contract()}, 0,
                                                   replay.metadata().content_digest,
                                                   replay.metadata().source_reference_digest));
        auto fixture_slot = take(ResultSlot::create(fixture_run, {synthetic.output_contract()}, 0,
                                                    synthetic.metadata().content_digest,
                                                    synthetic.metadata().source_reference_digest));
        for (std::size_t index = 0; index < expected->values.size(); ++index) {
            const auto &timing = expected->timeline[index];
            UpdateContext context{timing.key, timing.extent, timing.available_at_ns,
                                  timing.available_at_ns};
            auto actual = take(replay_cursor.next(context)),
                 generated = take(fixture_cursor.next(context));
            require(std::holds_alternative<PendingOutcome>(actual) &&
                        std::holds_alternative<PendingOutcome>(generated),
                    "provider paths emitted different statuses");
            replay_slot->commit(take(validate_pending(std::get<PendingOutcome>(std::move(actual)),
                                                      *replay_slot, context)));
            fixture_slot->commit(take(validate_pending(
                std::get<PendingOutcome>(std::move(generated)), *fixture_slot, context)));
            auto a = take(replay_slot->read<q::data::Trade>(0, context)),
                 b = take(fixture_slot->read<q::data::Trade>(0, context));
            require(q::integrity::same_observation(q::integrity::Observation{a.value},
                                                   q::integrity::Observation{b.value}) &&
                        a.value.price == expected->values[index].price &&
                        a.publication().key == b.publication().key &&
                        a.publication().extent == b.publication().extent &&
                        a.publication().available_at_ns == b.publication().available_at_ns &&
                        a.publication().source_reference_digest ==
                            b.publication().source_reference_digest,
                    "fixture/reader publication diverged");
        }
        require(take(replay_cursor.finish()).completed_scope ==
                    take(fixture_cursor.finish()).completed_scope,
                "provider closure diverged");
    }
}
void wrapped_cursor_owns_run_and_snapshot() {
    auto reader = std::make_shared<FakeReader>();
    std::weak_ptr<FakeReader> weak_reader = reader;
    std::weak_ptr<ResourceAccount> weak_preparation;
    std::optional<SourceCursor> cursor;
    auto run = account();
    {
        SourceValidationServices validation{
            account(),
            {{16, 16, 0, 16}, 1024, 16384, 32768, 16384},
            MissingIntervalPolicy::Strict,
            {1048576, 1048576, 1048576, 1048576, 1048576, 1048576, 1048576}};
        weak_preparation = validation.account;
        auto registry = take(Registry::create());
        auto type = take(register_trade_type(registry, {}));
        auto resolver = take(SourceResolver::create(validation.account, 65536, 4));
        success(register_trade_replay_source(resolver, "checked-trades", type, reader, binding(),
                                             validation.account));
        success(resolver.freeze());
        SourceRequest request{"checked-trades",       "test-trades",         "v1",
                              trade_interpretation(), {"trade-clock", 0, 3}, {1, {}}};
        auto candidate = take(resolver.inspect(request, validation));
        auto snapshot = take(resolver.validate(std::move(candidate), request, validation));
        PublicationAllocator ephemeral_allocator{run, 0};
        cursor = take(snapshot.cursor(ephemeral_allocator));
    }
    reader.reset();
    require(weak_reader.expired(), "source snapshot retained its validation reader capability");
    require(!weak_preparation.expired(), "cursor did not pin the validated source data account");
    const auto timing = *cursor->descriptor();
    UpdateContext context{timing.key, timing.extent, timing.available_at_ns,
                          timing.available_at_ns};
    require(std::holds_alternative<SourceEvent>(cursor->next(context)),
            "cursor borrowed destroyed allocator/resolver/snapshot");
    cursor.reset();
    require(weak_preparation.expired(), "source preparation account leaked after cursor release");
}
} // namespace
int main() {
    const Case cases[]{
        {"checked capture and current-only delivery", checked_capture_and_current_delivery},
        {"invalid sources block admission", errors_block_admission},
        {"bounds and transport-independent identity", bounds_and_identity},
        {"explicit query and independent cursor lifetimes", explicit_query_and_cursor_lifetimes},
        {"selected versus requested time coverage", selected_coverage_is_not_query_coverage},
        {"fixture and checked reader share typed source publication",
         fixture_and_reader_share_source_publication},
        {"wrapped cursor owns run account and validated data",
         wrapped_cursor_owns_run_and_snapshot}};
    return lexongine::test::run(cases);
}
