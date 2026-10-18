#include "price_artifact.hpp"
#include "quant/lexongine/components.hpp"

#include <tinyxml2.h>

static_assert(TIXML2_MAJOR_VERSION == 11 && TIXML2_MINOR_VERSION == 0 && TIXML2_PATCH_VERSION == 0);
namespace lexongine {
namespace {
struct CodecFailure { Diagnostic error; };
[[noreturn]] void fail(FailureCode code, std::string_view message) { throw CodecFailure{diagnostic(code, message)}; }
[[noreturn]] void corrupt(std::string_view message) { fail(FailureCode::CacheCorrupt, message); }
template<class T> T checked(std::variant<T, Diagnostic> result) {
    if (const auto* error = std::get_if<Diagnostic>(&result)) { throw CodecFailure{*error}; } return std::get<T>(std::move(result));
}
template<class F> auto protect(F&& function) noexcept -> decltype(function()) {
    return guarded([&]() -> decltype(function()) { try { return function(); } catch (const CodecFailure& failure) { return failure.error; } });
}
bool whitespace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool name_character(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'; }
bool xml_character(std::uint32_t c) { return c == 9 || c == 10 || c == 13 || (c >= 0x20 && c <= 0xd7ff) || (c >= 0xe000 && c <= 0xfffd) || (c >= 0x10000 && c <= 0x10ffff); }
void text_valid(std::string_view value, std::uint64_t limit, bool empty = false) {
    if (std::holds_alternative<Diagnostic>(validate_text(value, limit, empty))) { corrupt("invalid or oversized UTF-8 payload text"); }
    for (std::size_t i = 0; i < value.size(); ++i) {
        if ((static_cast<unsigned char>(value[i]) < 0x20 && !whitespace(value[i])) || value.substr(i, 3) == "\xef\xbf\xbe" || value.substr(i, 3) == "\xef\xbf\xbf") { corrupt("invalid XML 1.0 character"); }
    }
}
struct Counts { std::uint64_t elements = 0, attributes = 0, outcomes = 0, parents = 0, ports = 0; };
// Exact, attribute-only price payload grammar. No general document reader.
Counts preflight(std::string_view xml, const PricePayloadLimits& limits) {
    text_valid(xml, limits.xml_bytes);
    std::size_t position = xml.starts_with("\xef\xbb\xbf") ? 3 : 0, depth = 0;
    std::array<std::string_view, 4> stack{}; bool root_seen = false; Counts counts;
    auto space = [&] { while (position < xml.size() && whitespace(xml[position])) { ++position; } };
    auto name = [&] {
        const auto begin = position; while (position < xml.size() && name_character(xml[position])) { ++position; }
        if (position == begin || (position < xml.size() && xml[position] == ':')) { corrupt("invalid or namespaced payload name"); }
        return xml.substr(begin, position - begin);
    };
    auto quoted = [&] {
        if (position == xml.size() || (xml[position] != '"' && xml[position] != '\'')) { corrupt("payload attribute requires quotes"); }
        const auto quote = xml[position++]; const auto begin = position; std::uint64_t bytes = 0;
        while (position < xml.size() && xml[position] != quote) {
            if (xml[position] == '<') { corrupt("invalid payload attribute character"); }
            if (xml[position] == '&') {
                const auto end = xml.find(';', position + 1); if (end == std::string_view::npos) { corrupt("unterminated payload entity"); }
                auto entity = xml.substr(position + 1, end - position - 1); std::uint64_t length = 1;
                if (entity.starts_with('#')) {
                    entity.remove_prefix(1); int base = 10; if (entity.starts_with('x')) { entity.remove_prefix(1); base = 16; }
                    std::uint32_t scalar = 0; const auto parsed = std::from_chars(entity.data(), entity.data() + entity.size(), scalar, base);
                    if (entity.empty() || parsed.ec != std::errc{} || parsed.ptr != entity.data() + entity.size() || !xml_character(scalar)) { corrupt("invalid payload character reference"); }
                    length = scalar < 0x80 ? 1 : scalar < 0x800 ? 2 : scalar < 0x10000 ? 3 : 4;
                } else if (entity != "amp" && entity != "lt" && entity != "gt" && entity != "apos" && entity != "quot") { corrupt("unknown payload entity"); }
                bytes = checked(checked_add(bytes, length)); position = end + 1;
            } else { ++bytes; ++position; }
            if (bytes > limits.string_bytes) { corrupt("payload attribute exceeds text bound"); }
        }
        if (position == xml.size()) { corrupt("unterminated payload attribute"); }
        const auto value = xml.substr(begin, position - begin); ++position; return value;
    };
    if (xml.substr(position).starts_with("<?xml")) {
        position += 5; bool version = false, encoding = false, standalone = false;
        for (;;) {
            const auto before = position; space();
            if (xml.substr(position).starts_with("?>")) { position += 2; break; }
            if (before == position) { corrupt("invalid payload XML declaration"); }
            const auto attribute = name(); space();
            if (position == xml.size() || xml[position++] != '=') { corrupt("invalid declaration attribute"); }
            space(); const auto value = quoted();
            if (attribute == "version" && !version && !encoding && !standalone && value == "1.0") { version = true; }
            else if (attribute == "encoding" && version && !encoding && !standalone && (value == "UTF-8" || value == "utf-8")) { encoding = true; }
            else if (attribute == "standalone" && version && !standalone && (value == "yes" || value == "no")) { standalone = true; }
            else { corrupt("unsupported payload XML declaration"); }
        }
        if (!version) { corrupt("missing XML declaration version"); }
    }
    while (position < xml.size()) {
        space(); if (position == xml.size()) { break; }
        if (xml.substr(position).starts_with("<!--")) {
            const auto end = xml.find("-->", position + 4);
            if (end == std::string_view::npos || end - position - 4 > limits.string_bytes || xml.substr(position + 4, end - position - 4).find("--") != std::string_view::npos) { corrupt("invalid payload comment"); }
            position = end + 3; continue;
        }
        if (xml[position++] != '<' || position == xml.size() || xml[position] == '!' || xml[position] == '?') { corrupt("unsupported payload text or declaration"); }
        if (xml[position] == '/') {
            ++position; const auto close = name(); space();
            if (depth == 0 || stack[depth - 1] != close || position == xml.size() || xml[position++] != '>') { corrupt("mismatched payload close"); }
            --depth; continue;
        }
        const auto tag = name(); const auto parent = depth ? stack[depth - 1] : std::string_view{};
        const bool allowed = (parent.empty() && !root_seen && tag == "price_outcomes") || (parent == "price_outcomes" && tag == "outcome") ||
            (parent == "outcome" && (tag == "port" || tag == "parents")) || (parent == "parents" && tag == "parent");
        if (!allowed || depth == stack.size() || ++counts.elements > limits.xml_elements) { corrupt("unknown, misplaced or excessive payload element"); }
        if (parent.empty()) { root_seen = true; }
        counts.outcomes += tag == "outcome"; counts.parents += tag == "parent"; counts.ports += tag == "port";
        if (counts.outcomes > limits.outcomes) { corrupt("payload outcome count exceeds bound"); }
        std::array<std::string_view, 16> attributes{}; std::size_t count = 0; bool self_closed = false, terminated = false;
        while (position < xml.size()) {
            const auto before = position; space();
            if (xml.substr(position).starts_with("/>")) { position += 2; self_closed = terminated = true; break; }
            if (position < xml.size() && xml[position] == '>') { ++position; terminated = true; break; }
            if (position == before || count == attributes.size()) { corrupt("invalid payload attribute separation/count"); }
            const auto attribute = name();
            if (attribute == "xmlns" || std::find(attributes.begin(), attributes.begin() + static_cast<std::ptrdiff_t>(count), attribute) != attributes.begin() + static_cast<std::ptrdiff_t>(count)) { corrupt("duplicate or namespace payload attribute"); }
            attributes[count++] = attribute; ++counts.attributes; space();
            if (position == xml.size() || xml[position++] != '=') { corrupt("missing payload attribute equals sign"); }
            space(); quoted();
        }
        if (!terminated) { corrupt("unterminated payload element"); }
        if (!self_closed) { stack[depth++] = tag; }
    }
    if (!root_seen || depth != 0) { corrupt("incomplete price payload"); }
    return counts;
}
using Element = tinyxml2::XMLElement;
std::string_view attribute(const Element& element, const char* name) {
    const char* value = element.Attribute(name); if (!value) { corrupt("missing required payload attribute"); } return value;
}
template<class T> T number(const Element& element, const char* name) {
    auto result = parse_integer<T>(attribute(element, name)); if (std::holds_alternative<Diagnostic>(result)) { corrupt("invalid canonical payload integer"); } return std::get<T>(result);
}
void attributes(const Element& element, std::initializer_list<std::string_view> required,
    std::initializer_list<std::string_view> optional = {}, bool timed = false) {
    auto contains = [](auto list, std::string_view name) { return std::find(list.begin(), list.end(), name) != list.end(); };
    const auto kind = timed ? attribute(element, "time_kind") : std::string_view{};
    if (timed && kind != "point" && kind != "interval") { corrupt("unknown payload temporal kind"); }
    const std::initializer_list<std::string_view> timing = kind == "point" ?
        std::initializer_list<std::string_view>{"time_kind", "event_time_ns", "available_at_ns"} :
        std::initializer_list<std::string_view>{"time_kind", "interval_begin_ns", "interval_end_ns", "available_at_ns"};
    for (const auto key : required) { if (!element.Attribute(key.data())) { corrupt("missing required payload field"); } }
    if (timed) { for (const auto key : timing) { if (!element.Attribute(key.data())) { corrupt("missing required temporal field"); } } }
    for (auto* item = element.FirstAttribute(); item; item = item->Next()) {
        if (!contains(required, item->Name()) && !contains(optional, item->Name()) && !(timed && contains(timing, item->Name()))) { corrupt("unknown or wrong-kind payload field"); }
    }
}
TemporalExtent extent(const Element& element) {
    if (attribute(element, "time_kind") == "point") { return PointTime{number<std::int64_t>(element, "event_time_ns")}; }
    const Interval interval{number<std::int64_t>(element, "interval_begin_ns"), number<std::int64_t>(element, "interval_end_ns")};
    if (interval.end_ns <= interval.begin_ns) { corrupt("invalid payload interval"); } return interval;
}
Sha256 digest(std::string_view text) {
    if (text.size() != 64 || !std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) { corrupt("payload digest must be lowercase64hex"); }
    Sha256 value{}; for (std::size_t i = 0; i < value.size(); ++i) { unsigned byte = 0; const auto result = std::from_chars(text.data() + 2 * i, text.data() + 2 * i + 2, byte, 16); if (result.ec != std::errc{}) { corrupt("invalid payload digest"); } value[i] = static_cast<std::byte>(byte); }
    return value;
}
void contract_valid(const PricePayloadContract& contract, const PricePayloadLimits& limits) {
    if (contract.scope.end < contract.scope.begin || contract.outputs.empty() || contract.outputs.size() > std::numeric_limits<PortIndex>::max() || contract.complete_keys.size() > limits.outcomes) { fail(FailureCode::InvalidConfig, "invalid explicit Price payload contract"); }
    text_valid(contract.scope.clock, limits.string_bytes); const auto price = quant::lexongine::price_contract();
    for (const auto& output : contract.outputs) {
        text_valid(output.name, limits.string_bytes);
        if (!(output.type == price) || output.storage != StorageKind::ScalarCopy || output.codec != Contract{"qaedalus.price9_xml", 1, {1, {}}} ||
            output.validation != Contract{"qaedalus.price9", 1, {1, {}}} || !output.cacheable) { fail(FailureCode::UnsupportedContract, "unsupported exact Price9 output codec contract"); }
    }
    std::optional<std::uint64_t> previous;
    for (const auto& key : contract.complete_keys) {
        if (key.clock != contract.scope.clock || key.sequence < contract.scope.begin || key.sequence >= contract.scope.end || (previous && key.sequence <= *previous)) { fail(FailureCode::InvalidConfig, "invalid explicit complete-key coverage"); } previous = key.sequence;
    }
}
void record_valid(const PriceOutcomeRecord& record, const PricePayloadContract& contract, const PricePayloadLimits& limits, std::size_t index) {
    if (index >= contract.complete_keys.size() || record.key != contract.complete_keys[index]) { corrupt("payload keys do not exactly match complete coverage"); }
    if (const auto* interval = std::get_if<Interval>(&record.extent); interval && interval->end_ns <= interval->begin_ns) { corrupt("invalid outcome interval"); }
    if (record.kind == OutcomeKind::Published) {
        if (record.values.size() != contract.outputs.size() || record.remaining_preceding != 0 || record.declared_reason != 0) { corrupt("published payload bundle/status mismatch"); }
    } else if (record.kind == OutcomeKind::Warming) {
        if (!record.values.empty() || record.declared_reason != 0) { corrupt("warming payload contains values or absence reason"); }
    } else if (record.kind == OutcomeKind::ValidAbsence) {
        if (!record.values.empty() || record.remaining_preceding != 0) { corrupt("absent payload contains values or warming count"); }
    } else { corrupt("failure or unresolved outcome is not reusable"); }
    if (record.parents.size() > limits.parents_per_outcome) { corrupt("payload parent count exceeds explicit bound"); }
    const ParentReference* previous = nullptr;
    for (const auto& parent : record.parents) {
        const auto& publication = parent.publication;
        if (publication.computation_digest == Sha256{} || publication.key.clock != record.key.clock || publication.key.sequence < contract.scope.begin ||
            publication.key.sequence > record.key.sequence || publication.available_at_ns > record.available_at_ns ||
            (publication.source_reference_digest && *publication.source_reference_digest == Sha256{})) { corrupt("invalid or future payload parent"); }
        if (const auto* interval = std::get_if<Interval>(&publication.extent); interval && interval->end_ns <= interval->begin_ns) { corrupt("invalid parent interval"); }
        if ((!previous && parent.input_port != 0) || (previous && (parent.input_port < previous->input_port ||
            (parent.input_port != previous->input_port && static_cast<std::uint64_t>(parent.input_port) != static_cast<std::uint64_t>(previous->input_port) + 1) ||
            (parent.input_port == previous->input_port && publication.key.sequence <= previous->publication.key.sequence)))) { corrupt("missing, reordered or duplicate parent input group"); }
        previous = &parent;
    }
}
class Writer {
public:
    Writer(std::uint64_t maximum, std::uint64_t strings, std::string* output = nullptr) : maximum_(maximum), string_limit_(strings), output_(output) {}
    void append(std::string_view value) {
        if (value.size() > maximum_ - size_) { fail(FailureCode::ResourcePlanExceeded, "encoded price payload exceeds explicit byte bound"); }
        if (output_) { std::copy(value.begin(), value.end(), output_->begin() + static_cast<std::ptrdiff_t>(size_)); } size_ += value.size();
    }
    template<class T> void number(T value) { std::array<char, 32> text{}; const auto result = std::to_chars(text.data(), text.data() + text.size(), value);
        const auto length = static_cast<std::size_t>(result.ptr - text.data()); if (length > string_limit_) { corrupt("encoded integer exceeds attribute text bound"); } append({text.data(), length}); }
    void quoted(std::string_view value) {
        text_valid(value, string_limit_, true);
        append("\""); for (const char c : value) {
            if (c == '&') { append("&amp;"); } else if (c == '<') { append("&lt;"); } else if (c == '>') { append("&gt;"); }
            else if (c == '"') { append("&quot;"); } else if (c == '\'') { append("&apos;"); }
            else if (c == '\t') { append("&#9;"); } else if (c == '\n') { append("&#10;"); } else if (c == '\r') { append("&#13;"); } else { append({&c, 1}); }
        } append("\"");
    }
    void attr(std::string_view name, std::string_view value) { append(" "); append(name); append("="); quoted(value); }
    template<class T> void integer(std::string_view name, T value) { append(" "); append(name); append("=\""); number(value); append("\""); }
    void hash(std::string_view name, const Sha256& value) { constexpr std::string_view hex = "0123456789abcdef"; std::array<char, 64> text{}; for (std::size_t i = 0; i < value.size(); ++i) { auto byte = std::to_integer<unsigned>(value[i]); text[2 * i] = hex[byte >> 4]; text[2 * i + 1] = hex[byte & 15]; } attr(name, {text.data(), text.size()}); }
    void timing(const TemporalExtent& value, std::int64_t available) {
        if (const auto* point = std::get_if<PointTime>(&value)) { attr("time_kind", "point"); integer("event_time_ns", point->event_time_ns); }
        else { const auto& interval = std::get<Interval>(value); attr("time_kind", "interval"); integer("interval_begin_ns", interval.begin_ns); integer("interval_end_ns", interval.end_ns); }
        integer("available_at_ns", available);
    }
    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
private:
    std::uint64_t maximum_, string_limit_, size_ = 0; std::string* output_;
};
void encode(Writer& writer, std::span<const PriceOutcomeRecord> records, const PricePayloadContract& contract) {
    writer.append("<price_outcomes version=\"1\""); writer.attr("clock", contract.scope.clock); writer.integer("begin_step", contract.scope.begin); writer.integer("end_step", contract.scope.end); writer.append(">\n");
    for (const auto& record : records) {
        writer.append("<outcome"); writer.integer("step", record.key.sequence);
        writer.attr("kind", record.kind == OutcomeKind::Published ? "published" : record.kind == OutcomeKind::Warming ? "warming" : "valid_absence");
        writer.timing(record.extent, record.available_at_ns);
        if (record.kind == OutcomeKind::Warming) { writer.integer("remaining_preceding", record.remaining_preceding); }
        if (record.kind == OutcomeKind::ValidAbsence) { writer.integer("declared_reason", record.declared_reason); }
        writer.append(">\n");
        for (std::size_t i = 0; i < record.values.size(); ++i) {
            writer.append("<port"); writer.attr("name", contract.outputs[i].name); writer.attr("type", contract.outputs[i].type.nominal_name);
            writer.integer("type_version", contract.outputs[i].type.semantic_version); writer.integer("scale", 9); writer.integer("raw", record.values[i].raw()); writer.append("/>\n");
        }
        writer.append("<parents>\n");
        for (const auto& parent : record.parents) {
            const auto& publication = parent.publication; writer.append("<parent"); writer.integer("input_port", parent.input_port);
            writer.hash("computation_digest", publication.computation_digest); writer.attr("clock", publication.key.clock); writer.integer("step", publication.key.sequence);
            writer.integer("port", publication.port); writer.timing(publication.extent, publication.available_at_ns);
            if (publication.source_reference_digest) { writer.hash("source_reference_digest", *publication.source_reference_digest); } writer.append("/>\n");
        }
        writer.append("</parents></outcome>\n");
    }
    writer.append("</price_outcomes>\n");
}
} // namespace
struct DecodedPricePayload::Data { Reservation charge; SequenceScope scope; std::vector<PriceOutcomeRecord> outcomes; };
const SequenceScope& DecodedPricePayload::scope() const noexcept { return data_->scope; }
std::span<const PriceOutcomeRecord> DecodedPricePayload::outcomes() const noexcept { return data_->outcomes; }
Result<DecodedPricePayload> decode_price_payload(std::string_view xml, const PricePayloadContract& contract,
    std::shared_ptr<ResourceAccount> account, const PricePayloadLimits& limits) noexcept {
    return protect([&]() -> Result<DecodedPricePayload> {
        if (!account) { return diagnostic(FailureCode::InvalidConfig, "payload decoder requires an account"); }
        const auto counts = preflight(xml, limits);
        const auto workspace = checked(checked_add(65536, checked(checked_add(checked(checked_multiply(xml.size(), 2)),
            checked(checked_add(checked(checked_multiply(counts.elements, 256)), checked(checked_multiply(counts.attributes, 128))))))));
        if (workspace > limits.scratch_bytes) { return diagnostic(FailureCode::ResourcePlanExceeded, "payload parser exceeds explicit scratch ceiling"); }
        auto scratch = checked(account->reserve(ResourceKind::Scratch, workspace)); contract_valid(contract, limits);
        auto bytes = checked(checked_add(sizeof(DecodedPricePayload::Data) + 256, contract.scope.clock.size() + 32));
        bytes = checked(checked_add(bytes, checked(checked_multiply(counts.outcomes, sizeof(PriceOutcomeRecord) + contract.scope.clock.size() + 32))));
        bytes = checked(checked_add(bytes, checked(checked_multiply(counts.parents, sizeof(ParentReference) + contract.scope.clock.size() + 32))));
        bytes = checked(checked_add(bytes, checked(checked_multiply(counts.ports, sizeof(quant::data::Price)))));
        if (bytes > limits.decoded_bytes) { return diagnostic(FailureCode::ResourcePlanExceeded, "decoded payload exceeds explicit byte ceiling"); }
        auto charge = checked(account->reserve(ResourceKind::Graph, bytes));
        tinyxml2::XMLDocument document;
        if (document.Parse(xml.data(), xml.size()) != tinyxml2::XML_SUCCESS) { corrupt("malformed price payload XML"); }
        const auto* root = document.RootElement(); if (!root || std::string_view{root->Name()} != "price_outcomes") { corrupt("missing price payload root"); }
        attributes(*root, {"version", "clock", "begin_step", "end_step"});
        if (attribute(*root, "version") != "1" || attribute(*root, "clock") != contract.scope.clock || number<std::uint64_t>(*root, "begin_step") != contract.scope.begin || number<std::uint64_t>(*root, "end_step") != contract.scope.end) { corrupt("price payload version or exact scope mismatch"); }
        auto data = std::make_shared<DecodedPricePayload::Data>(DecodedPricePayload::Data{std::move(charge), contract.scope, {}}); data->outcomes.reserve(static_cast<std::size_t>(counts.outcomes));
        for (const auto* element = root->FirstChildElement(); element; element = element->NextSiblingElement()) {
            const auto kind = attribute(*element, "kind");
            if (kind == "published") { attributes(*element, {"step", "kind"}, {}, true); }
            else if (kind == "warming") { attributes(*element, {"step", "kind", "remaining_preceding"}, {}, true); }
            else if (kind == "valid_absence") { attributes(*element, {"step", "kind", "declared_reason"}, {}, true); }
            else { corrupt("unusable payload outcome kind"); }
            PriceOutcomeRecord record{{contract.scope.clock, number<std::uint64_t>(*element, "step")}, extent(*element), number<std::int64_t>(*element, "available_at_ns"),
                kind == "published" ? OutcomeKind::Published : kind == "warming" ? OutcomeKind::Warming : OutcomeKind::ValidAbsence,
                kind == "warming" ? number<std::uint64_t>(*element, "remaining_preceding") : 0, kind == "valid_absence" ? number<std::uint32_t>(*element, "declared_reason") : 0, {}, {}};
            const auto* child = element->FirstChildElement();
            if (record.kind == OutcomeKind::Published) {
                std::size_t port_count = 0;
                for (auto* port = child; port && std::string_view{port->Name()} == "port"; port = port->NextSiblingElement()) { ++port_count; }
                if (port_count != contract.outputs.size()) { corrupt("price payload output count differs from contract"); }
                record.values.reserve(contract.outputs.size());
                for (const auto& output : contract.outputs) {
                    if (!child || std::string_view{child->Name()} != "port") { corrupt("missing ordered price payload port"); }
                    attributes(*child, {"name", "type", "type_version", "scale", "raw"});
                    if (attribute(*child, "name") != output.name || attribute(*child, "type") != output.type.nominal_name || number<std::uint32_t>(*child, "type_version") != output.type.semantic_version || number<std::uint32_t>(*child, "scale") != 9) { corrupt("price payload semantic type or port mismatch"); }
                    record.values.push_back(quant::data::Price::from_raw(number<std::int64_t>(*child, "raw"))); child = child->NextSiblingElement();
                }
            }
            if (!child || std::string_view{child->Name()} != "parents" || child->NextSiblingElement()) { corrupt("missing, extra or reordered payload parents container"); }
            attributes(*child, {}); std::size_t parents = 0;
            for (auto* parent = child->FirstChildElement(); parent; parent = parent->NextSiblingElement()) { if (++parents > limits.parents_per_outcome) { corrupt("payload parent count exceeds bound"); } }
            record.parents.reserve(parents);
            for (const auto* parent = child->FirstChildElement(); parent; parent = parent->NextSiblingElement()) {
                attributes(*parent, {"input_port", "computation_digest", "clock", "step", "port"}, {"source_reference_digest"}, true);
                if (attribute(*parent, "clock") != contract.scope.clock) { corrupt("parent clock differs from activation"); }
                std::optional<Sha256> reference; if (const auto* value = parent->Attribute("source_reference_digest")) { reference = digest(value); }
                record.parents.push_back({number<PortIndex>(*parent, "input_port"), {digest(attribute(*parent, "computation_digest")),
                    {contract.scope.clock, number<std::uint64_t>(*parent, "step")}, number<PortIndex>(*parent, "port"), extent(*parent), number<std::int64_t>(*parent, "available_at_ns"), reference}});
            }
            record_valid(record, contract, limits, data->outcomes.size()); data->outcomes.push_back(std::move(record));
        }
        if (data->outcomes.size() != contract.complete_keys.size()) { corrupt("payload does not cover every complete-update key"); }
        return DecodedPricePayload{std::move(data)};
    });
}
Result<std::uint64_t> measure_price_payload(std::span<const PriceOutcomeRecord> records, const PricePayloadContract& contract,
    std::shared_ptr<ResourceAccount> account, const PricePayloadLimits& limits) noexcept {
    return protect([&]() -> Result<std::uint64_t> {
        if (!account || limits.scratch_bytes < 4096) { return diagnostic(FailureCode::ResourcePlanExceeded, "payload encoder requires explicit scratch/account"); }
        auto scratch = checked(account->reserve(ResourceKind::Scratch, 4096)); contract_valid(contract, limits);
        if (records.size() != contract.complete_keys.size()) { corrupt("encoded payload does not cover exact complete keys"); }
        std::uint64_t elements = 1;
        for (std::size_t i = 0; i < records.size(); ++i) {
            record_valid(records[i], contract, limits, i); elements = checked(checked_add(elements, checked(checked_add(2 + records[i].values.size(), records[i].parents.size()))));
        }
        if (elements > limits.xml_elements) { return diagnostic(FailureCode::ResourcePlanExceeded, "encoded payload exceeds element bound"); }
        Writer counter{std::numeric_limits<std::uint64_t>::max(), limits.string_bytes}; encode(counter, records, contract);
        return counter.size();
    });
}
Result<EncodedPricePayload> encode_price_payload(std::span<const PriceOutcomeRecord> records, const PricePayloadContract& contract,
    std::shared_ptr<ResourceAccount> account, const PricePayloadLimits& limits) noexcept {
    return protect([&]() -> Result<EncodedPricePayload> {
        const auto count = checked(measure_price_payload(records, contract, account, limits));
        if (count > limits.xml_bytes || count > std::string{}.max_size()) { return diagnostic(FailureCode::ResourcePlanExceeded, "encoded payload exceeds byte capacity"); }
        auto charge = checked(account->reserve(ResourceKind::Staging, checked(checked_add(count, sizeof(EncodedPricePayload) + 64))));
        std::string bytes(static_cast<std::size_t>(count), '\0'); Writer writer{count, limits.string_bytes, &bytes}; encode(writer, records, contract);
        static_cast<void>(preflight(bytes, limits));
        return EncodedPricePayload{std::move(charge), std::move(bytes)};
    });
}
} // namespace lexongine
