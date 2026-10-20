#pragma once

#include "lexongine/execution.hpp"
#include "lexongine/schema.hpp"
#include <tinyxml2.h>

namespace lexongine::wire {
struct Limits {
    std::uint64_t bytes, elements, depth, string_bytes, workspace_bytes;
};
struct Error {
    Diagnostic diagnostic;
};
[[noreturn]] void fail(std::string_view);
template <class T> T checked(std::variant<T, Diagnostic> value) {
    if (const auto *error = std::get_if<Diagnostic>(&value)) {
        throw Error{*error};
    }
    return std::get<T>(std::move(value));
}
inline void checked(Result<void> value) {
    if (const auto *error = std::get_if<Diagnostic>(&value)) {
        throw Error{*error};
    }
}
template <class F> auto protect(F &&function) noexcept -> decltype(function()) {
    return guarded([&]() -> decltype(function()) {
        try {
            return function();
        } catch (const Error &error) {
            return error.diagnostic;
        }
    });
}
using Element = tinyxml2::XMLElement;
struct EncodedXml {
    Reservation charge;
    std::string value;
    [[nodiscard]] std::string_view bytes() const noexcept {
        return value;
    }
};
class Document {
  public:
    static Result<Document> create(std::shared_ptr<ResourceAccount>, Limits) noexcept;
    static Result<Document> parse(std::string_view, std::shared_ptr<ResourceAccount>,
                                  Limits) noexcept;
    Document(Document &&) noexcept = default;
    Document &operator=(Document &&) noexcept = default;
    [[nodiscard]] tinyxml2::XMLDocument &dom() noexcept {
        return *document_;
    }
    [[nodiscard]] const Element &root() const;
    Element *element(std::string_view name);
    Element *text(std::string_view);
    Element *u64(std::uint64_t);
    Element *i64(std::int64_t);
    Element *boolean(bool);
    Element *digest(const Sha256 &);
    Element *list();
    void item(Element *, Element *);
    Element *record(std::string_view,
                    std::initializer_list<std::pair<std::string_view, Element *>>);
    Element *optional(Element *);
    Element *variant(std::string_view, Element *);
    Element *copy(const Element &);
    void attribute(Element *, std::string_view, std::string_view);
    void attribute(Element *, std::string_view, std::uint64_t);
    [[nodiscard]] Result<EncodedXml> render() const noexcept;
    [[nodiscard]] const Limits &limits() const noexcept {
        return limits_;
    }

  private:
    Document(Reservation charge, Limits limits, std::unique_ptr<tinyxml2::XMLDocument> document,
             std::shared_ptr<ResourceAccount> account)
        : charge_(std::move(charge)), limits_(limits), document_(std::move(document)),
          account_(std::move(account)) {}
    Reservation charge_;
    Limits limits_;
    std::unique_ptr<tinyxml2::XMLDocument> document_;
    std::shared_ptr<ResourceAccount> account_;
    std::uint64_t elements_ = 0, text_bytes_ = 0;
};
void attributes(const Element &, std::initializer_list<std::string_view>);
std::string_view attribute(const Element &, const char *);
template <class T> T number_attribute(const Element &e, const char *name) {
    auto value = parse_integer<T>(attribute(e, name));
    if (std::holds_alternative<Diagnostic>(value)) {
        fail("invalid metadata integer attribute");
    }
    return std::get<T>(value);
}
const Element &only_child(const Element &);
std::vector<const Element *> children(const Element &);
std::vector<const Element *> fields(const Element &, std::string_view type);
std::string text(const Element &);
std::uint64_t u64(const Element &);
std::int64_t i64(const Element &);
bool boolean(const Element &);
Sha256 digest(const Element &);
Sha256 digest_hex(std::string_view);
std::string hex(std::span<const std::byte>);
std::vector<std::byte> unhex(std::string_view, std::uint64_t maximum);
void validate_value(const Element &);
std::vector<std::byte> canonical(const Element &, std::uint16_t kind, std::uint64_t maximum);
Element *project(Document &, const CanonicalParams &);
Element *project(Document &, const Contract &);
Element *project(Document &, const TypeContract &);
Element *project(Document &, const SequenceScope &);
Element *project(Document &, const UpdateKey &);
Element *project(Document &, const Interval &);
Element *project(Document &, const TemporalExtent &);
Element *project(Document &, const SourceCoverage &);
Element *project(Document &, const InitializationSpec &);
Element *project(Document &, const EvidenceReference &);
CanonicalParams parameters(const Element &);
Contract contract(const Element &);
TypeContract type(const Element &);
SequenceScope scope(const Element &);
UpdateKey key(const Element &);
Interval interval(const Element &);
TemporalExtent extent(const Element &);
SourceCoverage coverage(const Element &);
InitializationSpec initialization(const Element &);
EvidenceReference evidence(const Element &);
} // namespace lexongine::wire
