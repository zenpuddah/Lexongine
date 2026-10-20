#include "lexongine/source.hpp"

namespace lexongine {
namespace {
struct SourceFailure {
    Diagnostic error;
};
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
[[noreturn]] void fail(FailureCode code, std::string_view message) {
    throw SourceFailure{diagnostic(code, message)};
}
template <class F> auto source_guarded(F &&call) noexcept -> decltype(call()) {
    return guarded([&]() -> decltype(call()) {
        try {
            return call();
        } catch (const SourceFailure &failure) {
            return failure.error;
        }
    });
}
std::array<std::uint64_t, 7> resources(const ResourceBounds &value) {
    return {value.source,    value.state, value.scratch, value.staging,
            value.retention, value.graph, value.evidence};
}
bool same_limits(const SourceServiceLimits &a, const SourceServiceLimits &b) {
    return a.string_bytes == b.string_bytes && a.candidate_bytes == b.candidate_bytes &&
           a.snapshot_bytes == b.snapshot_bytes && a.cursor_bytes == b.cursor_bytes &&
           a.coverage.max_records == b.coverage.max_records &&
           a.coverage.max_timeline_events == b.coverage.max_timeline_events &&
           a.coverage.max_gap_entries == b.coverage.max_gap_entries &&
           a.coverage.max_expected_updates == b.coverage.max_expected_updates;
}
bool same_request(const SourceRequest &a, const SourceRequest &b) {
    return a.provider_name == b.provider_name && a.snapshot_name == b.snapshot_name &&
           a.requested_version == b.requested_version && a.interpretation == b.interpretation &&
           a.required_scope == b.required_scope && a.provider_policies == b.provider_policies;
}
class Meter {
  public:
    Meter(std::uint64_t bound, std::uint64_t strings) : bound_(bound), strings_(strings) {}
    void add(std::uint64_t bytes) {
        bytes_ = checked(checked_add(bytes_, bytes));
        if (bytes_ > bound_) {
            fail(FailureCode::ResourcePlanExceeded,
                 "source wrapper exceeds its explicit byte ceiling");
        }
    }
    void text(std::string_view value, bool empty = false) {
        checked(validate_text(value, strings_, empty));
        add(checked(checked_add(value.size(), 32)));
    }
    void params(const CanonicalParams &value) {
        if (value.schema_version == 0) {
            fail(FailureCode::InvalidConfig, "source parameter schema is unresolved");
        }
        add(checked(checked_multiply(value.values.size(), sizeof(NamedParam))));
        std::string_view previous;
        for (const auto &parameter : value.values) {
            text(parameter.name);
            text(parameter.provenance, true);
            if (!previous.empty() && !unsigned_text_less(previous, parameter.name)) {
                fail(FailureCode::InvalidConfig, "source parameters are not canonical");
            }
            previous = parameter.name;
            if (const auto *item = std::get_if<std::string>(&parameter.value)) {
                text(*item, true);
            }
        }
    }
    void contract(const Contract &value) {
        text(value.id);
        if (value.version == 0) {
            fail(FailureCode::UnsupportedContract, "unversioned source contract");
        }
        params(value.parameters);
    }
    void type(const TypeContract &value) {
        text(value.nominal_name);
        if (value.semantic_version == 0) {
            fail(FailureCode::TypeMismatch, "unversioned source interpretation");
        }
        params(value.meaning);
    }
    void scope(const SequenceScope &value) {
        text(value.clock);
        if (value.end < value.begin) {
            fail(FailureCode::SourceCoverage, "reversed source scope");
        }
    }
    void request(const SourceRequest &value) {
        add(sizeof(SourceRequest));
        text(value.provider_name);
        text(value.snapshot_name);
        text(value.requested_version);
        type(value.interpretation);
        scope(value.required_scope);
        params(value.provider_policies);
    }
    void metadata(const SourceMetadata &value) {
        add(sizeof(SourceMetadata));
        text(value.snapshot_name);
        text(value.version);
        type(value.interpretation);
        scope(value.validated_scope);
        scope(value.coverage.expected_scope);
        add(checked(checked_multiply(value.coverage.complete_keys.size(), sizeof(UpdateKey))));
        for (const auto &key : value.coverage.complete_keys) {
            text(key.clock);
        }
        add(checked(checked_multiply(value.coverage.gaps.size(), sizeof(SourceGap))));
        for (const auto &gap : value.coverage.gaps) {
            scope(gap.missing_scope);
        }
        contract(value.coverage.expected_timeline_contract);
        contract(value.timing);
        contract(value.order);
        contract(value.availability);
        contract(value.validation);
        text(value.provider_implementation.sdk_build_contract_id);
        if (value.content_digest == Sha256{} || value.source_reference_digest == Sha256{} ||
            value.provider_implementation.binary_digest == Sha256{} ||
            value.provider_implementation.dependencies_digest == Sha256{} ||
            value.provider_implementation.source_build_digest == Sha256{}) {
            fail(FailureCode::SourceInvalid,
                 "source content/reference/build identity is unresolved");
        }
    }
    void timeline(std::span<const TimingDescriptor> value) {
        add(checked(checked_multiply(value.size(), sizeof(TimingDescriptor))));
        for (const auto &event : value) {
            text(event.key.clock);
            if (const auto *gap = std::get_if<DeclaredGap>(&event.delivery)) {
                scope(gap->gap.missing_scope);
            }
        }
    }
    [[nodiscard]] std::uint64_t size() const noexcept {
        return bytes_;
    }

