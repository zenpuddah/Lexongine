#pragma once

#include "lexongine/prepare.hpp"

namespace lexongine {

struct SourceServiceLimits {
    SourceValidationLimits coverage;
    std::uint64_t string_bytes, candidate_bytes, snapshot_bytes, cursor_bytes;
};
struct SourceValidationServices {
    std::shared_ptr<ResourceAccount> account;
    SourceServiceLimits limits;
    MissingIntervalPolicy missing_intervals;
    ResourceBounds admitted_resources;
};
struct SourceCursorServices {
    const std::shared_ptr<ResourceAccount> account;
};
template <class T> struct SourceRecord {
    TimingDescriptor timing;
    PublishValue<T> value;
};
template <class T> using SourceDelivery = std::variant<SourceRecord<T>, SourceGap, SourceEnd>;
using SourceEvent = std::variant<PendingOutcome, SourceGap, SourceEnd>;

template <class T, class Inspection, class Data, class Cursor> struct TypedSourceProvider {
    std::string name;
    RegisteredType<T> output_type;
    std::function<Result<Inspection>(const SourceRequest &, SourceValidationServices &)> inspect;
    std::function<Result<SourceBounds>(const Inspection &)> bounds;
    std::function<Result<Data>(Inspection &&, const SourceRequest &, SourceValidationServices &)>
        validate;
    std::function<Result<SourceMetadata>(const Data &)> metadata;
    std::function<Result<std::span<const TimingDescriptor>>(const Data &)> timeline;
    std::function<Result<Cursor>(const Data &, SourceCursorServices &)> make_cursor;
    std::function<Result<SourceDelivery<T>>(Cursor &, const Data &, const UpdateContext &,
                                            PublicationAllocator &)>
        next;
    std::function<Result<SourceEnd>(Cursor &, const Data &)> finish;
    ModulePin pin;
};

namespace detail {
struct SourceCursorCallbacks {
    std::function<Result<SourceEvent>(const UpdateContext &, const TimingDescriptor &)> next;
    std::function<Result<SourceEnd>()> finish;
};
struct ValidatedSourceData {
    SourceMetadata metadata;
    std::span<const TimingDescriptor>
        timeline; // Pinned by the factory's Data capture until host adoption/copy.
    std::function<Result<SourceCursorCallbacks>(SourceCursorServices &)> make_cursor;
};
struct InspectedSource {
    SourceBounds bounds;
    std::function<Result<ValidatedSourceData>(const SourceRequest &, SourceValidationServices &)>
        validate;
};
struct SourceAdapter {
    std::string name;
    ValueContract output;
    std::uint64_t inspection_bytes, data_bytes, cursor_bytes;
    std::function<Result<InspectedSource>(const SourceRequest &, SourceValidationServices &)>
        inspect;
};
struct SourceProviderControl;
struct SourceSnapshotControl;
} // namespace detail

// Complete owning declarations precede Result<opaque wrapper> instantiations.
class SourceCursor {
  public:
    SourceCursor(SourceCursor &&) noexcept;
    SourceCursor &operator=(SourceCursor &&) noexcept;
    ~SourceCursor() noexcept;
    SourceCursor(const SourceCursor &) = delete;
    SourceCursor &operator=(const SourceCursor &) = delete;
    [[nodiscard]] const TimingDescriptor *descriptor() const noexcept;
    [[nodiscard]] Result<SourceEvent> next(const UpdateContext &) noexcept;
    [[nodiscard]] Result<SourceEnd> finish() noexcept;

  private:
    friend class SourceSnapshot;
    friend class SourceResolver;
    struct Impl;
    explicit SourceCursor(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
};
class SourceSnapshot {
  public:
    SourceSnapshot(const SourceSnapshot &) noexcept = default;
    SourceSnapshot &operator=(const SourceSnapshot &) noexcept = default;
    SourceSnapshot(SourceSnapshot &&) noexcept = default;
    SourceSnapshot &operator=(SourceSnapshot &&) noexcept = default;
    ~SourceSnapshot() noexcept;
    [[nodiscard]] const SourceMetadata &metadata() const noexcept;
    [[nodiscard]] std::span<const TimingDescriptor>
    timeline() const noexcept; // Host-only validated timing, not payload access.
    [[nodiscard]] const ValueContract &output_contract() const noexcept;
    [[nodiscard]] Result<SourceCursor> cursor(PublicationAllocator &) const noexcept;

  private:
    friend class SourceResolver;
    explicit SourceSnapshot(std::shared_ptr<const detail::SourceSnapshotControl>) noexcept;
    std::shared_ptr<const detail::SourceSnapshotControl> impl_;
};
class SourceCandidate {
  public:
    SourceCandidate(SourceCandidate &&) noexcept;
    SourceCandidate &operator=(SourceCandidate &&) noexcept;
    ~SourceCandidate() noexcept;
    SourceCandidate(const SourceCandidate &) = delete;
    SourceCandidate &operator=(const SourceCandidate &) = delete;
    [[nodiscard]] const SourceBounds &bounds() const noexcept;

