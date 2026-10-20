#pragma once

#include "lexongine/source.hpp"
#include "quant/storage/backend.hpp"

namespace lexongine {
template <> struct PublicationTraits<quant::data::Trade> {
    using type = Immutable<quant::data::Trade>;
    static constexpr auto storage = StorageKind::ImmutableOwned;
};
} // namespace lexongine

namespace lexongine::qaedalus {

enum class ReplayDeliveryMode { AnalyticalChecked, OrderedEvent };
struct ReplayRequirements {
    bool ordering, groups, initial_state, allow_degraded, explicit_partial;
    ReplayRequirements(bool order, bool event_groups, bool initialized, bool degraded,
                       bool partial) noexcept
        : ordering(order), groups(event_groups), initial_state(initialized),
          allow_degraded(degraded), explicit_partial(partial) {}
    [[nodiscard]] quant::integrity::ConsumerRequirements canonical() const noexcept {
        return {ordering, groups, initial_state, allow_degraded, explicit_partial};
    }
};
struct OrdinalAvailability {
    std::uint64_t ordinal;
    std::int64_t available_at_ns;
};
struct ReplayMaterializationLimits {
    std::uint64_t max_records, decoded_bytes, metadata_bytes, fingerprint_bytes;
};
struct ReplaySourceBinding {
    quant::ingestion::MarketDataQuery original_query;
    std::vector<quant::storage::DataFragment> selected_fragments;
    ReplayRequirements consumer_requirements;
    ReplayDeliveryMode delivery_mode;
    quant::storage::DataKind payload_kind;
    std::string snapshot_name;
    SequenceScope expected_scope;
    Contract availability_contract;
    std::vector<OrdinalAvailability> availability;
    MissingIntervalPolicy missing_intervals;
    ReplayMaterializationLimits limits;
    OperationKey adapter_operation;
    ImplementationIdentity implementation;
    ModulePin pin;
};
struct ReplayStatistics {
    std::uint64_t scanned, selected, filtered;
};
struct TradeReplayRecord {
    Reservation delivery_charge;
    quant::data::Trade value;
    quant::data::StreamId stream_id;
    std::uint64_t acquisition_ordinal;
    TimingDescriptor timing;
};
using TradeReplayDelivery = std::variant<TradeReplayRecord, SourceEnd>;
class TradeReplayCursor {
  public:
    TradeReplayCursor(TradeReplayCursor &&) noexcept;
    TradeReplayCursor &operator=(TradeReplayCursor &&) noexcept;
    ~TradeReplayCursor() noexcept;
    TradeReplayCursor(const TradeReplayCursor &) = delete;
    TradeReplayCursor &operator=(const TradeReplayCursor &) = delete;
    // Host-only scheduling descriptor; observing it never advances delivery.
    [[nodiscard]] const TimingDescriptor *descriptor() const noexcept;
    [[nodiscard]] Result<TradeReplayDelivery> next(const UpdateContext &) noexcept;
    [[nodiscard]] Result<SourceEnd> finish() noexcept;

  private:
    friend class TradeReplayData;
    struct State;
    explicit TradeReplayCursor(std::unique_ptr<State>) noexcept;
    std::unique_ptr<State> state_;
};
class TradeReplayData {
  public:
    [[nodiscard]] const SourceMetadata &metadata() const noexcept;
    [[nodiscard]] const ReplayStatistics &statistics() const noexcept;
    [[nodiscard]] std::string_view acquisition_id() const noexcept;
    [[nodiscard]] const ReplaySourceBinding &binding() const noexcept;
    [[nodiscard]] const quant::storage::DatasetLocalMetadata &dataset_metadata() const noexcept;
    [[nodiscard]] std::span<const TimingDescriptor> timeline() const noexcept;
    [[nodiscard]] static std::uint64_t cursor_state_bytes() noexcept;
    [[nodiscard]] Result<TradeReplayCursor> cursor(std::shared_ptr<ResourceAccount>) const noexcept;

  private:
    struct Data;
    friend class TradeReplayCursor;
    friend Result<TradeReplayData>
    prepare_trade_replay(std::shared_ptr<const quant::storage::MarketDataReader>,
                         const ReplaySourceBinding &, std::shared_ptr<ResourceAccount>) noexcept;
    explicit TradeReplayData(std::shared_ptr<const Data> data) noexcept : data_(std::move(data)) {}
    std::shared_ptr<const Data> data_;
};

[[nodiscard]] TypeContract trade_interpretation();
[[nodiscard]] Result<TradeReplayData>
prepare_trade_replay(std::shared_ptr<const quant::storage::MarketDataReader>,
                     const ReplaySourceBinding &, std::shared_ptr<ResourceAccount>) noexcept;

[[nodiscard]] Result<RegisteredType<quant::data::Trade>> register_trade_type(Registry &,
                                                                             ModulePin) noexcept;
[[nodiscard]] Result<void> register_trade_replay_source(
    SourceResolver &, std::string provider_name, RegisteredType<quant::data::Trade>,
    std::shared_ptr<const quant::storage::MarketDataReader>, const ReplaySourceBinding &,
    std::shared_ptr<ResourceAccount> registration_account) noexcept;

} // namespace lexongine::qaedalus
