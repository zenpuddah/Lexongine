#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace lexongine {

enum class FailureCode {
    InvalidConfig,
    UnknownDefinition,
    DuplicateDefinition,
    UnsupportedContract,
    GraphCycle,
    TypeMismatch,
    SourceInvalid,
    SourceCoverage,
    SourceChanged,
    SourceKnownBoundViolation,
    HistoryInsufficient,
    ResourcePlanExceeded,
    ResourceExceeded,
    UpdateProtocol,
    FutureRead,
    MissingRequired,
    NumericBound,
    NumericOverflow,
    CallbackException,
    OutputInvalid,
    EvidenceQuota,
    EvidenceIO,
    CacheCorrupt,
    CacheIO,
    CacheBusy,
    CommitUnconfirmed,
    AlreadyExecuted
};
struct DiagnosticKey {
    std::uint32_t clock_ordinal;
    std::uint64_t sequence;
    friend bool operator==(const DiagnosticKey &, const DiagnosticKey &) = default;
};
struct Diagnostic {
    FailureCode code;
    std::array<char, 256> message{};
    std::uint16_t length = 0;
    bool truncated = false;
    std::optional<std::uint32_t> logical_ordinal{};
    std::optional<std::uint32_t> request_ordinal{};
    std::optional<DiagnosticKey> key{};
    [[nodiscard]] std::string_view text() const noexcept {
        return {message.data(), length};
    }
};
[[nodiscard]] inline Diagnostic diagnostic(FailureCode code, std::string_view text) noexcept {
    Diagnostic result{code};
    result.length = static_cast<std::uint16_t>(std::min(text.size(), result.message.size()));
    std::copy_n(text.data(), result.length, result.message.data());
    result.truncated = text.size() > result.message.size();
    return result;
}
template <class T> struct ResultType {
    using type = std::variant<T, Diagnostic>;
};
template <> struct ResultType<void> {
    using type = std::variant<std::monostate, Diagnostic>;
};
template <class T> using Result = typename ResultType<T>::type;
inline Result<void> ok() noexcept {
    return std::monostate{};
}

// All author/provider callbacks enter through a host-owned exception boundary.
template <class F> auto guarded(F &&function) noexcept -> decltype(function()) {
    try {
        return function();
    } catch (const std::bad_alloc &) {
        return diagnostic(FailureCode::ResourceExceeded, "allocation failed");
    } catch (const std::exception &error) {
        return diagnostic(FailureCode::CallbackException, error.what());
    } catch (...) {
        return diagnostic(FailureCode::CallbackException, "non-standard callback exception");
    }
}

using LogicalId = std::string;
using RequestId = std::string;
using ClockId = std::string;
using DeclaredOrdinal = std::uint32_t;
using PortIndex = std::uint32_t;
using Sha256 = std::array<std::byte, 32>;
using ModulePin = std::shared_ptr<const void>;

[[nodiscard]] inline Result<void> validate_text(std::string_view value, std::uint64_t maximum,
                                                bool allow_empty) noexcept {
    if ((!allow_empty && value.empty()) || value.size() > maximum) {
        return diagnostic(FailureCode::InvalidConfig, "text length outside declared bound");
    }
    for (std::size_t i = 0; i < value.size();) {
        const auto first = static_cast<unsigned char>(value[i++]);
        if (first == 0) {
            return diagnostic(FailureCode::InvalidConfig, "NUL in text");
        }
        if (first < 0x80) {
            continue;
        }
        std::uint32_t code = 0;
        std::uint32_t minimum = 0;
        std::size_t continuation = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            code = first & 0x1f;
            minimum = 0x80;
            continuation = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            code = first & 0x0f;
            minimum = 0x800;
            continuation = 2;
        } else if (first >= 0xf0 && first <= 0xf4) {
            code = first & 0x07;
            minimum = 0x10000;
            continuation = 3;
        } else {
            return diagnostic(FailureCode::InvalidConfig, "invalid UTF-8 lead byte");
        }
        if (continuation > value.size() - i) {
            return diagnostic(FailureCode::InvalidConfig, "truncated UTF-8");
        }
        for (std::size_t j = 0; j < continuation; ++j) {
            const auto next = static_cast<unsigned char>(value[i++]);
            if ((next & 0xc0) != 0x80) {
                return diagnostic(FailureCode::InvalidConfig, "invalid UTF-8 continuation");
            }
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
            return diagnostic(FailureCode::InvalidConfig, "noncanonical UTF-8 code point");
        }
    }
    return ok();
}

