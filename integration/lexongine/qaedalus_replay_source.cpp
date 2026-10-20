#include "qaedalus_replay_source.hpp"

#include "quant/storage/file_integrity.hpp"

namespace lexongine::qaedalus {
namespace {
namespace q = quant;
struct SourceFailure {
    Diagnostic error;
};
[[noreturn]] void fail(FailureCode code, std::string_view message) {
    throw SourceFailure{diagnostic(code, message)};
}
template <class T> T checked(std::variant<T, Diagnostic> result) {
    if (const auto *error = std::get_if<Diagnostic>(&result)) {
        throw SourceFailure{*error};
    }
    return std::get<T>(std::move(result));
}
void checked(Result<void> result) {
    if (const auto *error = std::get_if<Diagnostic>(&result)) {
        throw SourceFailure{*error};
    }
}
void text_valid(std::string_view text, std::uint64_t maximum) {
    checked(validate_text(text, maximum, false));
}
Sha256 hash(std::span<const std::byte> bytes) {
    const auto hex = q::storage::files::sha256(bytes);
    Sha256 result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        unsigned value = 0;
        const auto parsed =
            std::from_chars(hex.data() + 2 * index, hex.data() + 2 * index + 2, value, 16);
        if (parsed.ec != std::errc{}) {
            fail(FailureCode::SourceInvalid, "internal source digest encoding failed");
        }
        result[index] = static_cast<std::byte>(value);
    }
    return result;
}
bool canonical_digest(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
class Fingerprint {
  public:
    explicit Fingerprint(std::uint64_t limit) : limit_(limit) {
        if (limit > bytes_.max_size()) {
            fail(FailureCode::ResourcePlanExceeded, "source fingerprint capacity overflow");
        }
        bytes_.reserve(static_cast<std::size_t>(limit));
    }
    void raw(std::span<const std::byte> bytes) {
        if (bytes.size() > limit_ - bytes_.size()) {
            fail(FailureCode::ResourcePlanExceeded, "source fingerprint byte limit exceeded");
        }
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    }
    void number(std::uint64_t value) {
        std::array<std::byte, 8> bytes{};
        for (unsigned i = 0; i < 8; ++i) {
            bytes[i] = static_cast<std::byte>((value >> ((7 - i) * 8)) & 255);
        }
        raw(bytes);
    }
    void text(std::string_view value) {
        number(value.size());
        raw(std::as_bytes(std::span{value.data(), value.size()}));
    }
    template <class T, class F> void optional(const std::optional<T> &value, F emit) {
        number(value.has_value());
        if (value) {
            emit(*value);
        }
    }
    void scope(const SequenceScope &value) {
        text(value.clock);
        number(value.begin);
        number(value.end);
    }
    void range(const q::ingestion::TimeRange &value) {
        number(static_cast<std::uint64_t>(value.start.unix_nanos()));
        number(static_cast<std::uint64_t>(value.end.unix_nanos()));
    }
    void contract(const Contract &value) {
        text(value.id);
        number(value.version);
        number(value.parameters.schema_version);
        number(value.parameters.values.size());
        std::string_view previous;
        for (const auto &parameter : value.parameters.values) {
            if (parameter.name.empty() ||
                (!previous.empty() && !unsigned_text_less(previous, parameter.name))) {
                fail(FailureCode::InvalidConfig, "source contract parameters are not canonical");
            }
            previous = parameter.name;
            text(parameter.name);
            number(parameter.value.index() + 1);
            std::visit(
                [&](const auto &item) {
                    if constexpr (std::is_same_v<std::decay_t<decltype(item)>, std::string>) {
                        text(item);
                    } else {
                        number(static_cast<std::uint64_t>(item));
                    }
                },
                parameter.value);
        }
    }
    void header(const q::data::EventHeader &value) {
        number(value.instrument_id.value());
        number(value.venue_id.value());
        number(value.source_id.value());
        number(static_cast<std::uint64_t>(value.event_time.unix_nanos()));
        optional(value.source_receive_time,
                 [&](auto time) { number(static_cast<std::uint64_t>(time.unix_nanos())); });
        optional(value.sequence, [&](auto sequence) { number(sequence); });
        optional(value.channel_id, [&](auto channel) { number(channel); });
        number(value.source_flags);
    }
    void binding(const ReplaySourceBinding &value,
                 const q::storage::DatasetLocalMetadata &metadata) {
        text(value.snapshot_name);
        scope(value.expected_scope);
        text("ordered_event");
        text("trades");
        text(value.missing_intervals == MissingIntervalPolicy::Strict ? "strict" : "skip");
        const auto &query = value.original_query;
        number(query.instrument_id.value());
        optional(query.venue_id, [&](auto id) { number(id.value()); });
        number(static_cast<unsigned>(query.level));
        optional(query.event_time_range, [&](auto value) { range(value); });
        optional(query.source_receive_time_range, [&](auto value) { range(value); });
        number(static_cast<unsigned>(query.primary_time_basis));
        number(static_cast<unsigned>(query.fetch_policy));
        optional(query.derivation_margin, [&](auto margin) {
            number(margin.before_nanos);
            number(margin.after_nanos);
        });
        const auto &requirements = value.consumer_requirements;
        number(requirements.ordering);
        number(requirements.groups);
        number(requirements.initial_state);
        number(requirements.allow_degraded);
        number(requirements.explicit_partial);
        number(value.selected_fragments.size());
        for (const auto &fragment : value.selected_fragments) {
            text(q::storage::data_kind_name(fragment.data_kind));
            number(fragment.stream.instrument_id.value());
            number(fragment.stream.venue_id.value());
            number(fragment.stream.source_id.value());
            range(fragment.time_range);
            text(fragment.schema_version);
            text(fragment.version_id);
            text(fragment.acquisition_id);
            number(fragment.row_stream_id->value());
            text(fragment.evidence_digest);
            // Mutable locators remain in the owned binding, not semantic identity.
        }
        number(static_cast<unsigned>(metadata.acquisition_mode));
        text(metadata.adapter_version);
        text(metadata.mapping_version);
        number(metadata.quality_references.size());
        for (const auto &reference : metadata.quality_references) {
            text(reference);
        }
        number(metadata.source_artifacts.size());
        for (const auto &source : metadata.source_artifacts) {
            text(source.artifact_identity);
            range(source.range);
        }
        optional(metadata.checksum, [&](const auto &checksum) { text(checksum); });
        contract(value.availability_contract);
        number(value.availability.size());
        for (const auto &entry : value.availability) {
            number(entry.ordinal);
            number(static_cast<std::uint64_t>(entry.available_at_ns));
        }
        text(value.adapter_operation.name_space);
        text(value.adapter_operation.name);
        number(value.adapter_operation.semantic_version);
        text(value.adapter_operation.implementation_selector);
        raw(value.implementation.binary_digest);
        raw(value.implementation.dependencies_digest);
        raw(value.implementation.source_build_digest);
        text(value.implementation.sdk_build_contract_id);
    }
    [[nodiscard]] Sha256 digest() const {
        return hash(bytes_);
    }
    void reset(std::string_view domain) {
        bytes_.clear();
        text(domain);
    }

  private:
    std::uint64_t limit_;
    std::vector<std::byte> bytes_;
};
std::uint64_t metadata_bound(const ReplaySourceBinding &binding) {
    auto clock_bytes = checked(checked_multiply(binding.expected_scope.clock.size() + 32, 2));
    auto count = checked(checked_multiply(
        binding.limits.max_records, sizeof(UpdateKey) + sizeof(TimingDescriptor) + clock_bytes));
    count = checked(checked_add(count, 8192));
    auto add = [&](std::string_view value) {
        count = checked(checked_add(count, value.size() + 32));
    };
    add(binding.snapshot_name);
    add(binding.expected_scope.clock);
    add(binding.availability_contract.id);
    add(binding.adapter_operation.name_space);
    add(binding.adapter_operation.name);
    add(binding.adapter_operation.implementation_selector);
    add(binding.implementation.sdk_build_contract_id);
    count = checked(
        checked_add(count, checked(type_storage_bytes(
                               {"source_policy", 1, binding.availability_contract.parameters}))));
    count = checked(checked_add(count, checked(checked_multiply(binding.availability.size(),
                                                                sizeof(OrdinalAvailability)))));
    for (const auto &fragment : binding.selected_fragments) {
        count = checked(checked_add(count, sizeof(fragment)));
        add(fragment.locator);
        add(fragment.version_id);
        add(fragment.acquisition_id);
        add(fragment.package_locator);
        add(fragment.schema_version);
        add(fragment.evidence_digest);
    }
    return count;
}
Contract policy(std::string name) {
    return {std::move(name), 1, {1, {}}};
}
struct CapturedTrade {
    q::data::Trade value;
    q::data::StreamId stream_id;
    std::uint64_t acquisition_ordinal;
};
} // namespace

struct TradeReplayData::Data {
    ModulePin pin;
    Reservation decoded_charge, metadata_charge;
    ReplaySourceBinding binding;
    q::storage::DatasetLocalMetadata dataset;
    SourceMetadata metadata;
    std::vector<CapturedTrade> records;
    std::vector<TimingDescriptor> timeline;
    ReplayStatistics statistics;
};
struct TradeReplayCursor::State {
    Reservation charge;
    std::shared_ptr<ResourceAccount> account;
    std::shared_ptr<const TradeReplayData::Data> data;
    std::size_t position = 0;
    bool closed = false;
};
TypeContract trade_interpretation() {
    return {"qaedalus.trade",
            1,
            {1,
             {{"price_scale", std::uint64_t{q::data::Price::canonical_scale}, Origin::VersionedFact,
               "Qaedalus"},
              {"quantity_scale", std::uint64_t{q::data::Quantity::canonical_scale},
               Origin::VersionedFact, "Qaedalus"},
              {"time_unit", std::string{"unix_ns"}, Origin::VersionedFact, "Qaedalus"}}}};
}
const SourceMetadata &TradeReplayData::metadata() const noexcept {
    return data_->metadata;
}
const ReplayStatistics &TradeReplayData::statistics() const noexcept {
    return data_->statistics;
}
std::string_view TradeReplayData::acquisition_id() const noexcept {
    return data_->binding.selected_fragments[0].acquisition_id;
}
const ReplaySourceBinding &TradeReplayData::binding() const noexcept {
    return data_->binding;
}
const q::storage::DatasetLocalMetadata &TradeReplayData::dataset_metadata() const noexcept {
    return data_->dataset;
}
std::span<const TimingDescriptor> TradeReplayData::timeline() const noexcept {
    return data_->timeline;
}
std::uint64_t TradeReplayData::cursor_state_bytes() noexcept {
    return sizeof(TradeReplayCursor::State) + 256;
}
Result<TradeReplayCursor>
TradeReplayData::cursor(std::shared_ptr<ResourceAccount> account) const noexcept {
    return guarded([&]() -> Result<TradeReplayCursor> {
        if (!account) {
            return diagnostic(FailureCode::InvalidConfig,
                              "source cursor needs an explicit account");
        }
        auto charge = account->reserve(ResourceKind::State, cursor_state_bytes());
        if (const auto *error = std::get_if<Diagnostic>(&charge)) {
            return *error;
        }
        return TradeReplayCursor{
            std::make_unique<TradeReplayCursor::State>(TradeReplayCursor::State{
                std::get<Reservation>(std::move(charge)), std::move(account), data_, 0, false})};
    });
}
TradeReplayCursor::TradeReplayCursor(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}
TradeReplayCursor::TradeReplayCursor(TradeReplayCursor &&) noexcept = default;
TradeReplayCursor &TradeReplayCursor::operator=(TradeReplayCursor &&) noexcept = default;
TradeReplayCursor::~TradeReplayCursor() noexcept = default;
const TimingDescriptor *TradeReplayCursor::descriptor() const noexcept {
    if (!state_ || state_->closed || state_->position == state_->data->records.size()) {
        return nullptr;
    }
    return &state_->data->timeline[state_->position];
}
Result<TradeReplayDelivery> TradeReplayCursor::next(const UpdateContext &context) noexcept {
    return guarded([&]() -> Result<TradeReplayDelivery> {
        if (!state_ || state_->closed) {
            return diagnostic(FailureCode::AlreadyExecuted, "source cursor is closed");
        }
        const auto *expected = descriptor();
        if (!expected || expected->key != context.key || expected->extent != context.extent ||
            expected->available_at_ns != context.activation_available_at_ns) {
            return diagnostic(FailureCode::UpdateProtocol,
                              "source delivery does not match the next declared activation");
        }
        auto valid = validate_context(context);
        if (const auto *error = std::get_if<Diagnostic>(&valid)) {
            return *error;
        }
        auto charge = state_->account->reserve(
            ResourceKind::Staging, sizeof(TradeReplayRecord) + 256 + expected->key.clock.size());
        if (const auto *error = std::get_if<Diagnostic>(&charge)) {
            return *error;
        }
        const auto &row = state_->data->records[state_->position];
        TradeReplayRecord delivery{std::get<Reservation>(std::move(charge)), row.value,
                                   row.stream_id, row.acquisition_ordinal, *expected};
        ++state_->position;
        return TradeReplayDelivery{std::move(delivery)};
    });
}
Result<SourceEnd> TradeReplayCursor::finish() noexcept {
    return guarded([&]() -> Result<SourceEnd> {
        if (!state_ || state_->closed) {
            return diagnostic(FailureCode::AlreadyExecuted, "source cursor is closed");
        }
        if (state_->position != state_->data->records.size()) {
            return diagnostic(FailureCode::SourceCoverage, "source scope not exhausted");
        }
        SourceEnd end{state_->data->metadata.validated_scope};
        state_->closed = true;
        return end;
    });
}

Result<TradeReplayData>
prepare_trade_replay(std::shared_ptr<const q::storage::MarketDataReader> reader,
                     const ReplaySourceBinding &binding,
                     std::shared_ptr<ResourceAccount> account) noexcept {
    return guarded([&]() -> Result<TradeReplayData> {
        try {
            if (!reader || !account) {
                fail(FailureCode::InvalidConfig, "checked replay requires explicit reader/account");
            }
            if (binding.delivery_mode != ReplayDeliveryMode::OrderedEvent ||
                binding.payload_kind != q::storage::DataKind::Trades ||
                binding.selected_fragments.size() != 1 ||
                binding.selected_fragments[0].data_kind != q::storage::DataKind::Trades) {
                fail(FailureCode::UnsupportedContract,
                     "first replay adapter supports one ordered Trade partition");
            }
            const auto &fragment = binding.selected_fragments[0];
            if (fragment.schema_version != q::storage::supported_encoding) {
                fail(FailureCode::UnsupportedContract, "unsupported replay encoding");
            }
            q::storage::validate(fragment);
            q::ingestion::validate(binding.original_query);
            const auto &query = binding.original_query;
            if ((query.level != q::ingestion::MarketDataLevel::L1 &&
                 query.level != q::ingestion::MarketDataLevel::L2 &&
                 query.level != q::ingestion::MarketDataLevel::L3) ||
                (query.primary_time_basis != q::ingestion::TimeBasis::EventTime &&
                 query.primary_time_basis != q::ingestion::TimeBasis::SourceReceiveTime) ||
                (query.fetch_policy != q::ingestion::FetchPolicy::DeriveProviderRange &&
                 query.fetch_policy != q::ingestion::FetchPolicy::RequireExplicitRange)) {
                fail(FailureCode::UnsupportedContract, "unsupported canonical query policy");
            }
            const auto &requirements = binding.consumer_requirements;
            if (!requirements.ordering || !requirements.groups) {
                fail(FailureCode::UnsupportedContract,
                     "ordered source requires explicit ordering/group checks");
            }
            if (!query.event_time_range) {
                fail(FailureCode::UnsupportedContract,
                     "this bounded replay source needs declared event-time coverage evidence");
            }
            if (!requirements.explicit_partial &&
                (query.event_time_range->start < fragment.time_range.start ||
                 query.event_time_range->end > fragment.time_range.end)) {
                fail(FailureCode::SourceCoverage,
                     "selected fragment does not cover the full requested event-time range");
            }
            if (binding.missing_intervals != MissingIntervalPolicy::Strict &&
                binding.missing_intervals != MissingIntervalPolicy::Skip) {
                fail(FailureCode::UnsupportedContract, "unsupported source missing policy");
            }
            if (binding.expected_scope.end < binding.expected_scope.begin) {
                fail(FailureCode::SourceCoverage, "reversed source scope");
            }
            if (binding.expected_scope.end - binding.expected_scope.begin >
                    binding.limits.max_records ||
                binding.availability.size() > binding.limits.max_records) {
                fail(FailureCode::ResourcePlanExceeded,
                     "source scope/timing table exceeds record limit");
            }
            if (binding.availability_contract.id != "qaedalus.ordinal_availability_table" ||
                binding.availability_contract.version != 1 ||
                binding.availability_contract.parameters.schema_version != 1 ||
                !binding.availability_contract.parameters.values.empty()) {
                fail(FailureCode::UnsupportedContract,
                     "explicit ordinal availability-table contract required");
            }
            for (std::size_t i = 1; i < binding.availability.size(); ++i) {
                if (binding.availability[i - 1].ordinal >= binding.availability[i].ordinal) {
                    fail(FailureCode::SourceCoverage,
                         "availability ordinals are not unique/ordered");
                }
            }
            if (fragment.version_id.empty() || fragment.acquisition_id.empty() ||
                fragment.package_locator.empty() || !fragment.row_stream_id ||
                !canonical_digest(fragment.evidence_digest) ||
                binding.implementation.sdk_build_contract_id.empty() ||
                binding.implementation.binary_digest == Sha256{} ||
                binding.implementation.dependencies_digest == Sha256{} ||
                binding.implementation.source_build_digest == Sha256{}) {
                fail(FailureCode::SourceInvalid,
                     "replay requires exact version/reference/evidence/build identity");
            }
            if (fragment.stream.instrument_id != binding.original_query.instrument_id ||
                (binding.original_query.venue_id &&
                 *binding.original_query.venue_id != fragment.stream.venue_id)) {
                fail(FailureCode::SourceInvalid, "selected fragment disagrees with query identity");
            }
            text_valid(binding.snapshot_name, binding.limits.metadata_bytes);
            text_valid(binding.expected_scope.clock, binding.limits.metadata_bytes);
            text_valid(binding.adapter_operation.name_space, binding.limits.metadata_bytes);
            text_valid(binding.adapter_operation.name, binding.limits.metadata_bytes);
            text_valid(binding.adapter_operation.implementation_selector,
                       binding.limits.metadata_bytes);
            if (binding.adapter_operation.semantic_version == 0) {
                fail(FailureCode::SourceInvalid, "adapter semantic version is unresolved");
            }
            const auto required_metadata = metadata_bound(binding);
            if (required_metadata > binding.limits.metadata_bytes) {
                fail(FailureCode::ResourcePlanExceeded, "source metadata exceeds explicit bound");
            }
            const auto decoded_bytes =
                checked(checked_multiply(binding.limits.max_records, sizeof(CapturedTrade)));
            if (decoded_bytes > binding.limits.decoded_bytes ||
                binding.limits.max_records > std::vector<CapturedTrade>{}.max_size()) {
                fail(FailureCode::ResourcePlanExceeded,
                     "decoded replay slice exceeds explicit bound");
            }
            auto metadata_charge =
                checked(account->reserve(ResourceKind::Graph, binding.limits.metadata_bytes));
            auto decoded_charge = checked(account->reserve(ResourceKind::Source, decoded_bytes));
            auto scratch = checked(account->reserve(
                ResourceKind::Scratch,
                checked(checked_add(checked(checked_multiply(binding.limits.fingerprint_bytes, 3)),
                                    4096))));
            auto metadata_result = reader->read_metadata(fragment);
            if (!metadata_result.ok()) {
                fail(FailureCode::SourceInvalid, metadata_result.status().message());
            }
            auto metadata = std::move(metadata_result).ValueOrDie();
            q::storage::validate(metadata);
            if (metadata.data_kind != fragment.data_kind || metadata.stream != fragment.stream ||
                metadata.time_range != fragment.time_range ||
                metadata.schema_version != fragment.schema_version ||
                metadata.adapter_version.empty() || metadata.mapping_version.empty()) {
                fail(FailureCode::SourceChanged,
                     "reader metadata disagrees with the pinned fragment");
            }
            if (metadata.acquisition_mode != q::ingestion::AcquisitionMode::Historical &&
                metadata.acquisition_mode != q::ingestion::AcquisitionMode::Live) {
                fail(FailureCode::UnsupportedContract,
                     "unknown acquisition mode in reader metadata");
            }
            auto metadata_size = required_metadata;
            for (const auto &value : metadata.quality_references) {
                metadata_size =
                    checked(checked_add(metadata_size, sizeof(std::string) + value.size() + 32));
            }
            for (const auto &value : metadata.source_artifacts) {
                metadata_size = checked(checked_add(
                    metadata_size, sizeof(value) + value.artifact_identity.size() + 32));
            }
            metadata_size = checked(checked_add(
                metadata_size, metadata.adapter_version.size() + metadata.mapping_version.size() +
                                   (metadata.checksum ? metadata.checksum->size() : 0)));
            if (metadata_size > binding.limits.metadata_bytes) {
                fail(FailureCode::ResourcePlanExceeded, "reader evidence exceeds metadata bound");
            }
            std::vector<CapturedTrade> records;
            records.reserve(static_cast<std::size_t>(binding.limits.max_records));
            std::vector<TimingDescriptor> timeline;
            timeline.reserve(static_cast<std::size_t>(binding.limits.max_records));
            ReplayStatistics statistics{0, 0, 0};
            std::optional<std::uint64_t> previous_ordinal;
            std::optional<Diagnostic> callback_failure;
            const auto status = reader->stream_ordered_events(
                binding.selected_fragments, requirements.canonical(),
                [&](const q::integrity::BoundObservation &row) {
                    if (callback_failure) {
                        return arrow::Status::Invalid("source candidate already failed");
                    }
                    const auto captured = guarded([&]() -> Result<void> {
                        if (statistics.scanned == binding.limits.max_records) {
                            return diagnostic(FailureCode::ResourcePlanExceeded,
                                              "replay scan/materialization limit exceeded");
                        }
                        ++statistics.scanned;
                        const auto *trade = std::get_if<q::data::Trade>(&row.event);
                        if (!trade || row.stream_id != *fragment.row_stream_id ||
                            trade->header.instrument_id != fragment.stream.instrument_id ||
                            trade->header.venue_id != fragment.stream.venue_id ||
                            trade->header.source_id != fragment.stream.source_id ||
                            !q::data::validate(*trade).empty() ||
                            !q::ingestion::contains(fragment.time_range,
                                                    trade->header.event_time) ||
                            (previous_ordinal && row.ordinal <= *previous_ordinal)) {
                            return diagnostic(
                                FailureCode::SourceInvalid,
                                "invalid replay payload, stream, range or ordinal association");
                        }
                        previous_ordinal = row.ordinal;
                        if (binding.original_query.source_receive_time_range &&
                            !trade->header.source_receive_time) {
                            return diagnostic(FailureCode::SourceInvalid,
                                              "required source-receive time is absent");
                        }
                        if (!q::ingestion::matches_time(binding.original_query,
                                                        trade->header.event_time,
                                                        trade->header.source_receive_time)) {
                            ++statistics.filtered;
                            return ok();
                        }
                        const auto available = std::lower_bound(
                            binding.availability.begin(), binding.availability.end(), row.ordinal,
                            [](const auto &entry, std::uint64_t ordinal) {
                                return entry.ordinal < ordinal;
                            });
                        if (available == binding.availability.end() ||
                            available->ordinal != row.ordinal) {
                            return diagnostic(FailureCode::SourceCoverage,
                                              "selected row has no explicit availability");
                        }
                        if (statistics.selected >=
                            binding.expected_scope.end - binding.expected_scope.begin) {
                            return diagnostic(FailureCode::SourceCoverage,
                                              "selected rows exceed the expected source scope");
                        }
                        records.push_back({*trade, row.stream_id, row.ordinal});
                        timeline.push_back({{binding.expected_scope.clock,
                                             binding.expected_scope.begin + statistics.selected},
                                            PointTime{trade->header.event_time.unix_nanos()},
                                            available->available_at_ns,
                                            CompleteRecord{statistics.selected}});
                        ++statistics.selected;
                        return ok();
                    });
                    if (const auto *error = std::get_if<Diagnostic>(&captured)) {
                        callback_failure = *error;
                        return arrow::Status::Invalid("source admission rejected callback data");
                    }
                    return arrow::Status::OK();
                });
            if (callback_failure) {
                return *callback_failure;
            }
            if (!status.ok()) {
                fail(status.IsNotImplemented() ? FailureCode::UnsupportedContract
                                               : FailureCode::SourceInvalid,
                     status.message());
            }
            if (statistics.selected != binding.expected_scope.end - binding.expected_scope.begin) {
                fail(FailureCode::SourceCoverage,
                     "checked replay did not close its expected scope");
            }
            SourceCoverage coverage{
                binding.expected_scope, {}, {}, policy("qaedalus.selected_trade_succession")};
            coverage.complete_keys.reserve(records.size());
            for (const auto &timing : timeline) {
                coverage.complete_keys.push_back(timing.key);
            }
            static_cast<void>(checked(validate_source_timeline(
                coverage, timeline, records.size(), binding.missing_intervals,
                {binding.limits.max_records, binding.limits.max_records, 0,
                 binding.limits.max_records})));
            Fingerprint fingerprint{binding.limits.fingerprint_bytes};
            fingerprint.reset("qaedalus.trade-replay-content.v1");
            fingerprint.binding(binding, metadata);
            fingerprint.number(records.size());
            for (std::size_t index = 0; index < records.size(); ++index) {
                const auto &record = records[index];
                const auto &timing = timeline[index];
                fingerprint.number(record.stream_id.value());
                fingerprint.number(record.acquisition_ordinal);
                fingerprint.header(record.value.header);
                fingerprint.number(static_cast<std::uint64_t>(record.value.price.raw()));
                fingerprint.number(record.value.quantity.raw());
                fingerprint.optional(record.value.aggressor_side, [&](auto side) {
                    fingerprint.number(static_cast<unsigned>(side));
                });
                fingerprint.text(timing.key.clock);
                fingerprint.number(timing.key.sequence);
                fingerprint.number(static_cast<std::uint64_t>(timing.available_at_ns));
            }
            const auto content = fingerprint.digest();
            fingerprint.reset("qaedalus.trade-replay-associations.v1");
            fingerprint.binding(binding, metadata);
            fingerprint.number(records.size());
            for (std::size_t index = 0; index < records.size(); ++index) {
                const auto &record = records[index];
                fingerprint.number(timeline[index].key.sequence);
                fingerprint.number(record.stream_id.value());
                fingerprint.number(record.acquisition_ordinal);
                fingerprint.header(record.value.header);
            }
            SourceMetadata source{binding.snapshot_name,
                                  fragment.version_id,
                                  content,
                                  trade_interpretation(),
                                  {binding.pin, std::type_index(typeid(q::data::Trade))},
                                  binding.expected_scope,
                                  std::move(coverage),
                                  policy("point_time"),
                                  policy("qaedalus.checked_acquisition_succession"),
                                  binding.availability_contract,
                                  policy("qaedalus.checked_trade_replay_v1"),
                                  fingerprint.digest(),
                                  binding.implementation};
            return TradeReplayData{std::make_shared<const TradeReplayData::Data>(
                TradeReplayData::Data{binding.pin, std::move(decoded_charge),
                                      std::move(metadata_charge), binding, std::move(metadata),
                                      std::move(source), std::move(records), std::move(timeline),
                                      statistics})};
        } catch (const SourceFailure &failure) {
            return failure.error;
        } catch (const std::invalid_argument &error) {
            return diagnostic(FailureCode::SourceInvalid, error.what());
        }
    });
}

Result<RegisteredType<q::data::Trade>> register_trade_type(Registry &registry,
                                                           ModulePin pin) noexcept {
    return guarded([&]() -> Result<RegisteredType<q::data::Trade>> {
        return registry.bind_type<q::data::Trade>(
            {trade_interpretation(), StorageKind::ImmutableOwned,
             [](const q::data::Trade &value) -> Result<void> {
                 if (!q::data::validate(value).empty()) {
                     return diagnostic(FailureCode::SourceInvalid,
                                       "invalid canonical Trade publication");
                 }
                 return ok();
             },
             [](const q::data::Trade &) -> Result<std::uint64_t> { return sizeof(q::data::Trade); },
             FreezeStrategy::DeepCopy, [](const q::data::Trade &value) { return value; },
             std::move(pin)});
    });
}
Result<void> register_trade_replay_source(
    SourceResolver &resolver, std::string name, RegisteredType<q::data::Trade> type,
    std::shared_ptr<const q::storage::MarketDataReader> reader, const ReplaySourceBinding &binding,
    std::shared_ptr<ResourceAccount> account) noexcept {
    return guarded([&]() -> Result<void> {
        try {
            if (!reader || !account) {
                fail(FailureCode::InvalidConfig,
                     "Trade source registration requires explicit reader/account");
            }
            if (!(type.policy.contract == trade_interpretation())) {
                fail(FailureCode::TypeMismatch, "Trade provider interpretation mismatch");
            }
            const auto bytes = checked(checked_add(metadata_bound(binding), 1024));
            if (bytes > binding.limits.metadata_bytes) {
                fail(FailureCode::ResourcePlanExceeded,
                     "Trade provider binding exceeds registration bound");
            }
            auto charge =
                checked(account->reserve(ResourceKind::Graph, binding.limits.metadata_bytes));
            struct Input {
                Reservation charge;
                std::shared_ptr<const q::storage::MarketDataReader> reader;
                ReplaySourceBinding binding;
            };
            using Inspection = std::shared_ptr<const Input>;
            auto input =
                std::make_shared<const Input>(Input{std::move(charge), std::move(reader), binding});
            TypedSourceProvider<q::data::Trade, Inspection, TradeReplayData, TradeReplayCursor>
                provider{
                    std::move(name),
                    std::move(type),
                    [input](const SourceRequest &request,
                            SourceValidationServices &services) -> Result<Inspection> {
                        const auto &bound = input->binding;
                        if (bound.selected_fragments.size() != 1 ||
                            bound.payload_kind != q::storage::DataKind::Trades ||
                            bound.delivery_mode != ReplayDeliveryMode::OrderedEvent) {
                            return diagnostic(FailureCode::UnsupportedContract,
                                              "unsupported typed Trade binding");
                        }
                        if (request.snapshot_name != bound.snapshot_name ||
                            request.requested_version != bound.selected_fragments[0].version_id ||
                            request.required_scope != bound.expected_scope ||
                            services.missing_intervals != bound.missing_intervals) {
                            return diagnostic(
                                FailureCode::SourceChanged,
                                "source request differs from explicitly registered Trade binding");
                        }
                        if (request.provider_policies.schema_version != 1 ||
                            !request.provider_policies.values.empty()) {
                            return diagnostic(FailureCode::UnsupportedContract,
                                              "Trade binding has no request-time policy overrides");
                        }
                        return input;
                    },
                    [](const Inspection &input) -> Result<SourceBounds> {
                        auto scratch = checked_multiply(input->binding.limits.fingerprint_bytes, 3);
                        if (const auto *error = std::get_if<Diagnostic>(&scratch)) {
                            return *error;
                        }
                        scratch = checked_add(std::get<std::uint64_t>(scratch), 4096);
                        if (const auto *error = std::get_if<Diagnostic>(&scratch)) {
                            return *error;
                        }
                        return SourceBounds{input->binding.expected_scope,
                                            {input->binding.limits.decoded_bytes,
                                             TradeReplayData::cursor_state_bytes(),
                                             std::get<std::uint64_t>(scratch),
                                             sizeof(TradeReplayRecord) + 512,
                                             sizeof(q::data::Trade) + 512,
                                             input->binding.limits.metadata_bytes, 0}};
                    },
                    [](Inspection &&input, const SourceRequest &,
                       SourceValidationServices &services) -> Result<TradeReplayData> {
                        return prepare_trade_replay(input->reader, input->binding,
                                                    services.account);
                    },
                    [](const TradeReplayData &data) -> Result<SourceMetadata> {
                        return data.metadata();
                    },
                    [](const TradeReplayData &data) -> Result<std::span<const TimingDescriptor>> {
                        return data.timeline();
                    },
                    [](const TradeReplayData &data, SourceCursorServices &services)
                        -> Result<TradeReplayCursor> { return data.cursor(services.account); },
                    [](TradeReplayCursor &cursor, const TradeReplayData &,
                       const UpdateContext &context,
                       PublicationAllocator &allocator) -> Result<SourceDelivery<q::data::Trade>> {
                        auto result = cursor.next(context);
                        if (const auto *error = std::get_if<Diagnostic>(&result)) {
                            return *error;
                        }
                        auto &delivery = std::get<TradeReplayDelivery>(result);
                        if (const auto *end = std::get_if<SourceEnd>(&delivery)) {
                            return SourceDelivery<q::data::Trade>{*end};
                        }
                        auto &row = std::get<TradeReplayRecord>(delivery);
                        auto value = allocator.copy_output(0, row.value);
                        if (const auto *error = std::get_if<Diagnostic>(&value)) {
                            return *error;
                        }
                        return SourceDelivery<q::data::Trade>{SourceRecord<q::data::Trade>{
                            std::move(row.timing),
                            std::get<Immutable<q::data::Trade>>(std::move(value))}};
                    },
                    [](TradeReplayCursor &cursor, const TradeReplayData &) -> Result<SourceEnd> {
                        return cursor.finish();
                    },
                    binding.pin};
            return resolver.register_provider(std::move(provider));
        } catch (const SourceFailure &failure) {
            return failure.error;
        }
    });
}

} // namespace lexongine::qaedalus
