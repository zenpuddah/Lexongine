// Checked-reader callback ownership and point-time interface probe.
// Full pipeline and persisted-acquisition tests are separate targets.
#include "quant/storage/backend.hpp"
#include "lexongine/binding.hpp"
#include "../../tests/lexongine_test_support.hpp"

#include <type_traits>

namespace {
using namespace lexongine;
using namespace lexongine::test;
using Reader = quant::storage::MarketDataReader;
using Bound = quant::integrity::BoundObservation;
using Requirements = quant::integrity::ConsumerRequirements;
using Fragments = std::span<const quant::storage::DataFragment>;
using Callback = quant::storage::SequencedEventConsumer;
static_assert(std::is_same_v<Callback, std::function<arrow::Status(const Bound&)>>);
static_assert(std::is_same_v<decltype(&Reader::stream_ordered_events),
              arrow::Status (Reader::*)(Fragments, Requirements, const Callback&) const>);

struct ReceiptPrototype {
    StepStatus status;
    std::optional<UpdateKey> key;
    std::optional<TemporalExtent> extent;
    std::optional<std::int64_t> available_at_ns;
    std::optional<SourceGap> gap;
    std::optional<SequenceScope> completed_scope;
    std::shared_ptr<const OutcomeFrame> current;
    std::optional<Diagnostic> cause;
};
static_assert(std::is_nothrow_move_constructible_v<ReceiptPrototype>);
static_assert(std::is_nothrow_copy_constructible_v<Diagnostic>);

quant::data::Trade trade(std::int64_t event_time, std::int64_t price) {
    using namespace quant::data;
    return {{InstrumentId{1}, VenueId{1}, Timestamp::from_unix_nanos(event_time),
             std::nullopt, std::nullopt, std::nullopt, 0, SourceId{1}}, Price::from_raw(price), Quantity::from_raw(1000000), std::nullopt};
}
class ProbeReader final : public Reader {
public:
    arrow::Result<quant::storage::DatasetLocalMetadata> read_metadata(const quant::storage::DataFragment&) const override {
        return arrow::Status::NotImplemented("probe metadata is not a validated snapshot");
    }
    arrow::Status read_batches(const quant::storage::DataFragment&, quant::storage::BatchProjection,
                              const quant::storage::BatchConsumer&) const override {
        return arrow::Status::Invalid("unchecked batch access must not be used");
    }
    arrow::Status stream_events(const quant::storage::DataFragment&, const quant::storage::EventConsumer&) const override {
        return arrow::Status::Invalid("unchecked replay must not be used");
    }
    arrow::Status stream_ordered_events(Fragments, Requirements requirements, const Callback& callback) const override {
        if (!requirements.ordering || !requirements.groups || requirements.initial_state ||
            requirements.allow_degraded || requirements.explicit_partial) {
            return arrow::Status::Invalid("explicit trade consumer requirements changed");
        }
        Bound transient{quant::data::StreamId{1}, 10, trade(100, 7)};
        ARROW_RETURN_NOT_OK(callback(transient));
        transient.ordinal = 12;
        transient.event = trade(95, 9); // Nonmonotonic event time, preserved succession.
        ARROW_RETURN_NOT_OK(callback(transient));
        transient.event = trade(999, 99); // Consumer must already own its copy.
        return arrow::Status::OK();
    }
};
void actual_reader_fit() {
    auto resources = take(ResourceAccount::create({{4096, 4096, 4096, 4096, 4096, 65536, 65536}, 8, 8, 32, 4}));
    // Known fixed prototype bound reserved before the callback's vector storage.
    auto reservation = take(resources->reserve(ResourceKind::Source, 2 * sizeof(Bound)));
    std::vector<Bound> retained;
    retained.reserve(2);
    ProbeReader reader;
    const auto status = reader.stream_ordered_events({}, {true, true, false, false, false}, [&](const Bound& row) {
        if (retained.size() == retained.capacity()) { return arrow::Status::CapacityError("bounded prototype is full"); }
        retained.push_back(row);
        return arrow::Status::OK();
    });
    require(status.ok(), "actual checked callback signature failed");
    require(retained.size() == 2 && retained[0].ordinal == 10 && retained[1].ordinal == 12, "source succession lost");
    require(std::get<quant::data::Trade>(retained[0].event).price == quant::data::Price::from_raw(7), "callback view escaped");
    require(quant::integrity::header(retained[1].event).event_time.unix_nanos() == 95, "event-time sort or fabricated interval");
    const UpdateContext activation{{"trade-clock", 1}, PointTime{95}, 111, 111};
    success(validate_context(activation)); // Availability is explicit host policy, not inferred from event time.
    auto early = activation; early.decision_time_ns = 110;
    rejects(validate_context(early), FailureCode::FutureRead);
}
}
int main() {
    const Case cases[]{{"actual MarketDataReader callback ownership and point-time fit", actual_reader_fit}};
    return lexongine::test::run(cases);
}