struct UpdateKey {
    ClockId clock;
    std::uint64_t sequence;
    friend bool operator==(const UpdateKey &, const UpdateKey &) = default;
};
struct SequenceScope {
    ClockId clock;
    std::uint64_t begin, end;
    friend bool operator==(const SequenceScope &, const SequenceScope &) = default;
};
struct Interval {
    std::int64_t begin_ns, end_ns;
    friend bool operator==(const Interval &, const Interval &) = default;
};
struct PointTime {
    std::int64_t event_time_ns;
    friend bool operator==(const PointTime &, const PointTime &) = default;
};
using TemporalExtent = std::variant<PointTime, Interval>;
struct UpdateContext {
    UpdateKey key;
    TemporalExtent extent;
    std::int64_t decision_time_ns, activation_available_at_ns;
};
enum class Phase { Warmup, Evaluation };
struct RequestContext {
    RequestId request;
    SequenceScope evaluation;
    Phase phase;
    UpdateKey key;
};
inline Result<void> validate_context(const UpdateContext &context) noexcept {
    if (context.key.clock.empty()) {
        return diagnostic(FailureCode::UpdateProtocol, "empty update clock");
    }
    if (const auto *interval = std::get_if<Interval>(&context.extent);
        interval && interval->end_ns <= interval->begin_ns) {
        return diagnostic(FailureCode::UpdateProtocol, "invalid interval extent");
    }
    if (context.activation_available_at_ns > context.decision_time_ns) {
        return diagnostic(FailureCode::FutureRead, "activation is not yet available");
    }
    return ok();
}
inline Result<std::uint64_t> checked_add(std::uint64_t left, std::uint64_t right) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return diagnostic(FailureCode::ResourcePlanExceeded, "size addition overflow");
    }
    return left + right;
}
inline Result<std::uint64_t> checked_multiply(std::uint64_t left, std::uint64_t right) noexcept {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return diagnostic(FailureCode::ResourcePlanExceeded, "size multiplication overflow");
    }
    return left * right;
}

enum class Origin { ExplicitConfig, VersionedFact, Derived };
using ParamValue = std::variant<std::int64_t, std::uint64_t, bool, std::string>;
struct NamedParam {
    std::string name;
    ParamValue value;
    Origin origin;
    std::string provenance;
    friend bool operator==(const NamedParam &left, const NamedParam &right) {
        return left.name == right.name && left.value == right.value;
    }
};
struct CanonicalParams {
    std::uint32_t schema_version;
    std::vector<NamedParam> values;
    friend bool operator==(const CanonicalParams &, const CanonicalParams &) = default;
};
inline bool unsigned_text_less(std::string_view left, std::string_view right) noexcept {
    return std::lexicographical_compare(
        left.begin(), left.end(), right.begin(), right.end(), [](char a, char b) {
            return static_cast<unsigned char>(a) < static_cast<unsigned char>(b);
        });
}
inline Result<CanonicalParams> canonical_params(std::uint32_t version,
                                                std::vector<NamedParam> values,
                                                std::uint64_t string_bytes) noexcept {
    return guarded([&]() -> Result<CanonicalParams> {
        if (version == 0) {
            return diagnostic(FailureCode::InvalidConfig,
                              "parameter schema version must be explicit");
        }
        for (const auto &parameter : values) {
            if (auto check = validate_text(parameter.name, string_bytes, false);
                std::holds_alternative<Diagnostic>(check)) {
                return std::get<Diagnostic>(check);
            }
            if (const auto *text = std::get_if<std::string>(&parameter.value)) {
                if (auto check = validate_text(*text, string_bytes, true);
                    std::holds_alternative<Diagnostic>(check)) {
                    return std::get<Diagnostic>(check);
                }
            }
        }
        std::sort(values.begin(), values.end(),
                  [](const auto &a, const auto &b) { return unsigned_text_less(a.name, b.name); });
        for (std::size_t i = 1; i < values.size(); ++i) {
            if (values[i - 1].name == values[i].name) {
                return diagnostic(FailureCode::InvalidConfig, "duplicate parameter");
            }
        }
        return CanonicalParams{version, std::move(values)};
    });
}
struct SourceLocation {
    std::string path;
    std::uint64_t line;
};
struct NamedLiteral {
    std::string name, text;
    SourceLocation source_location;
};
struct ParamSet {
    std::vector<NamedLiteral> values;
};
enum class ParamKind { Signed64, Unsigned64, Boolean, Text };
struct ParameterConstraints {
    std::optional<std::int64_t> signed_min{}, signed_max{};
    std::optional<std::uint64_t> unsigned_min{}, unsigned_max{};
    std::vector<std::string> allowed_text{};
};
struct ParameterField {
    std::string name;
    ParamKind kind;
    bool required;
    ParameterConstraints constraints;
};
struct ParameterSchema {
    std::uint32_t version;
    std::vector<ParameterField> fields;
};