  private:
    std::uint64_t bound_, strings_, bytes_ = 0;
};
void services_valid(const SourceValidationServices &value) {
    if (!value.account || value.limits.string_bytes == 0 ||
        (value.missing_intervals != MissingIntervalPolicy::Strict &&
         value.missing_intervals != MissingIntervalPolicy::Skip)) {
        fail(FailureCode::InvalidConfig,
             "source validation requires explicit account, text bound and missing policy");
    }
    checked(value.admitted_resources.total());
}
void bounds_valid(const SourceBounds &bounds, const SourceRequest &request,
                  const SourceValidationServices &services) {
    if (bounds.available_scope.clock != request.required_scope.clock ||
        bounds.available_scope.begin > request.required_scope.begin ||
        bounds.available_scope.end < request.required_scope.end ||
        bounds.available_scope.end < bounds.available_scope.begin) {
        fail(FailureCode::SourceCoverage, "provider bounds do not contain the requested scope");
    }
    checked(bounds.resources.total());
    const auto required = resources(bounds.resources),
               admitted = resources(services.admitted_resources);
    for (std::size_t i = 0; i < required.size(); ++i) {
        if (required[i] > admitted[i]) {
            fail(FailureCode::ResourcePlanExceeded, "provider resource bound exceeds admission");
        }
    }
}
} // namespace

namespace detail {
// Host-compiled controls keep the module pin until all typed managers/destructors
// return, including failure cleanup and the last cursor after resolver teardown.
struct SourceProviderControl {
    ModulePin pin;
    std::shared_ptr<const Reservation> registration_charge;
    SourceAdapter adapter;
};
struct SourceSnapshotControl {
    ModulePin pin;
    Reservation charge;
    SourceMetadata metadata;
    ValueContract output;
    std::vector<TimingDescriptor> timeline;
    SourceServiceLimits limits;
    std::uint64_t cursor_minimum;
    std::function<Result<SourceCursorCallbacks>(SourceCursorServices &)> make_cursor;
};
} // namespace detail
struct SourceCandidate::Impl {
    std::shared_ptr<const detail::SourceProviderControl> provider;
    Reservation charge;
    SourceRequest request;
    SourceValidationServices services;
    detail::InspectedSource inspection;
};
struct SourceResolver::Impl {
    std::shared_ptr<const Reservation> charge;
    std::shared_ptr<ResourceAccount> account;
    std::uint64_t remaining, capacity;
    bool frozen = false;
    std::vector<std::shared_ptr<const detail::SourceProviderControl>> providers;
};
struct SourceCursor::Impl {
    std::shared_ptr<const detail::SourceSnapshotControl> snapshot;
    Reservation charge;
    std::shared_ptr<ResourceAccount> account;
    detail::SourceCursorCallbacks callbacks;
    std::size_t position = 0;
    bool closed = false;
    std::optional<Diagnostic> failure;
};

SourceResolver::SourceResolver(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
SourceResolver::SourceResolver(SourceResolver &&) noexcept = default;
SourceResolver &SourceResolver::operator=(SourceResolver &&) noexcept = default;
SourceResolver::~SourceResolver() noexcept = default;
SourceCandidate::SourceCandidate(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
SourceCandidate::SourceCandidate(SourceCandidate &&) noexcept = default;
SourceCandidate &SourceCandidate::operator=(SourceCandidate &&) noexcept = default;
SourceCandidate::~SourceCandidate() noexcept = default;
const SourceBounds &SourceCandidate::bounds() const noexcept {
    return impl_->inspection.bounds;
}
SourceSnapshot::SourceSnapshot(std::shared_ptr<const detail::SourceSnapshotControl> impl) noexcept
    : impl_(std::move(impl)) {}
SourceSnapshot::~SourceSnapshot() noexcept = default;
const SourceMetadata &SourceSnapshot::metadata() const noexcept {
    return impl_->metadata;
}
std::span<const TimingDescriptor> SourceSnapshot::timeline() const noexcept {
    return impl_->timeline;
}
const ValueContract &SourceSnapshot::output_contract() const noexcept {
    return impl_->output;
}
SourceCursor::SourceCursor(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
SourceCursor::SourceCursor(SourceCursor &&) noexcept = default;
SourceCursor &SourceCursor::operator=(SourceCursor &&) noexcept = default;
SourceCursor::~SourceCursor() noexcept = default;

Result<SourceResolver> SourceResolver::create(std::shared_ptr<ResourceAccount> account,
                                              std::uint64_t bytes, std::uint64_t maximum) noexcept {
    return source_guarded([&]() -> Result<SourceResolver> {
        if (!account || maximum == 0 ||
            maximum >
                std::vector<std::shared_ptr<const detail::SourceProviderControl>>{}.max_size()) {
            fail(FailureCode::InvalidConfig,
                 "source registry needs explicit account and provider capacity");
        }
        const auto base = checked(checked_add(
            sizeof(Impl) + 512,
            checked(checked_multiply(
                maximum, sizeof(std::shared_ptr<const detail::SourceProviderControl>)))));
        if (base > bytes) {
            fail(FailureCode::ResourcePlanExceeded,
                 "source registry control exceeds registration ceiling");
        }
        auto reserved = checked(account->reserve(ResourceKind::Graph, bytes));
        auto impl =
            std::make_unique<Impl>(Impl{std::make_shared<const Reservation>(std::move(reserved)),
                                        std::move(account),
                                        bytes - base,
                                        maximum,
                                        false,
                                        {}});
        impl->providers.reserve(static_cast<std::size_t>(maximum));
        return SourceResolver{std::move(impl)};
    });
}
std::shared_ptr<ResourceAccount> SourceResolver::registration_account() const noexcept {
    return impl_ ? impl_->account : nullptr;
}
Result<void> SourceResolver::register_adapter(ModulePin pin,
                                              detail::SourceAdapter adapter) noexcept {
    return source_guarded([&]() -> Result<void> {
        if (!impl_ || impl_->frozen) {
            fail(FailureCode::InvalidConfig, "source registry is empty or frozen");
        }
        for (const auto &provider : impl_->providers) {
            if (provider->adapter.name == adapter.name) {
                fail(FailureCode::DuplicateDefinition, "source provider already registered");
            }
        }
        if (impl_->providers.size() == impl_->capacity) {
            fail(FailureCode::ResourcePlanExceeded, "source provider capacity exceeded");
        }
        Meter meter{impl_->remaining, impl_->remaining};
        meter.add(sizeof(detail::SourceProviderControl) + 512);
        meter.text(adapter.name);
        meter.type(adapter.output.semantic);
        meter.add(adapter.inspection_bytes); // Includes retained typed callbacks/policies; dynamic
                                             // provider-owned buffers account separately.
        auto control = std::make_shared<const detail::SourceProviderControl>(
            detail::SourceProviderControl{std::move(pin), impl_->charge, std::move(adapter)});
        impl_->providers.push_back(std::move(control));
        impl_->remaining -= meter.size();
        return ok();
    });
}
Result<void> SourceResolver::freeze() noexcept {
    if (!impl_) {
        return diagnostic(FailureCode::InvalidConfig, "empty source resolver");
    }
    impl_->frozen = true;
    return ok();
}
Result<SourceCandidate> SourceResolver::inspect(const SourceRequest &request,
                                                SourceValidationServices &services) noexcept {
    return source_guarded([&]() -> Result<SourceCandidate> {
        if (!impl_ || !impl_->frozen) {
            fail(FailureCode::InvalidConfig, "source resolver must be frozen before inspection");
        }
        services_valid(services);
        Meter meter{services.limits.candidate_bytes, services.limits.string_bytes};
        meter.request(request);
        if (request.required_scope.end - request.required_scope.begin >
            services.limits.coverage.max_expected_updates) {
            fail(FailureCode::ResourcePlanExceeded, "source request exceeds expected-update limit");
        }
        const auto found = std::find_if(
            impl_->providers.begin(), impl_->providers.end(),
            [&](const auto &provider) { return provider->adapter.name == request.provider_name; });
        if (found == impl_->providers.end()) {
            fail(FailureCode::UnknownDefinition, "exact source provider is not registered");
        }
        const auto provider = *found;
        if (!(provider->adapter.output.semantic == request.interpretation)) {
            fail(FailureCode::TypeMismatch,
                 "requested source interpretation differs from provider");
        }
        meter.add(sizeof(SourceCandidate::Impl) + 256);
        meter.add(provider->adapter.inspection_bytes);
        auto reservation = checked(
            services.account->reserve(ResourceKind::Graph, services.limits.candidate_bytes));
        auto captured_request = request;
        auto captured_services = services;
        auto callback_services = services;
        auto inspected = checked(provider->adapter.inspect(captured_request, callback_services));
        bounds_valid(inspected.bounds, captured_request, captured_services);
        return SourceCandidate{std::make_unique<SourceCandidate::Impl>(
            SourceCandidate::Impl{provider, std::move(reservation), std::move(captured_request),
                                  std::move(captured_services), std::move(inspected)})};
    });
}
Result<SourceSnapshot> SourceResolver::validate(SourceCandidate &&candidate,
                                                const SourceRequest &request,
                                                SourceValidationServices &services) noexcept {
    return source_guarded([&]() -> Result<SourceSnapshot> {
        auto owned = std::move(
            candidate
                .impl_); // Consume even a rejected attempt; partial validation cannot be retried.
        if (!impl_ || !impl_->frozen || !owned) {
            fail(FailureCode::InvalidConfig, "source candidate/resolver is unavailable");
        }
        if (std::find(impl_->providers.begin(), impl_->providers.end(), owned->provider) ==
                impl_->providers.end() ||
            !same_request(owned->request, request) || services.account != owned->services.account ||
            services.missing_intervals != owned->services.missing_intervals ||
            !same_limits(services.limits, owned->services.limits) ||
            resources(services.admitted_resources) !=
                resources(owned->services.admitted_resources)) {
            fail(FailureCode::SourceChanged,
                 "candidate request, resolver or validation services changed");
        }
        services_valid(services);
        const auto &adapter = owned->provider->adapter;
        const auto cursor_minimum =
            checked(checked_add(sizeof(SourceCursor::Impl) + 256, adapter.cursor_bytes));
        if (cursor_minimum > services.limits.cursor_bytes) {
            fail(FailureCode::ResourcePlanExceeded, "cursor control exceeds its byte ceiling");
        }
        Meter meter{services.limits.snapshot_bytes, services.limits.string_bytes};
        meter.add(sizeof(detail::SourceSnapshotControl) + 256);
        meter.add(adapter.data_bytes);
        meter.type(adapter.output.semantic);
        auto charge =
            checked(services.account->reserve(ResourceKind::Graph, services.limits.snapshot_bytes));
        auto callback_services = services;
        auto validated = checked(owned->inspection.validate(owned->request, callback_services));
        const auto &metadata = validated.metadata;
        if (metadata.snapshot_name != request.snapshot_name ||
            metadata.version != request.requested_version ||
            metadata.validated_scope != request.required_scope ||
            metadata.coverage.expected_scope != metadata.validated_scope) {
            fail(FailureCode::SourceChanged,
                 "validated source identity/version/scope disagrees with request");
        }
        if (!(metadata.interpretation == adapter.output.semantic) ||
            metadata.witness.type != adapter.output.witness) {
            fail(FailureCode::TypeMismatch,
                 "validated source has a different semantic/native type");
        }
        checked(validate_source_timeline(metadata.coverage, validated.timeline,
                                         metadata.coverage.complete_keys.size(),
                                         services.missing_intervals, services.limits.coverage));
        meter.metadata(metadata);
        meter.timeline(validated.timeline);
        return SourceSnapshot{std::make_shared<const detail::SourceSnapshotControl>(
            detail::SourceSnapshotControl{owned->provider->pin,
                                          std::move(charge),
                                          std::move(validated.metadata),
                                          adapter.output,
                                          {validated.timeline.begin(), validated.timeline.end()},
                                          services.limits,
                                          cursor_minimum,
                                          std::move(validated.make_cursor)})};
    });
}
Result<SourceCursor> SourceSnapshot::cursor(PublicationAllocator &allocator) const noexcept {
    return source_guarded([&]() -> Result<SourceCursor> {
        if (!impl_ || !allocator.account_) {
            fail(FailureCode::InvalidConfig, "source cursor requires snapshot and run account");
        }
        if (impl_->cursor_minimum > impl_->limits.cursor_bytes) {
            fail(FailureCode::ResourcePlanExceeded, "cursor control exceeds its byte ceiling");
        }
        auto charge =
            checked(allocator.account_->reserve(ResourceKind::State, impl_->limits.cursor_bytes));
        SourceCursorServices services{allocator.account_};
        auto callbacks = checked(impl_->make_cursor(services));
        return SourceCursor{std::make_unique<SourceCursor::Impl>(SourceCursor::Impl{
            impl_, std::move(charge), allocator.account_, std::move(callbacks), 0, false, {}})};
    });
}
const TimingDescriptor *SourceCursor::descriptor() const noexcept {
    if (!impl_ || impl_->closed || impl_->failure ||
        impl_->position == impl_->snapshot->timeline.size()) {
        return nullptr;
    }
    return &impl_->snapshot->timeline[impl_->position];
}
Result<SourceEvent> SourceCursor::next(const UpdateContext &context) noexcept {
    if (!impl_ || impl_->closed) {
        return diagnostic(FailureCode::AlreadyExecuted, "source cursor is closed");
    }
    if (impl_->failure) {
        return *impl_->failure;
    }
    const auto *expected = descriptor();
    if (!expected || context.key != expected->key || context.extent != expected->extent ||
        context.activation_available_at_ns != expected->available_at_ns) {
        return diagnostic(FailureCode::UpdateProtocol,
                          "source activation differs from next validated descriptor");
    }
    if (auto valid = validate_context(context); std::holds_alternative<Diagnostic>(valid)) {
        return std::get<Diagnostic>(valid);
    }
    auto result = source_guarded([&]() -> Result<SourceEvent> {
        auto event = checked(impl_->callbacks.next(context, *expected));
        if (std::holds_alternative<SourceEnd>(event)) {
            fail(FailureCode::SourceCoverage, "provider ended before context-free scope closure");
        }
        if (const auto *declared = std::get_if<DeclaredGap>(&expected->delivery)) {
            const auto *actual = std::get_if<SourceGap>(&event);
            if (!actual || !(*actual == declared->gap)) {
                fail(FailureCode::SourceCoverage, "provider gap differs from validated timeline");
            }
        } else if (!std::holds_alternative<PendingOutcome>(event)) {
            fail(FailureCode::SourceCoverage, "provider omitted a required complete record");
        }
        ++impl_->position;
        return event;
    });
    if (const auto *error = std::get_if<Diagnostic>(&result)) {
        impl_->failure = *error;
    }
    return result;
}
Result<SourceEnd> SourceCursor::finish() noexcept {
    if (!impl_ || impl_->closed) {
        return diagnostic(FailureCode::AlreadyExecuted, "source cursor is closed");
    }
    if (impl_->failure) {
        return *impl_->failure;
    }
    if (impl_->position != impl_->snapshot->timeline.size()) {
        return diagnostic(FailureCode::SourceCoverage, "source scope is not exhausted");
    }
    auto result = source_guarded([&]() -> Result<SourceEnd> {
        auto end = checked(impl_->callbacks.finish());
        if (end.completed_scope != impl_->snapshot->metadata.validated_scope) {
            fail(FailureCode::SourceCoverage,
                 "provider terminal scope differs from validated scope");
        }
        impl_->closed = true;
        return end;
    });
    if (const auto *error = std::get_if<Diagnostic>(&result)) {
        impl_->failure = *error;
    }
    return result;
}

} // namespace lexongine