  private:
    friend class SourceResolver;
    struct Impl;
    explicit SourceCandidate(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
};
class SourceResolver {
  public:
    SourceResolver(SourceResolver &&) noexcept;
    SourceResolver &operator=(SourceResolver &&) noexcept;
    ~SourceResolver() noexcept;
    SourceResolver(const SourceResolver &) = delete;
    SourceResolver &operator=(const SourceResolver &) = delete;
    [[nodiscard]] static Result<SourceResolver> create(std::shared_ptr<ResourceAccount>,
                                                       std::uint64_t registration_bytes,
                                                       std::uint64_t maximum_providers) noexcept;
    template <class T, class Inspection, class Data, class Cursor>
    [[nodiscard]] Result<void>
        register_provider(TypedSourceProvider<T, Inspection, Data, Cursor>) noexcept;
    [[nodiscard]] Result<void> freeze() noexcept;
    [[nodiscard]] Result<SourceCandidate> inspect(const SourceRequest &,
                                                  SourceValidationServices &) noexcept;
    [[nodiscard]] Result<SourceSnapshot> validate(SourceCandidate &&, const SourceRequest &,
                                                  SourceValidationServices &) noexcept;

  private:
    struct Impl;
    explicit SourceResolver(std::unique_ptr<Impl>) noexcept;
    [[nodiscard]] std::shared_ptr<ResourceAccount> registration_account() const noexcept;
    [[nodiscard]] Result<void> register_adapter(ModulePin, detail::SourceAdapter) noexcept;
    std::unique_ptr<Impl> impl_;
};

template <class T, class Inspection, class Data, class Cursor>
Result<void> SourceResolver::register_provider(
    TypedSourceProvider<T, Inspection, Data, Cursor> provider) noexcept {
    static_assert(std::is_nothrow_destructible_v<Inspection> &&
                  std::is_nothrow_destructible_v<Data> && std::is_nothrow_destructible_v<Cursor>);
    return guarded([&]() -> Result<void> {
        if (!provider.inspect || !provider.bounds || !provider.validate || !provider.metadata ||
            !provider.timeline || !provider.make_cursor || !provider.next || !provider.finish ||
            !provider.output_type.policy.validate || !provider.output_type.policy.measure ||
            !provider.output_type.policy.freeze || provider.name.empty()) {
            return diagnostic(FailureCode::InvalidConfig, "incomplete typed source provider");
        }
        if (provider.output_type.witness.type != std::type_index(typeid(T)) ||
            provider.output_type.policy.storage != PublicationTraits<T>::storage) {
            return diagnostic(FailureCode::TypeMismatch,
                              "source provider native witness/storage mismatch");
        }
        const auto &policy = provider.output_type.policy;
        if ((policy.storage == StorageKind::ScalarCopy &&
             (policy.freeze_strategy != FreezeStrategy::ScalarCopy ||
              !std::is_trivially_copyable_v<T>)) ||
            (policy.storage == StorageKind::ImmutableOwned &&
             policy.freeze_strategy != FreezeStrategy::ExclusiveConstruction &&
             policy.freeze_strategy != FreezeStrategy::DeepCopy)) {
            return diagnostic(FailureCode::InvalidConfig,
                              "contradictory source storage/freeze policy");
        }
        auto combined =
            combine_pins(registration_account(), provider.pin, provider.output_type.policy.pin);
        if (const auto *error = std::get_if<Diagnostic>(&combined)) {
            return *error;
        }
        auto pin = std::get<ModulePin>(std::move(combined));
        auto size = type_storage_bytes(provider.output_type.policy.contract);
        if (const auto *error = std::get_if<Diagnostic>(&size)) {
            return *error;
        }
        auto overhead = checked_multiply(std::get<std::uint64_t>(size), 4);
        if (const auto *error = std::get_if<Diagnostic>(&overhead)) {
            return *error;
        }
        overhead = checked_add(std::get<std::uint64_t>(overhead), sizeof(provider) * 4 + 1024);
        if (const auto *error = std::get_if<Diagnostic>(&overhead)) {
            return *error;
        }
        const auto bytes = std::get<std::uint64_t>(overhead);
        auto inspection_size = checked_add(bytes, sizeof(Inspection));
        auto data_size = checked_add(bytes, sizeof(Data));
        auto cursor_size = checked_add(bytes, sizeof(Cursor));
        if (std::holds_alternative<Diagnostic>(inspection_size) ||
            std::holds_alternative<Diagnostic>(data_size) ||
            std::holds_alternative<Diagnostic>(cursor_size)) {
            return diagnostic(FailureCode::ResourcePlanExceeded,
                              "typed source control size overflow");
        }
        auto name = provider.name;
        ValueContract output{provider.output_type.policy.contract, typeid(T),
                             provider.output_type.policy.storage, pin};
        return register_adapter(
            pin,
            {std::move(name), std::move(output), std::get<std::uint64_t>(inspection_size),
             std::get<std::uint64_t>(data_size), std::get<std::uint64_t>(cursor_size),
             [provider = std::move(provider),
              pin](const SourceRequest &request,
                   SourceValidationServices &services) -> Result<detail::InspectedSource> {
                 auto inspected = provider.inspect(request, services);
                 if (const auto *error = std::get_if<Diagnostic>(&inspected)) {
                     return *error;
                 }
                 auto inspection =
                     std::make_shared<Inspection>(std::get<Inspection>(std::move(inspected)));
                 auto bounds = provider.bounds(*inspection);
                 if (const auto *error = std::get_if<Diagnostic>(&bounds)) {
                     return *error;
                 }
                 return detail::InspectedSource{
                     std::get<SourceBounds>(std::move(bounds)),
                     [inspection, validate = provider.validate, metadata = provider.metadata,
                      timeline = provider.timeline, make_cursor = provider.make_cursor,
                      next = provider.next, finish = provider.finish, type = provider.output_type,
                      pin](const SourceRequest &request, SourceValidationServices &services)
                         -> Result<detail::ValidatedSourceData> {
                         auto validated = validate(std::move(*inspection), request, services);
                         if (const auto *error = std::get_if<Diagnostic>(&validated)) {
                             return *error;
                         }
                         auto data =
                             std::make_shared<const Data>(std::get<Data>(std::move(validated)));
                         auto described = metadata(*data);
                         if (const auto *error = std::get_if<Diagnostic>(&described)) {
                             return *error;
                         }
                         auto timing = timeline(*data);
                         if (const auto *error = std::get_if<Diagnostic>(&timing)) {
                             return *error;
                         }
                         return detail::ValidatedSourceData{
                             std::get<SourceMetadata>(std::move(described)),
                             std::get<std::span<const TimingDescriptor>>(timing),
                             [data, make_cursor, next, finish, type,
                              pin](SourceCursorServices &services)
                                 -> Result<detail::SourceCursorCallbacks> {
                                 auto made = make_cursor(*data, services);
                                 if (const auto *error = std::get_if<Diagnostic>(&made)) {
                                     return *error;
                                 }
                                 auto cursor =
                                     std::make_shared<Cursor>(std::get<Cursor>(std::move(made)));
                                 return detail::SourceCursorCallbacks{
                                     [data, cursor, next, type, pin, account = services.account](
                                         const UpdateContext &context,
                                         const TimingDescriptor &expected) -> Result<SourceEvent> {
                                         auto staging = PendingOutcome::create(account, context, 1);
                                         if (const auto *error =
                                                 std::get_if<Diagnostic>(&staging)) {
                                             return *error;
                                         }
                                         auto pending =
                                             std::get<PendingOutcome>(std::move(staging));
                                         const std::array<OutputPolicyBinding, 1> policies{
                                             {{type.witness.type, &type}}};
                                         PublicationAllocator allocator{account, 0, policies, pin};
                                         auto delivery = next(*cursor, *data, context, allocator);
                                         if (const auto *error =
                                                 std::get_if<Diagnostic>(&delivery)) {
                                             return *error;
                                         }
                                         auto &value = std::get<SourceDelivery<T>>(delivery);
                                         if (const auto *gap = std::get_if<SourceGap>(&value)) {
                                             return SourceEvent{*gap};
                                         }
                                         if (const auto *end = std::get_if<SourceEnd>(&value)) {
                                             return SourceEvent{*end};
                                         }
                                         auto &record = std::get<SourceRecord<T>>(value);
                                         const auto *actual_record =
                                             std::get_if<CompleteRecord>(&record.timing.delivery);
                                         const auto *expected_record =
                                             std::get_if<CompleteRecord>(&expected.delivery);
                                         if (record.timing.key != expected.key ||
                                             record.timing.extent != expected.extent ||
                                             record.timing.available_at_ns !=
                                                 expected.available_at_ns ||
                                             !actual_record || !expected_record ||
                                             actual_record->payload_index !=
                                                 expected_record->payload_index) {
                                             return diagnostic(FailureCode::SourceCoverage,
                                                               "source record differs from its "
                                                               "validated descriptor");
                                         }
                                         auto erased = detail::erase_output<T>(
                                             std::move(record.value), type, allocator);
                                         if (const auto *error = std::get_if<Diagnostic>(&erased)) {
                                             return *error;
                                         }
                                         auto stored = pending.set_output(
                                             0, std::get<ErasedValue>(std::move(erased)));
                                         if (const auto *error = std::get_if<Diagnostic>(&stored)) {
                                             return *error;
                                         }
                                         auto parents = ProvenanceValidator::attach(
                                             pending, ParentSelections::none(), {}, 0, context);
                                         if (const auto *error =
                                                 std::get_if<Diagnostic>(&parents)) {
                                             return *error;
                                         }
                                         return SourceEvent{std::move(pending)};
                                     },
                                     [data, cursor, finish]() -> Result<SourceEnd> {
                                         return finish(*cursor, *data);
                                     }};
                             }};
                     }};
             }});
    });
}

} // namespace lexongine