inline bool canonical_integer(std::string_view token, bool signed_value) noexcept {
    if (token == "0") {
        return true;
    }
    if (token.empty()) {
        return false;
    }
    if (token.front() == '-') {
        if (!signed_value) {
            return false;
        }
        token.remove_prefix(1);
    }
    if (token.empty() || token.front() < '1' || token.front() > '9') {
        return false;
    }
    return std::all_of(token.begin(), token.end(), [](char c) { return c >= '0' && c <= '9'; });
}
template <class T> Result<T> parse_integer(std::string_view token) noexcept {
    static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>);
    if (!canonical_integer(token, std::is_signed_v<T>)) {
        return diagnostic(FailureCode::InvalidConfig, "noncanonical integer");
    }
    T value{};
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) {
        return diagnostic(FailureCode::InvalidConfig, "integer out of range or trailing text");
    }
    return value;
}
inline Result<CanonicalParams> parse_parameters(const ParamSet &input,
                                                const ParameterSchema &schema,
                                                std::uint64_t string_bytes) noexcept {
    return guarded([&]() -> Result<CanonicalParams> {
        if (schema.version == 0) {
            return diagnostic(FailureCode::InvalidConfig, "missing parameter schema version");
        }
        std::vector<NamedParam> values;
        values.reserve(input.values.size());
        for (const auto &literal : input.values) {
            const auto field =
                std::find_if(schema.fields.begin(), schema.fields.end(),
                             [&](const auto &entry) { return entry.name == literal.name; });
            if (field == schema.fields.end()) {
                return diagnostic(FailureCode::InvalidConfig, "unknown parameter");
            }
            ParamValue value;
            switch (field->kind) {
            case ParamKind::Signed64: {
                auto result = parse_integer<std::int64_t>(literal.text);
                if (const auto *error = std::get_if<Diagnostic>(&result)) {
                    return *error;
                }
                const auto number = std::get<std::int64_t>(result);
                if ((field->constraints.signed_min && number < *field->constraints.signed_min) ||
                    (field->constraints.signed_max && number > *field->constraints.signed_max)) {
                    return diagnostic(FailureCode::InvalidConfig,
                                      "signed parameter outside declared domain");
                }
                value = number;
                break;
            }
            case ParamKind::Unsigned64: {
                auto result = parse_integer<std::uint64_t>(literal.text);
                if (const auto *error = std::get_if<Diagnostic>(&result)) {
                    return *error;
                }
                const auto number = std::get<std::uint64_t>(result);
                if ((field->constraints.unsigned_min &&
                     number < *field->constraints.unsigned_min) ||
                    (field->constraints.unsigned_max &&
                     number > *field->constraints.unsigned_max)) {
                    return diagnostic(FailureCode::InvalidConfig,
                                      "unsigned parameter outside declared domain");
                }
                value = number;
                break;
            }
            case ParamKind::Boolean:
                if (literal.text != "true" && literal.text != "false") {
                    return diagnostic(FailureCode::InvalidConfig, "invalid boolean");
                }
                value = literal.text == "true";
                break;
            case ParamKind::Text:
                if (!field->constraints.allowed_text.empty() &&
                    std::find(field->constraints.allowed_text.begin(),
                              field->constraints.allowed_text.end(),
                              literal.text) == field->constraints.allowed_text.end()) {
                    return diagnostic(FailureCode::InvalidConfig, "unsupported text policy");
                }
                value = literal.text;
                break;
            }
            values.push_back({literal.name, std::move(value), Origin::ExplicitConfig,
                              literal.source_location.path + ":" +
                                  std::to_string(literal.source_location.line)});
        }
        for (const auto &field : schema.fields) {
            if (!field.required) {
                return diagnostic(FailureCode::UnsupportedContract,
                                  "optional implicit parameters unsupported");
            }
            if (std::none_of(values.begin(), values.end(),
                             [&](const auto &value) { return value.name == field.name; })) {
                return diagnostic(FailureCode::InvalidConfig, "missing required parameter");
            }
        }
        return canonical_params(schema.version, std::move(values), string_bytes);
    });
}

