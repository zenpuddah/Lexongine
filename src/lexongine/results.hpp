#pragma once

#include "lexongine/model.hpp"

#include <functional>
#include <stdexcept>
#include <typeindex>

namespace lexongine {

enum class ResourceKind : std::size_t {
    Source,
    State,
    Scratch,
    Staging,
    Retention,
    Graph,
    Evidence
};
enum class RetentionEventKind {
    Allocated,
    HoldAcquired,
    HoldReleased,
    LongRetention,
    OutstandingAtClose
};
struct AllocationOwner {
    std::uint32_t producer;
    std::uint64_t publication;
};
struct RetentionEvent {
    RetentionEventKind kind;
    std::uint64_t allocation, bytes, age_steps;
    std::array<char, 64> holder{}, reason{};
    ResourceKind resource_kind = ResourceKind::Retention;
    std::uint64_t step = 0;
    std::optional<AllocationOwner> owner{};
};
struct AccountLimits {
    ResourceBounds bytes;
    std::uint64_t max_allocations, max_holders, max_events, retention_warning_steps;
};
class ResourceAccount;

class Reservation {
  public:
    Reservation() = default;
    Reservation(const Reservation &) = delete;
    Reservation &operator=(const Reservation &) = delete;
    Reservation(Reservation &&other) noexcept;
    Reservation &operator=(Reservation &&other) noexcept;
    ~Reservation() noexcept;
    [[nodiscard]] std::uint64_t id() const noexcept {
        return id_;
    }

  private:
    friend class ResourceAccount;
    Reservation(std::shared_ptr<ResourceAccount> account, std::size_t slot,
                std::uint64_t id) noexcept
        : account_(std::move(account)), slot_(slot), id_(id) {}
    void reset() noexcept;
    std::shared_ptr<ResourceAccount> account_;
    std::size_t slot_ = 0;
    std::uint64_t id_ = 0;
};
class HoldLease {
  public:
    HoldLease() = default;
    HoldLease(const HoldLease &) = delete;
    HoldLease &operator=(const HoldLease &) = delete;
    HoldLease(HoldLease &&other) noexcept;
    HoldLease &operator=(HoldLease &&other) noexcept;
    ~HoldLease() noexcept;

  private:
    friend class ResourceAccount;
    HoldLease(std::shared_ptr<ResourceAccount> account, std::size_t slot) noexcept
        : account_(std::move(account)), slot_(slot) {}
    void reset() noexcept;
    std::shared_ptr<ResourceAccount> account_;
    std::size_t slot_ = 0;
};

class ResourceAccount : public std::enable_shared_from_this<ResourceAccount> {
  public:
    ResourceAccount(const ResourceAccount &) = delete;
    ResourceAccount &operator=(const ResourceAccount &) = delete;
    ResourceAccount(ResourceAccount &&) = delete;
    ResourceAccount &operator=(ResourceAccount &&) = delete;
    static Result<std::shared_ptr<ResourceAccount>> create(const AccountLimits &) noexcept;
    [[nodiscard]] static Result<ResourceBounds> metadata_bounds(const AccountLimits &) noexcept;
    Result<Reservation> reserve(ResourceKind, std::uint64_t bytes) noexcept;
    Result<HoldLease> hold(std::uint64_t allocation, std::string_view holder,
                           std::string_view reason) noexcept;
    Result<void> advance_age(std::uint64_t step) noexcept;
    Result<void> close_evidence() noexcept;
    void set_owner(std::optional<AllocationOwner> value) noexcept {
        owner_ = value;
    }
    [[nodiscard]] std::optional<AllocationOwner> owner_context() const noexcept {
        return owner_;
    }
    [[nodiscard]] std::optional<AllocationOwner> allocation_owner(std::uint64_t) const noexcept;
    [[nodiscard]] std::uint64_t current(ResourceKind kind) const noexcept {
        return current_[static_cast<std::size_t>(kind)];
    }
    [[nodiscard]] std::uint64_t peak(ResourceKind kind) const noexcept {
        return peak_[static_cast<std::size_t>(kind)];
    }
    [[nodiscard]] std::span<const RetentionEvent> events() const noexcept {
        return {events_.get(), event_count_};
    }
    [[nodiscard]] const std::optional<Diagnostic> &failure() const noexcept {
        return failure_;
    }

  private:
    friend class Reservation;
    friend class HoldLease;
    struct Allocation {
        bool active = false, warned = false;
        ResourceKind kind = ResourceKind::Graph;
        std::uint64_t id = 0, bytes = 0, created = 0;
        std::optional<AllocationOwner> owner{};
    };
    struct Holder {
        bool active = false;
        RetentionEvent event{};
    };
    explicit ResourceAccount(AccountLimits limits) : limits_(limits) {}
    Result<void> note(RetentionEvent) noexcept;
    void release(std::size_t slot) noexcept;
    void release_hold(std::size_t slot) noexcept;
    AccountLimits limits_;
    std::array<std::uint64_t, 7> bounds_{}, current_{}, peak_{};
    std::unique_ptr<Allocation[]> allocations_;
    std::unique_ptr<Holder[]> holders_;
    std::unique_ptr<RetentionEvent[]> events_;
    std::size_t event_count_ = 0;
    std::uint64_t next_id_ = 1, step_ = 0;
    bool closed_ = false;
    std::optional<Diagnostic> failure_;
    std::optional<AllocationOwner> owner_;
};

// Host-defined control/destruction code is compiled into the common core library.
// pin_ is destroyed last, after the plugin's entire typed destructor has returned.
class AllocationControl {
  public:
    AllocationControl(ModulePin, Reservation, Reservation, std::shared_ptr<ResourceAccount>, void *,
                      void (*)(void *) noexcept, std::type_index) noexcept;
    ~AllocationControl() noexcept;
    AllocationControl(const AllocationControl &) = delete;
    AllocationControl &operator=(const AllocationControl &) = delete;
    [[nodiscard]] const void *data() const noexcept {
        return data_;
    }
    [[nodiscard]] std::type_index type() const noexcept {
        return type_;
    }
    [[nodiscard]] std::uint64_t id() const noexcept {
        return payload_charge_.id();
    }
    [[nodiscard]] const auto &account() const noexcept {
        return account_;
    }

