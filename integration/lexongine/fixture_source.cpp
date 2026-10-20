#include "fixture_source.hpp"
#include "fixture.hpp"
#include "quant/storage/file_integrity.hpp"

namespace lexongine::fixture {
namespace {
using quant::data::Bar;
struct FixtureFailure {
    Diagnostic error;
};
template <class T> T checked(std::variant<T, Diagnostic> result) {
    if (const auto *error = std::get_if<Diagnostic>(&result)) {
        throw FixtureFailure{*error};
    }
    return std::get<T>(std::move(result));
}
void checked(Result<void> result) {
    if (const auto *error = std::get_if<Diagnostic>(&result)) {
        throw FixtureFailure{*error};
    }
}
template <class F> auto safe(F &&call) noexcept -> decltype(call()) {
    return guarded([&]() -> decltype(call()) {
        try {
            return call();
        } catch (const FixtureFailure &failure) {
            return failure.error;
        }
    });
}
Contract contract(std::string name) {
    return {std::move(name), 1, {1, {}}};
}
CanonicalParams policies(MissingIntervalPolicy missing) {
    return {1,
            {{"availability", std::string{"interval_end_zero_lag_fixture"}, Origin::ExplicitConfig,
              "source"},
             {"completeness", std::string{"complete_only"}, Origin::ExplicitConfig, "source"},
             {"empty", std::string{"reject"}, Origin::ExplicitConfig, "source"},
             {"missing", std::string{"fail"}, Origin::ExplicitConfig, "source"},
             {"missing_intervals",
              std::string{missing == MissingIntervalPolicy::Strict ? "strict" : "skip"},
              Origin::ExplicitConfig, "source"}}};
}
OperationKey key() {
    return {"qaedalus", "fixture_bars", 1, "fixture-generator-v1"};
}
// Provider-specific field framing; this is not the computation/artifact codec.
class Encoder {
  public:
    explicit Encoder(std::uint64_t maximum) : maximum_(maximum) {
        if (maximum > bytes_.max_size()) {
            throw FixtureFailure{diagnostic(FailureCode::ResourcePlanExceeded,
                                            "fixture fingerprint capacity overflow")};
        }
        bytes_.reserve(static_cast<std::size_t>(maximum));
    }
    void raw(std::span<const std::byte> value) {
        if (value.size() > maximum_ - bytes_.size()) {
            throw FixtureFailure{diagnostic(FailureCode::ResourcePlanExceeded,
                                            "fixture fingerprint byte ceiling exceeded")};
        }
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    void number(std::uint64_t value) {
        std::array<std::byte, 8> bytes{};
        for (unsigned i = 0; i < 8; ++i) {
            bytes[i] = static_cast<std::byte>(value >> ((7 - i) * 8));
        }
        raw(bytes);
    }
    void domain(std::string_view name) {
        bytes_.clear();
        number(name.size());
        raw(std::as_bytes(std::span{name.data(), name.size()}));
    }
    std::size_t begin(std::uint16_t tag) {
        const std::array<std::byte, 2> tags{static_cast<std::byte>(tag >> 8),
                                            static_cast<std::byte>(tag & 255)};
        raw(tags);
        const auto position = bytes_.size();
        number(0);
        return position;
    }
    void end(std::size_t position) noexcept {
        const auto length = static_cast<std::uint64_t>(bytes_.size() - position - 8);
        for (unsigned i = 0; i < 8; ++i) {
            bytes_[position + i] = static_cast<std::byte>(length >> ((7 - i) * 8));
        }
    }
    void field(std::uint16_t tag, std::uint64_t value) {
        const auto start = begin(tag);
        number(value);
        end(start);
    }
    void text(std::uint16_t tag, std::string_view value) {
        const auto start = begin(tag);
        raw(std::as_bytes(std::span{value.data(), value.size()}));
        end(start);
    }
    void bar(const Bar &value) {
        const auto row = begin(1);
        const auto &h = value.header;
        field(1, h.instrument_id.value());
        field(2, h.venue_id.value());
        field(3, h.source_id.value());
        field(4, static_cast<std::uint64_t>(h.event_time.unix_nanos()));
        field(5, h.source_receive_time.has_value());
        if (h.source_receive_time) {
            field(6, static_cast<std::uint64_t>(h.source_receive_time->unix_nanos()));
        }
        field(7, h.sequence.has_value());
        if (h.sequence) {
            field(8, *h.sequence);
        }
        field(9, h.channel_id.has_value());
        if (h.channel_id) {
            field(10, *h.channel_id);
        }
        field(11, h.source_flags);
        field(12, static_cast<std::uint64_t>(value.interval_start.unix_nanos()));
        field(13, static_cast<std::uint64_t>(value.interval_end.unix_nanos()));
        field(14, static_cast<std::uint64_t>(value.open.raw()));
        field(15, static_cast<std::uint64_t>(value.high.raw()));
        field(16, static_cast<std::uint64_t>(value.low.raw()));
        field(17, static_cast<std::uint64_t>(value.close.raw()));
        field(18, value.volume.raw());
        end(row);
    }
    Sha256 digest() const {
        const auto hex = quant::storage::files::sha256(bytes_);
        Sha256 result{};
        for (std::size_t i = 0; i < result.size(); ++i) {
            unsigned value = 0;
            const auto parsed =
                std::from_chars(hex.data() + 2 * i, hex.data() + 2 * i + 2, value, 16);
            if (parsed.ec != std::errc{}) {
                throw FixtureFailure{
                    diagnostic(FailureCode::SourceInvalid, "fixture digest encoding failed")};
            }
            result[i] = static_cast<std::byte>(value);
        }
        return result;
    }

  private:
    std::uint64_t maximum_;
    std::vector<std::byte> bytes_;
};
struct Inspection {
    SequenceScope scope;
};
struct Data {
    Reservation rows_charge, metadata_charge;
    std::vector<Bar> rows;
    SourceMetadata metadata;
    std::vector<TimingDescriptor> timeline;
};
struct Cursor {
    std::size_t position = 0;
};
} // namespace

Result<SourceRequest> source_request(const SourceDeclaration &source) noexcept {
    return safe([&]() -> Result<SourceRequest> {
        if (source.operation != key() || source.completeness != "complete_only" ||
            source.empty != "reject" || source.missing != "fail" ||
            source.availability != "interval_end_zero_lag_fixture" ||
            !source.parameters.values.empty() ||
            (source.missing_intervals != MissingIntervalPolicy::Strict &&
             source.missing_intervals != MissingIntervalPolicy::Skip)) {
            return diagnostic(FailureCode::UnsupportedContract,
                              "unsupported explicit fixture source contract");
        }
        return SourceRequest{"qaedalus.fixture_bars",
                             std::string{snapshot_name},
                             source.source_version,
                             quant::lexongine::bar_contract(),
                             source.scope,
                             policies(source.missing_intervals)};
    });
}
Result<void> register_source(Registry &registry, SourceResolver &resolver, RegisteredType<Bar> type,
                             const quant::lexongine::ComponentRegistration &build,
                             SourceLimits limits) noexcept {
    return safe([&]() -> Result<void> {
        if (build.implementation.binary_digest == Sha256{} ||
            build.implementation.dependencies_digest == Sha256{} ||
            build.implementation.source_build_digest == Sha256{} ||
            build.implementation.sdk_build_contract_id.empty() ||
            (build.missing_intervals != MissingIntervalPolicy::Strict &&
             build.missing_intervals != MissingIntervalPolicy::Skip)) {
            return diagnostic(
                FailureCode::InvalidConfig,
                "fixture registration requires explicit build identity and missing policy");
        }
        if (!(type.policy.contract == quant::lexongine::bar_contract())) {
            return diagnostic(FailureCode::TypeMismatch, "fixture Bar interpretation mismatch");
        }
        const auto row_bytes = checked(checked_multiply(bar_count, sizeof(Bar)));
        const auto metadata_bytes = checked(
            checked_add(16384, checked(checked_multiply(bar_count, sizeof(TimingDescriptor) +
                                                                       sizeof(UpdateKey) + 128))));
        const auto scratch_bytes =
            checked(checked_add(4096, checked(checked_multiply(limits.fingerprint_bytes, 3))));
        if (limits.max_records < bar_count || limits.decoded_bytes < row_bytes ||
            limits.metadata_bytes < metadata_bytes) {
            return diagnostic(FailureCode::ResourcePlanExceeded,
                              "fixture fixed snapshot exceeds materialization ceilings");
        }
        TypedSourceProvider<Bar, Inspection, Data, Cursor> provider{
            "qaedalus.fixture_bars",
            type,
            [build](const SourceRequest &request,
                    SourceValidationServices &services) -> Result<Inspection> {
                if (request.snapshot_name != snapshot_name ||
                    request.requested_version != snapshot_name) {
                    return diagnostic(FailureCode::SourceChanged,
                                      "fixture snapshot/version is unresolved");
                }
                if (request.required_scope.clock != clock_name ||
                    request.required_scope.end > bar_count ||
                    request.required_scope.begin >= request.required_scope.end) {
                    return diagnostic(FailureCode::SourceCoverage,
                                      "fixture request is empty or outside its fixed snapshot");
                }
                if (services.missing_intervals != build.missing_intervals ||
                    !(request.provider_policies == policies(build.missing_intervals))) {
                    return diagnostic(FailureCode::UnsupportedContract,
                                      "fixture request policy differs from registered contract");
                }
                return Inspection{request.required_scope};
            },
            [row_bytes, metadata_bytes, scratch_bytes](const Inspection &) -> Result<SourceBounds> {
                return SourceBounds{
                    {std::string{clock_name}, 0, bar_count},
                    {row_bytes, 512, scratch_bytes, 8192, sizeof(Bar) + 512, metadata_bytes, 0}};
            },
            [build, limits, row_bytes, metadata_bytes,
             scratch_bytes](Inspection &&inspection, const SourceRequest &,
                            SourceValidationServices &services) -> Result<Data> {
                return safe([&]() -> Result<Data> {
                    auto row_charge =
                        checked(services.account->reserve(ResourceKind::Source, row_bytes));
                    auto metadata_charge =
                        checked(services.account->reserve(ResourceKind::Graph, metadata_bytes));
                    auto scratch =
                        checked(services.account->reserve(ResourceKind::Scratch, scratch_bytes));
                    std::vector<Bar> rows;
                    rows.reserve(bar_count);
                    Encoder encoder{limits.fingerprint_bytes};
                    encoder.domain("qaedalus.fixture-bars.content.v1");
                    encoder.text(1, snapshot_name);
                    encoder.text(2, clock_name);
                    encoder.field(3, bar_count);
                    const auto list = encoder.begin(4);
                    encoder.number(bar_count);
                    for (std::uint64_t index = 0; index < bar_count; ++index) {
                        auto value = checked(complete_bar(index));
                        if (!quant::data::validate(value).empty()) {
                            return diagnostic(FailureCode::SourceInvalid,
                                              "generated fixture Bar is not canonical");
                        }
                        encoder.bar(value);
                        rows.push_back(std::move(value));
                    }
                    encoder.end(list);
                    const auto content = encoder.digest();
                    encoder.domain("qaedalus.fixture-bars.references.v1");
                    encoder.text(1, snapshot_name);
                    encoder.text(2, clock_name);
                    const auto reference_content = encoder.begin(3);
                    encoder.raw(content);
                    encoder.end(reference_content);
                    encoder.field(4, 0);
                    encoder.field(5, bar_count);
                    encoder.field(6, static_cast<std::uint64_t>(interval_ns));
                    const auto references = encoder.digest();
                    SourceCoverage coverage{
                        inspection.scope, {}, {}, contract("qaedalus.fixture_complete_intervals")};
                    const auto count =
                        static_cast<std::size_t>(inspection.scope.end - inspection.scope.begin);
                    coverage.complete_keys.reserve(count);
                    std::vector<TimingDescriptor> timeline;
                    timeline.reserve(count);
                    for (auto key = inspection.scope.begin; key < inspection.scope.end; ++key) {
                        const auto &value = rows[static_cast<std::size_t>(key)];
                        coverage.complete_keys.push_back({std::string{clock_name}, key});
                        timeline.push_back({{std::string{clock_name}, key},
                                            Interval{value.interval_start.unix_nanos(),
                                                     value.interval_end.unix_nanos()},
                                            value.interval_end.unix_nanos(),
                                            CompleteRecord{key - inspection.scope.begin}});
                    }
                    SourceMetadata metadata{std::string{snapshot_name},
                                            std::string{snapshot_name},
                                            content,
                                            quant::lexongine::bar_contract(),
                                            {build.pin, typeid(Bar)},
                                            inspection.scope,
                                            std::move(coverage),
                                            contract("interval_v1"),
                                            contract("qaedalus.fixture_original_keys"),
                                            contract("interval_end_zero_lag_fixture"),
                                            contract("qaedalus.fixture_complete_bars_v1"),
                                            references,
                                            build.implementation};
                    return Data{std::move(row_charge), std::move(metadata_charge), std::move(rows),
                                std::move(metadata), std::move(timeline)};
                });
            },
            [](const Data &data) -> Result<SourceMetadata> { return data.metadata; },
            [](const Data &data) -> Result<std::span<const TimingDescriptor>> {
                return std::span<const TimingDescriptor>{data.timeline};
            },
            [](const Data &, SourceCursorServices &) -> Result<Cursor> { return Cursor{}; },
            [](Cursor &cursor, const Data &data, const UpdateContext &,
               PublicationAllocator &allocator) -> Result<SourceDelivery<Bar>> {
                if (cursor.position >= data.timeline.size()) {
                    return diagnostic(FailureCode::SourceCoverage, "fixture cursor is exhausted");
                }
                const auto index =
                    static_cast<std::size_t>(data.metadata.validated_scope.begin) + cursor.position;
                auto value = allocator.copy_output(0, data.rows[index]);
                if (const auto *error = std::get_if<Diagnostic>(&value)) {
                    return *error;
                }
                SourceRecord<Bar> record{data.timeline[cursor.position],
                                         std::get<Immutable<Bar>>(std::move(value))};
                ++cursor.position;
                return SourceDelivery<Bar>{std::move(record)};
            },
            [](Cursor &cursor, const Data &data) -> Result<SourceEnd> {
                if (cursor.position != data.timeline.size()) {
                    return diagnostic(FailureCode::SourceCoverage,
                                      "fixture ended before selected scope closure");
                }
                return SourceEnd{data.metadata.validated_scope};
            },
            build.pin};
        checked(resolver.register_provider(std::move(provider)));
        OperationContract description{
            key(),
            build.implementation,
            {1, {}},
            {},
            {{"bar", type.policy.contract, StorageKind::ImmutableOwned,
              contract("unsupported_bar_codec"), contract("qaedalus.fixture_complete_bars_v1"),
              false}},
            {SharingClass::StatefulDeclared,
             contract("source_cursor"),
             contract("same_step"),
             contract("none"),
             true,
             true,
             false,
             {"fixture-source-tests"}},
            {"interval_v1"},
            {Alignment::SameStep},
            {build.missing_intervals},
            {"count"},
            "Fixed versioned 120-Bar synthetic source; source-service construction only"};
        return registry.register_definition(
            std::move(description), build.pin,
            [](const ParamSet &, const PreparationFacts &) -> Result<PreparedKernel> {
                return diagnostic(FailureCode::UnsupportedContract,
                                  "fixture source is constructed through SourceResolver");
            });
    });
}
} // namespace lexongine::fixture