struct Contract {
    std::string id;
    std::uint32_t version;
    CanonicalParams parameters;
    friend bool operator==(const Contract &, const Contract &) = default;
};
struct TypeContract {
    std::string nominal_name;
    std::uint32_t semantic_version;
    CanonicalParams meaning;
    friend bool operator==(const TypeContract &, const TypeContract &) = default;
};
struct OperationKey {
    std::string name_space, name;
    std::uint32_t semantic_version;
    std::string implementation_selector;
    friend bool operator==(const OperationKey &, const OperationKey &) = default;
};
struct ImplementationIdentity {
    Sha256 binary_digest, dependencies_digest, source_build_digest;
    std::string sdk_build_contract_id;
    friend bool operator==(const ImplementationIdentity &,
                           const ImplementationIdentity &) = default;
};
struct InitializationSpec {
    Contract contract;
    SequenceScope actual_prefix;
    CanonicalParams explicit_values;
    friend bool operator==(const InitializationSpec &, const InitializationSpec &) = default;
};
struct CountHistory {
    std::uint64_t preceding;
    friend bool operator==(const CountHistory &, const CountHistory &) = default;
};
struct TimeHistory {
    std::int64_t preceding_ns;
    friend bool operator==(const TimeHistory &, const TimeHistory &) = default;
};
struct ExplicitPrefix {
    Sha256 identity;
    SequenceScope scope;
    friend bool operator==(const ExplicitPrefix &, const ExplicitPrefix &) = default;
};
using HistoryRequirement = std::variant<CountHistory, TimeHistory, ExplicitPrefix>;
struct ResourceBounds {
    std::uint64_t source, state, scratch, staging, retention, graph, evidence;
    [[nodiscard]] Result<std::uint64_t> total() const noexcept {
        std::uint64_t sum = 0;
        for (const auto value : {source, state, scratch, staging, retention, graph, evidence}) {
            auto next = checked_add(sum, value);
            if (const auto *error = std::get_if<Diagnostic>(&next)) {
                return *error;
            }
            sum = std::get<std::uint64_t>(next);
        }
        return sum;
    }
};
enum class MissingIntervalPolicy { Strict, Skip };
enum class Alignment { SameStep };
enum class CountBasis { CompleteBarUpdates };
enum class AbsencePolicy { RequireValue };
enum class StorageKind { ScalarCopy, ImmutableOwned };
enum class Driver { Standalone, Hosted };
enum class RunLifecycle { Created, Running, AwaitingCompletion, Finalizing, Terminal };
enum class NumericalOutcome { Completed, Failed, Aborted };
enum class IntegrationOutcome { NotFinished, Completed, Failed, Aborted };
enum class AcceptanceStatus { Pending, Accepted, Rejected, Unconfirmed };
enum class CachePublicationStatus {
    Disabled,
    NoNewEntries,
    Bypassed,
    Staged,
    Published,
    NotPublished,
    Unconfirmed
};
enum class StepStatus { Processed, Gap, Exhausted, Stopped };
enum class OutcomeKind { Published, Warming, ValidAbsence, MissingRequired, Failure };
struct Warming {
    std::uint64_t remaining_preceding;
};
struct ValidAbsence {
    std::uint32_t declared_reason;
};
struct ParentPublication;
struct ParentCertificate;
struct ParentSelectionBuffer;
class ProvenanceValidator;
class ParentSelection {
  public:
    [[nodiscard]] const ParentPublication &metadata() const noexcept;
    [[nodiscard]] std::optional<PortIndex> input_port() const noexcept {
        return input_;
    }

