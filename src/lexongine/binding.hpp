#pragma once

#include "lexongine/results.hpp"

namespace lexongine {

struct ValueContract {
    TypeContract semantic;
    std::type_index witness;
    StorageKind storage;
    ModulePin pin;
};
struct ProducerAuthority {
    Reservation charge;
    Sha256 computation;
    std::optional<Sha256> source_reference;
};
struct ParentCertificate {
    Reservation charge;
    ParentPublication publication;
    std::shared_ptr<const ProducerAuthority> authority;
    std::uint64_t publication_ordinal;
};
inline Result<std::uint64_t> parent_certificate_storage_bytes(std::uint64_t clock_bytes) noexcept {
    return checked_add(sizeof(ParentCertificate) + 257, clock_bytes);
}
inline Result<std::uint64_t> type_storage_bytes(const TypeContract& type) noexcept {
    auto size = checked_add(sizeof(TypeContract), type.nominal_name.size() + 1);
    if (const auto* error = std::get_if<Diagnostic>(&size)) { return *error; }
    auto count = std::get<std::uint64_t>(size);
    for (const auto& parameter : type.meaning.values) {
        auto addition = checked_add(sizeof(NamedParam), parameter.name.size() + parameter.provenance.size() + 2);
        if (const auto* error = std::get_if<Diagnostic>(&addition)) { return *error; }
        if (const auto* text = std::get_if<std::string>(&parameter.value)) {
            addition = checked_add(std::get<std::uint64_t>(addition), text->size() + 1);
            if (const auto* error = std::get_if<Diagnostic>(&addition)) { return *error; }
        }
        size = checked_add(count, std::get<std::uint64_t>(addition));
        if (const auto* error = std::get_if<Diagnostic>(&size)) { return *error; }
        count = std::get<std::uint64_t>(size);
    }
    return count;
}
class ErasedValue {
public:
    ErasedValue(ErasedValue&&) noexcept = default;
    ErasedValue& operator=(ErasedValue&&) noexcept = default;
    ErasedValue(const ErasedValue&) = delete;
    ErasedValue& operator=(const ErasedValue&) = delete;
    template<class T> static Result<ErasedValue> from(Immutable<T> value, const TypeContract& semantic) noexcept {
        return guarded([&]() -> Result<ErasedValue> {
            if (!value.control()) { return diagnostic(FailureCode::OutputInvalid, "empty output payload"); }
            auto bytes = type_storage_bytes(semantic);
            if (const auto* error = std::get_if<Diagnostic>(&bytes)) { return *error; }
            auto account = value.control()->account();
            auto charge = account->reserve(ResourceKind::Staging, std::get<std::uint64_t>(bytes));
            if (const auto* error = std::get_if<Diagnostic>(&charge)) { return *error; }
            // Transfer the producer's existing lease; erasure does not create a
            // second holder or require extra holder capacity for the same owner.
            return ErasedValue{std::get<Reservation>(std::move(charge)), std::move(value.control_),
                               std::move(value.hold_), semantic};
        });
    }
    [[nodiscard]] const auto& control() const noexcept { return control_; }
    [[nodiscard]] const TypeContract& semantic() const noexcept { return semantic_; }
    template<class T> [[nodiscard]] Result<Immutable<T>> share(std::string_view holder, std::string_view reason) const noexcept {
        return Immutable<T>::from_control(control_, holder, reason);
    }
private:
    ErasedValue(Reservation charge, std::shared_ptr<const AllocationControl> control, HoldLease hold, TypeContract semantic)
        : charge_(std::move(charge)), control_(std::move(control)), hold_(std::move(hold)), semantic_(std::move(semantic)) {}
    Reservation charge_;
    std::shared_ptr<const AllocationControl> control_;
    HoldLease hold_;
    TypeContract semantic_;
};
template<class T> class ReadValue {
private:
    ModulePin consumer_pin_;
    Immutable<T> lease_;
    StorageKind storage_;
    std::shared_ptr<const ParentCertificate> certificate_;
    std::optional<PortIndex> input_;
public:
    const T& value;
    ReadValue(Immutable<T> lease, StorageKind storage, std::shared_ptr<const ParentCertificate> certificate,
              std::optional<PortIndex> input, ModulePin consumer_pin)
        : consumer_pin_(std::move(consumer_pin)), lease_(std::move(lease)), storage_(storage),
          certificate_(std::move(certificate)), input_(input), value(lease_.get()) {}
    ReadValue(ReadValue&&) noexcept = default;
    ReadValue(const ReadValue&) = delete;
    ReadValue& operator=(const ReadValue&) = delete;
    [[nodiscard]] const ParentPublication& publication() const noexcept { return certificate_->publication; }
    [[nodiscard]] ParentSelection parent() const noexcept { return ParentSelection{certificate_, input_}; }
    [[nodiscard]] Result<Immutable<T>> retain(std::string_view holder, std::string_view reason) const noexcept {
        if (storage_ == StorageKind::ScalarCopy) {
            if constexpr (std::is_trivially_copyable_v<T>) {
                auto account = lease_.control()->account();
                struct RestoreOwner {
                    std::shared_ptr<ResourceAccount> account;
                    std::optional<AllocationOwner> previous;
                    ~RestoreOwner() { account->set_owner(previous); }
                } restore{account, account->owner_context()};
                account->set_owner(account->allocation_owner(lease_.control()->id()));
                auto pin = combine_pins(lease_.control()->account(), consumer_pin_, lease_.control()->pin_);
                if (const auto* error = std::get_if<Diagnostic>(&pin)) { return *error; }
                auto copied = make_owned<T>(lease_.control()->account(), ResourceKind::Retention, sizeof(T),
                                            std::get<ModulePin>(std::move(pin)), [&] { return value; });
                if (const auto* error = std::get_if<Diagnostic>(&copied)) { return *error; }
                return std::get<Immutable<T>>(copied).share(holder, reason);
            } else {
                return diagnostic(FailureCode::TypeMismatch, "ScalarCopy requires a trivially copyable type");
            }
        }
        return lease_.share(holder, reason);
    }
};

struct OutcomeFrame {
    Reservation charge;
    std::shared_ptr<ResourceAccount> account;
    UpdateContext context;
    std::int64_t available_at_ns;
    OutcomeKind kind;
    std::uint64_t remaining_preceding;
    std::uint32_t declared_reason;
    std::vector<std::optional<ErasedValue>> outputs;
    std::vector<std::shared_ptr<const ParentCertificate>> certificates;
    ParentSelections parents;
};
class ResultSlot;
class ValidatedOutcome;
class PendingOutcome {
public:
    PendingOutcome(PendingOutcome&&) noexcept = default;
    PendingOutcome& operator=(PendingOutcome&&) noexcept = default;
    PendingOutcome(const PendingOutcome&) = delete;
    PendingOutcome& operator=(const PendingOutcome&) = delete;
    static Result<PendingOutcome> create(std::shared_ptr<ResourceAccount>, const UpdateContext&, std::size_t outputs) noexcept;
    [[nodiscard]] static Result<std::uint64_t> storage_bytes(std::uint64_t outputs, std::uint64_t clock_bytes) noexcept;
    Result<void> set_output(PortIndex index, ErasedValue value) noexcept;
    void warming(std::uint64_t remaining) noexcept;
    Result<void> waiting(std::uint64_t remaining, std::int64_t available_at_ns) noexcept;
    void absent(std::uint32_t reason) noexcept;
private:
    friend class ProvenanceValidator;
    friend class OutcomeView;
    friend class PreparedKernel;
    friend class ResultStore;
    friend class ArtifactCursor;
    friend Result<ValidatedOutcome> validate_pending(PendingOutcome&&, const ResultSlot&, const UpdateContext&) noexcept;
    explicit PendingOutcome(std::shared_ptr<OutcomeFrame> frame) : frame_(std::move(frame)) {}
    std::shared_ptr<OutcomeFrame> frame_;
};
class ValidatedOutcome {
public:
    ValidatedOutcome(ValidatedOutcome&&) noexcept = default;
    ValidatedOutcome& operator=(ValidatedOutcome&&) noexcept = default;
    ValidatedOutcome(const ValidatedOutcome&) = delete;
    ValidatedOutcome& operator=(const ValidatedOutcome&) = delete;
private:
    friend class ResultSlot;
    friend Result<ValidatedOutcome> validate_pending(PendingOutcome&&, const ResultSlot&, const UpdateContext&) noexcept;
    explicit ValidatedOutcome(std::shared_ptr<const OutcomeFrame> frame) noexcept : frame_(std::move(frame)) {}
    std::shared_ptr<const OutcomeFrame> frame_;
};
class ResultSlot {
public:
    static Result<std::shared_ptr<ResultSlot>> create(std::shared_ptr<ResourceAccount>, std::vector<ValueContract>,
                                                     std::uint64_t prior_outputs, Sha256 computation,
                                                     std::optional<Sha256> source_reference) noexcept;
    void commit(ValidatedOutcome&&) noexcept;
    [[nodiscard]] const OutcomeFrame* current() const noexcept { return current_.get(); }
    [[nodiscard]] const auto& contracts() const noexcept { return contracts_; }
    template<class T> Result<ReadValue<T>> read(PortIndex index, const UpdateContext& context,
                                              std::optional<PortIndex> input = std::nullopt, ModulePin consumer_pin = {}) const noexcept {
        if (auto result = validate_context(context); std::holds_alternative<Diagnostic>(result)) { return std::get<Diagnostic>(result); }
        if (!current_ || current_->context.key != context.key || current_->kind != OutcomeKind::Published) {
            return diagnostic(FailureCode::MissingRequired, "no published value at current key");
        }
        if (current_->available_at_ns > context.decision_time_ns) {
            return diagnostic(FailureCode::FutureRead, "publication is not yet available");
        }
        if (current_->context.extent != context.extent) {
            return diagnostic(FailureCode::UpdateProtocol, "current key has a different temporal extent");
        }
        if (index >= contracts_.size() || contracts_[index].witness != std::type_index(typeid(T))) {
            return diagnostic(FailureCode::TypeMismatch, "read requested wrong C++ output type");
        }
        auto view = Immutable<T>::from_control(current_->outputs[index]->control(), "reader", "current_output");
        if (const auto* error = std::get_if<Diagnostic>(&view)) { return *error; }
        return ReadValue<T>{std::get<Immutable<T>>(std::move(view)), contracts_[index].storage,
                            current_->certificates[index], input, std::move(consumer_pin)};
    }
    template<class T> Result<ReadValue<T>> read_previous(PortIndex index, const UpdateContext& context,
                                                        std::uint64_t prior, std::optional<PortIndex> input = std::nullopt,
                                                        ModulePin consumer_pin = {}) const noexcept {
        if (auto valid = validate_context(context); std::holds_alternative<Diagnostic>(valid)) { return std::get<Diagnostic>(valid); }
        if (!current_ || current_->kind != OutcomeKind::Published || current_->context.key != context.key ||
            prior == 0 || prior >= history_.size() || prior >= published_count_) {
            return diagnostic(FailureCode::MissingRequired, "requested publication is outside declared retained history");
        }
        if (current_->context.extent != context.extent) { return diagnostic(FailureCode::UpdateProtocol, "retained read activation extent mismatch"); }
        if (index >= contracts_.size() || contracts_[index].witness != std::type_index(typeid(T))) {
            return diagnostic(FailureCode::TypeMismatch, "retained read requested wrong C++ output type");
        }
        const auto latest = next_history_ == 0 ? history_.size() - 1 : next_history_ - 1;
        const auto offset = static_cast<std::size_t>(prior);
        const auto position = offset <= latest ? latest - offset : history_.size() - (offset - latest);
        const auto& frame = history_[position];
        if (!frame) { return diagnostic(FailureCode::MissingRequired, "retained publication is unavailable"); }
        if (current_->available_at_ns > context.decision_time_ns || frame->available_at_ns > context.decision_time_ns) {
            return diagnostic(FailureCode::FutureRead, "retained publication is not causally available");
        }
        auto view = Immutable<T>::from_control(frame->outputs[index]->control(), "reader", "declared_history");
        if (const auto* error = std::get_if<Diagnostic>(&view)) { return *error; }
        return ReadValue<T>{std::get<Immutable<T>>(std::move(view)), contracts_[index].storage, frame->certificates[index], input, std::move(consumer_pin)};
    }
private:
    friend class ProvenanceValidator;
    friend class OutcomeView;
    friend Result<ValidatedOutcome> validate_pending(PendingOutcome&&, const ResultSlot&, const UpdateContext&) noexcept;
    ResultSlot(Reservation charge, std::vector<ValueContract> contracts, std::size_t capacity,
               std::shared_ptr<const ProducerAuthority> authority)
        : charge_(std::move(charge)), contracts_(std::move(contracts)), history_(capacity), authority_(std::move(authority)) {}
    friend class PublicationHistory;
    Reservation charge_;
    std::vector<ValueContract> contracts_;
    std::vector<std::shared_ptr<const OutcomeFrame>> history_;
    std::size_t next_history_ = 0;
    std::shared_ptr<const OutcomeFrame> current_;
    std::shared_ptr<const ProducerAuthority> authority_;
    std::uint64_t published_count_ = 0;
};
Result<ValidatedOutcome> validate_pending(PendingOutcome&&, const ResultSlot&, const UpdateContext&) noexcept;
struct BoundInput { std::shared_ptr<ResultSlot> slot; PortIndex port; std::uint64_t prior_outputs; };
class ProvenanceValidator {
public:
    static Result<void> attach(PendingOutcome&, ParentSelections, std::span<const BoundInput>,
                               std::uint64_t preceding, const UpdateContext&) noexcept;
    static Result<void> restore(PendingOutcome&, std::span<const ParentReference>, std::span<const BoundInput>,
                               std::span<const class PublicationHistory* const>, std::uint64_t preceding, const UpdateContext&) noexcept;
};

} // namespace lexongine
