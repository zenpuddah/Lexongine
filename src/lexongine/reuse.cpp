#include "lexongine/reuse.hpp"

namespace lexongine {
namespace {
struct ReuseFailure { Diagnostic error; };
template<class T> T checked(std::variant<T, Diagnostic> result) {
    if (const auto* error = std::get_if<Diagnostic>(&result)) { throw ReuseFailure{*error}; } return std::get<T>(std::move(result));
}
template<class F> auto protect(F&& function) noexcept -> decltype(function()) {
    return guarded([&]() -> decltype(function()) { try { return function(); } catch (const ReuseFailure& failure) { return failure.error; } });
}
}
struct PublicationHistory::Data {
    Reservation charge;
    std::shared_ptr<const ProducerAuthority> authority;
    std::vector<std::shared_ptr<const ParentCertificate>> certificates;
    std::size_t outputs, capacity, next = 0;
    std::uint64_t publications = 0;
    std::optional<std::uint64_t> last_sequence;
};
PublicationHistory::PublicationHistory(std::unique_ptr<Data> data) noexcept : data_(std::move(data)) {}
PublicationHistory::PublicationHistory(PublicationHistory&&) noexcept = default;
PublicationHistory& PublicationHistory::operator=(PublicationHistory&&) noexcept = default;
PublicationHistory::~PublicationHistory() noexcept = default;
Result<std::uint64_t> PublicationHistory::resource_bytes(std::uint64_t outputs, std::uint64_t preceding) noexcept {
    return protect([&]() -> Result<std::uint64_t> {
        if (outputs == 0) { return diagnostic(FailureCode::InvalidConfig, "publication history needs declared output ports"); }
        const auto count = checked(checked_multiply(outputs, checked(checked_add(preceding, 1))));
        if (count > std::vector<std::shared_ptr<const ParentCertificate>>{}.max_size()) { return diagnostic(FailureCode::ResourcePlanExceeded, "publication metadata ring cannot represent capacity"); }
        return checked_add(sizeof(Data) + 256, checked(checked_multiply(count, sizeof(std::shared_ptr<const ParentCertificate>))));
    });
}
Result<PublicationHistory> PublicationHistory::create(std::shared_ptr<ResourceAccount> account, const ResultSlot& slot, std::uint64_t preceding) noexcept {
    return protect([&]() -> Result<PublicationHistory> {
        if (!account) { return diagnostic(FailureCode::InvalidConfig, "publication history needs a run account"); }
        auto charge = checked(account->reserve(ResourceKind::State, checked(resource_bytes(slot.contracts_.size(), preceding))));
        const auto capacity = static_cast<std::size_t>(checked(checked_add(preceding, 1)));
        auto data = std::make_unique<Data>(Data{std::move(charge), slot.authority_,
            std::vector<std::shared_ptr<const ParentCertificate>>(capacity * slot.contracts_.size()), slot.contracts_.size(), capacity, 0, 0, {}});
        return PublicationHistory{std::move(data)};
    });
}
Result<void> PublicationHistory::capture(const ResultSlot& slot) noexcept {
    if (!data_ || data_->authority != slot.authority_ || data_->outputs != slot.contracts_.size()) { return diagnostic(FailureCode::UpdateProtocol, "publication history belongs to another producer"); }
    if (!slot.current_) { return diagnostic(FailureCode::MissingRequired, "publication history has no resolved update"); }
    const auto& frame = *slot.current_; auto& data = *data_;
    if (data.last_sequence && frame.context.key.sequence <= *data.last_sequence) { return diagnostic(FailureCode::UpdateProtocol, "duplicate or regressed history capture"); }
    if (frame.kind == OutcomeKind::Published) {
        if (frame.certificates.size() != data.outputs) { return diagnostic(FailureCode::OutputInvalid, "history capture has incomplete certificates"); }
        for (std::size_t i = 0; i < data.outputs; ++i) {
            const auto& certificate = frame.certificates[i];
            if (!certificate || certificate->authority != data.authority || certificate->publication_ordinal != data.publications || certificate->publication.port != i) {
                return diagnostic(FailureCode::UpdateProtocol, "history capture skipped or changed a publication");
            }
        }
        for (std::size_t i = 0; i < data.outputs; ++i) { data.certificates[data.next * data.outputs + i] = frame.certificates[i]; }
        data.next = (data.next + 1) % data.capacity; ++data.publications;
    } else if (frame.kind != OutcomeKind::Warming && frame.kind != OutcomeKind::ValidAbsence) { return diagnostic(FailureCode::OutputInvalid, "cannot capture failed publication history"); }
    data.last_sequence = frame.context.key.sequence; return ok();
}
Result<void> ProvenanceValidator::restore(PendingOutcome& pending, std::span<const ParentReference> references,
    std::span<const BoundInput> inputs, std::span<const PublicationHistory* const> histories,
    std::uint64_t preceding, const UpdateContext& context) noexcept {
    return protect([&]() -> Result<void> {
        if (!pending.frame_ || pending.frame_->kind != OutcomeKind::Published || inputs.size() != histories.size()) {
            return diagnostic(FailureCode::UpdateProtocol, "cached provenance requires a complete bound staging context");
        }
        std::uint64_t maximum = 0;
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            if (!inputs[i].slot || !histories[i] || !histories[i]->data_ || histories[i]->data_->authority != inputs[i].slot->authority_) {
                return diagnostic(FailureCode::CacheCorrupt, "cached parent history has foreign input authority");
            }
            maximum = checked(checked_add(maximum, checked(checked_add(std::max(preceding, inputs[i].prior_outputs), 1))));
        }
        if (references.size() > maximum) { return diagnostic(FailureCode::CacheCorrupt, "cached parents exceed declared selection capacity"); }
        auto scratch = checked(pending.frame_->account->reserve(ResourceKind::Scratch,
            checked(checked_add(256, checked(checked_multiply(references.size(), sizeof(ParentSelection)))))));
        std::vector<ParentSelection> selections; selections.reserve(references.size());
        for (const auto& reference : references) {
            if (reference.input_port >= inputs.size() || reference.publication.port != inputs[reference.input_port].port) {
                return diagnostic(FailureCode::CacheCorrupt, "cached parent names an undeclared input or output port");
            }
            const auto& history = *histories[reference.input_port]->data_;
            const auto found = std::find_if(history.certificates.begin(), history.certificates.end(), [&](const auto& certificate) {
                return certificate && certificate->publication == reference.publication;
            });
            if (found == history.certificates.end()) { return diagnostic(FailureCode::CacheCorrupt, "cached parent has no matching reached publication certificate"); }
            selections.push_back(ParentSelection{*found, reference.input_port});
        }
        auto parents = checked(ParentSelections::create(pending.frame_->account, maximum, selections));
        return attach(pending, std::move(parents), inputs, preceding, context);
    });
}
} // namespace lexongine