  private:
    template <class> friend class ReadValue;
    ModulePin pin_;
    Reservation payload_charge_, control_charge_;
    std::shared_ptr<ResourceAccount> account_;
    void *data_;
    void (*destroy_)(void *) noexcept;
    std::type_index type_;
};
// A conservative, versioned same-build control-block reservation. The allocator
// rejects an actual library control allocation exceeding this bound BEFORE new.
inline constexpr std::uint64_t ownership_control_reserve = sizeof(AllocationControl) + 256;
Result<std::shared_ptr<const AllocationControl>>
make_allocation_control(ModulePin, Reservation, Reservation, std::shared_ptr<ResourceAccount>,
                        void *, void (*)(void *) noexcept, std::type_index) noexcept;
Result<ModulePin> combine_pins(std::shared_ptr<ResourceAccount>, ModulePin, ModulePin) noexcept;

template <class T> class Immutable {
  public:
    Immutable() = default;
    Immutable(const Immutable &) = delete;
    Immutable &operator=(const Immutable &) = delete;
    Immutable(Immutable &&) noexcept = default;
    Immutable &operator=(Immutable &&) noexcept = default;
    [[nodiscard]] const T &get() const {
        if (!control_) {
            throw std::logic_error("empty immutable value");
        }
        return *static_cast<const T *>(control_->data());
    }
    [[nodiscard]] Result<Immutable> share(std::string_view holder,
                                          std::string_view reason) const noexcept {
        if (!control_) {
            return diagnostic(FailureCode::OutputInvalid, "cannot share empty value");
        }
        auto held = control_->account()->hold(control_->id(), holder, reason);
        if (const auto *error = std::get_if<Diagnostic>(&held)) {
            return *error;
        }
        return Immutable{control_, std::get<HoldLease>(std::move(held))};
    }

  private:
    friend class ErasedValue;
    friend class ResultSlot;
    friend class OutcomeView;
    template <class> friend class ReadValue;
    template <class U, class Factory>
    friend Result<Immutable<U>> make_owned(std::shared_ptr<ResourceAccount>, ResourceKind,
                                           std::uint64_t, ModulePin, Factory &&) noexcept;
    [[nodiscard]] const std::shared_ptr<const AllocationControl> &control() const noexcept {
        return control_;
    }
    static Result<Immutable> from_control(std::shared_ptr<const AllocationControl> control,
                                          std::string_view holder,
                                          std::string_view reason) noexcept {
        if (!control || control->type() != std::type_index(typeid(T))) {
            return diagnostic(FailureCode::TypeMismatch, "exact C++ type witness mismatch");
        }
        auto held = control->account()->hold(control->id(), holder, reason);
        if (const auto *error = std::get_if<Diagnostic>(&held)) {
            return *error;
        }
        return Immutable{std::move(control), std::get<HoldLease>(std::move(held))};
    }
    Immutable(std::shared_ptr<const AllocationControl> control, HoldLease hold) noexcept
        : control_(std::move(control)), hold_(std::move(hold)) {}
    std::shared_ptr<const AllocationControl> control_;
    HoldLease hold_;
};

template <class T, class Factory>
Result<Immutable<T>> make_owned(std::shared_ptr<ResourceAccount> account, ResourceKind kind,
                                std::uint64_t payload_bytes, ModulePin pin,
                                Factory &&factory) noexcept {
    static_assert(std::is_nothrow_destructible_v<T>);
    return guarded([&]() -> Result<Immutable<T>> {
        if (!account || payload_bytes < sizeof(T)) {
            return diagnostic(FailureCode::ResourceExceeded,
                              "insufficient declared payload reservation");
        }
        auto payload = account->reserve(kind, payload_bytes);
        if (const auto *error = std::get_if<Diagnostic>(&payload)) {
            return *error;
        }
        auto overhead = account->reserve(ResourceKind::Staging, ownership_control_reserve);
        if (const auto *error = std::get_if<Diagnostic>(&overhead)) {
            return *error;
        }
        // Construction can allocate only after both reservations succeed. For a
        // large type its registered measure/freeze contract covers owned buffers.
        std::unique_ptr<T> value{new T(std::invoke(std::forward<Factory>(factory)))};
        auto control = make_allocation_control(
            pin, std::get<Reservation>(std::move(payload)),
            std::get<Reservation>(std::move(overhead)), account, value.get(),
            [](void *pointer) noexcept { delete static_cast<T *>(pointer); },
            std::type_index(typeid(T)));
        if (const auto *error = std::get_if<Diagnostic>(&control)) {
            return *error;
        }
        value.release();
        return Immutable<T>::from_control(
            std::get<std::shared_ptr<const AllocationControl>>(std::move(control)), "producer",
            "publication");
    });
}

} // namespace lexongine