  private:
    friend class ResultSlot;
    friend class ProvenanceValidator;
    template <class> friend class ReadValue;
    ParentSelection(std::shared_ptr<const ParentCertificate> certificate,
                    std::optional<PortIndex> input) noexcept
        : certificate_(std::move(certificate)), input_(input) {}
    std::shared_ptr<const ParentCertificate> certificate_;
    std::optional<PortIndex> input_;
};
class ParentSelections {
  public:
    [[nodiscard]] static Result<std::uint64_t> storage_bytes(std::uint64_t count) noexcept;
    [[nodiscard]] static ParentSelections none() noexcept {
        return ParentSelections{nullptr};
    }
    [[nodiscard]] std::span<const ParentSelection> entries() const noexcept;

  private:
    friend class PublicationAllocator;
    friend class ProvenanceValidator;
    static Result<ParentSelections> create(std::shared_ptr<class ResourceAccount>,
                                           std::uint64_t maximum,
                                           std::span<const ParentSelection>) noexcept;
    explicit ParentSelections(std::shared_ptr<const ParentSelectionBuffer> data) noexcept
        : data_(std::move(data)) {}
    std::shared_ptr<const ParentSelectionBuffer> data_;
};
template <class T> struct Published {
    T outputs;
    ParentSelections parents;
};
template <class T> using StepResult = std::variant<Published<T>, Warming, ValidAbsence, Diagnostic>;
struct SourceGap {
    SequenceScope missing_scope;
    Interval interval;
    std::uint32_t declared_reason;
    friend bool operator==(const SourceGap &, const SourceGap &) = default;
};
struct SourceEnd {
    SequenceScope completed_scope;
};
struct CompleteRecord {
    std::uint64_t payload_index;
};
struct DeclaredGap {
    SourceGap gap;
};
struct TimingDescriptor {
    UpdateKey key;
    TemporalExtent extent;
    std::int64_t available_at_ns;
    std::variant<CompleteRecord, DeclaredGap> delivery;
};
struct SourceCoverage {
    SequenceScope expected_scope;
    std::vector<UpdateKey> complete_keys;
    std::vector<SourceGap> gaps;
    Contract expected_timeline_contract;
    friend bool operator==(const SourceCoverage &, const SourceCoverage &) = default;
};
struct SourceRequest {
    std::string provider_name, snapshot_name, requested_version;
    TypeContract interpretation;
    SequenceScope required_scope;
    CanonicalParams provider_policies;
};
struct SourceBounds {
    SequenceScope available_scope;
    ResourceBounds resources;
};
struct InputBindingSpec {
    std::string input_name;
    LogicalId producer;
    std::string output_name;
    std::uint64_t prior_outputs;
    Alignment alignment;
    MissingIntervalPolicy missing_intervals;
};
struct ObservationRequest {
    MissingIntervalPolicy missing_intervals;
    RequestId id;
    LogicalId producer;
    std::string output_name;
    SequenceScope evaluation;
    AbsencePolicy absence;
    std::uint64_t prior_outputs;
    std::string label;
};
struct NodeSpec {
    LogicalId id;
    std::string label;
    DeclaredOrdinal ordinal;
    OperationKey operation;
    CanonicalParams parameters;
    std::vector<InputBindingSpec> inputs;
    InitializationSpec initialization;
    Contract update_policy, availability_policy, effect_policy;
};
struct IntegrationCompletion {
    IntegrationOutcome kind;
    std::optional<Diagnostic> cause;
    std::string host_contract_id;
    std::uint32_t host_contract_version;
    Sha256 host_experiment_identity;
};

struct ParentPublication {
    Sha256 computation_digest;
    UpdateKey key;
    PortIndex port;
    TemporalExtent extent;
    std::int64_t available_at_ns;
    std::optional<Sha256> source_reference_digest;
    friend bool operator==(const ParentPublication &, const ParentPublication &) = default;
};
struct ParentReference {
    PortIndex input_port;
    ParentPublication publication;
    friend bool operator==(const ParentReference &, const ParentReference &) = default;
};
struct PublicationMetadata {
    UpdateKey key;
    TemporalExtent extent;
    std::int64_t available_at_ns;
    std::vector<ParentReference> parents;
    std::vector<TypeContract> output_types;
};

} // namespace lexongine
