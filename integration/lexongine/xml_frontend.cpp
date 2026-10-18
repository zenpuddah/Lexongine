#include "xml_frontend.hpp"
#include <tinyxml2.h>

#include <filesystem>

static_assert(TIXML2_MAJOR_VERSION == 11 && TIXML2_MINOR_VERSION == 0 && TIXML2_PATCH_VERSION == 0,
              "Lexongine requires the accepted TinyXML2 11.0.0");

namespace lexongine {
class FrontendAccess {
public:
    static ParsedComposition parsed(std::shared_ptr<const CompositionDeclaration> value) { return ParsedComposition{std::move(value)}; }
    static Composition resolved(Reservation charge, const ParsedComposition& parsed, std::vector<ResolvedParameters> sources,
                                std::vector<ResolvedParameters> nodes) {
        return Composition{std::move(charge), parsed.declaration_, std::move(sources), std::move(nodes)};
    }
};
namespace {
struct ParseFailure { Diagnostic error; };
[[noreturn]] void fail(FailureCode code, std::string_view text, std::uint64_t line = 0) {
    auto error = diagnostic(code, text);
    if (line) {
        std::array<char, 32> position{};
        constexpr std::string_view prefix = " at XML line ";
        std::copy(prefix.begin(), prefix.end(), position.begin());
        auto converted = std::to_chars(position.data() + prefix.size(), position.data() + position.size(), line);
        const auto length = static_cast<std::size_t>(converted.ptr - position.data());
        const auto available = error.message.size() - error.length;
        const auto copied = std::min(length, available);
        std::copy_n(position.data(), copied, error.message.data() + error.length);
        error.length = static_cast<std::uint16_t>(error.length + copied);
        error.truncated |= copied != length;
    }
    throw ParseFailure{error};
}
template<class T> T checked(std::variant<T, Diagnostic> result) {
    if (const auto* error = std::get_if<Diagnostic>(&result)) { throw ParseFailure{*error}; }
    return std::get<T>(std::move(result));
}
void checked(Result<void> result) { if (const auto* error = std::get_if<Diagnostic>(&result)) { throw ParseFailure{*error}; } }
bool whitespace(char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool name_character(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
}
bool xml_character(std::uint32_t value) noexcept {
    return value == 9 || value == 10 || value == 13 || (value >= 0x20 && value <= 0xd7ff) ||
           (value >= 0xe000 && value <= 0xfffd) || (value >= 0x10000 && value <= 0x10ffff);
}
bool child_allowed(std::string_view parent, std::string_view child) noexcept {
    if (parent.empty()) { return child == "composition"; }
    if (parent == "composition") { return child == "run" || child == "limits" || child == "plugins" || child == "cache" || child == "sources" || child == "nodes" || child == "observations"; }
    if (parent == "plugins") { return child == "plugin"; }
    if (parent == "sources") { return child == "source"; }
    if (parent == "nodes") { return child == "node"; }
    if (parent == "observations") { return child == "observe"; }
    if (parent == "source") { return child == "param"; }
    if (parent == "node") { return child == "param" || child == "input"; }
    return false;
}
struct LexicalCounts { std::uint64_t elements = 0, depth = 0; };

// Narrow authoring vocabulary preflight. No DTD, namespace, external expansion,
// generic XML grammar or plugin-defined element language is introduced.
LexicalCounts preflight(std::string_view xml, const ParseLimits& limits) {
    if (xml.empty() || xml.size() > limits.xml_bytes) { fail(FailureCode::InvalidConfig, "XML byte limit exceeded or empty input"); }
    checked(validate_text(xml, limits.xml_bytes, false));
    for (std::size_t i = 0; i < xml.size(); ++i) {
        if ((static_cast<unsigned char>(xml[i]) < 0x20 && !whitespace(xml[i])) ||
            xml.substr(i, 3) == "\xef\xbf\xbe" || xml.substr(i, 3) == "\xef\xbf\xbf") {
            fail(FailureCode::InvalidConfig, "character is not legal in XML 1.0");
        }
    }
    std::array<std::string_view, 4> stack{}; // Maximum depth of the exact authoring grammar.
    std::size_t depth = 0, offset = 0;
    std::uint64_t line = 1;
    bool root_seen = false, declaration_seen = false;
    LexicalCounts counts;
    if (xml.starts_with("\xef\xbb\xbf")) { offset = 3; }
    const auto initial = offset;
    auto advance = [&] { if (xml[offset] == '\n') { ++line; } ++offset; };
    auto space = [&] { while (offset < xml.size() && whitespace(xml[offset])) { advance(); } };
    auto name = [&]() -> std::string_view {
        const auto begin = offset;
        while (offset < xml.size() && name_character(xml[offset])) { advance(); }
        if (begin == offset || (offset < xml.size() && xml[offset] == ':')) { fail(FailureCode::InvalidConfig, "invalid or namespaced XML name", line); }
        return xml.substr(begin, offset - begin);
    };
    auto quoted = [&]() {
        if (offset == xml.size() || (xml[offset] != '\'' && xml[offset] != '"')) { fail(FailureCode::InvalidConfig, "quoted attribute value required", line); }
        const auto quote = xml[offset]; advance();
        std::uint64_t decoded_bytes = 0;
        while (offset < xml.size() && xml[offset] != quote) {
            const auto character = static_cast<unsigned char>(xml[offset]);
            if (character == '<' || (character < 0x20 && !whitespace(xml[offset]))) { fail(FailureCode::InvalidConfig, "invalid XML attribute character", line); }
            if (xml[offset] == '&') {
                const auto end = xml.find(';', offset + 1);
                if (end == std::string_view::npos) { fail(FailureCode::InvalidConfig, "unterminated XML entity", line); }
                const auto entity = xml.substr(offset + 1, end - offset - 1);
                std::uint64_t length = 1;
                if (entity.starts_with('#')) {
                    auto digits = entity.substr(1);
                    int base = 10;
                    if (digits.starts_with('x')) { base = 16; digits.remove_prefix(1); }
                    std::uint32_t value = 0;
                    const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
                    if (digits.empty() || result.ec != std::errc{} || result.ptr != digits.data() + digits.size() || !xml_character(value)) {
                        fail(FailureCode::InvalidConfig, "invalid XML character reference", line);
                    }
                    length = value < 0x80 ? 1 : value < 0x800 ? 2 : value < 0x10000 ? 3 : 4;
                } else if (entity != "amp" && entity != "lt" && entity != "gt" && entity != "apos" && entity != "quot") {
                    fail(FailureCode::InvalidConfig, "undeclared XML entity", line);
                }
                decoded_bytes = checked(checked_add(decoded_bytes, length));
                while (offset <= end) { advance(); }
            } else { ++decoded_bytes; advance(); }
            if (decoded_bytes > limits.string_bytes) { fail(FailureCode::InvalidConfig, "XML attribute text limit exceeded", line); }
        }
        if (offset == xml.size()) { fail(FailureCode::InvalidConfig, "unterminated attribute", line); }
        advance();
    };
    while (offset < xml.size()) {
        if (whitespace(xml[offset])) { advance(); continue; }
        if (xml.substr(offset).starts_with("<!--")) {
            const auto end = xml.find("-->", offset + 4);
            if (end == std::string_view::npos || xml.substr(offset + 4, end - offset - 4).find("--") != std::string_view::npos ||
                end - offset - 4 > limits.string_bytes) { fail(FailureCode::InvalidConfig, "invalid or over-limit XML comment", line); }
            while (offset < end + 3) { advance(); }
            continue;
        }
        if (xml.substr(offset).starts_with("<?")) {
            if (offset != initial || declaration_seen || !xml.substr(offset).starts_with("<?xml") ||
                offset + 5 == xml.size() || !whitespace(xml[offset + 5])) {
                fail(FailureCode::InvalidConfig, "unsupported XML processing instruction", line);
            }
            declaration_seen = true;
            offset += 5;
            bool version = false, encoding = false, standalone = false, terminated = false;
            while (offset < xml.size()) {
                const auto before_space = offset; space();
                if (xml.substr(offset).starts_with("?>")) { advance(); advance(); terminated = true; break; }
                if (offset == before_space) { fail(FailureCode::InvalidConfig, "declaration attribute separator required", line); }
                const auto attribute = name(); space();
                if (offset == xml.size() || xml[offset] != '=') { fail(FailureCode::InvalidConfig, "declaration equals sign required", line); }
                advance(); space();
                const auto begin = offset;
                quoted();
                const auto value = xml.substr(begin + 1, offset - begin - 2);
                if (attribute == "version") {
                    if (version || encoding || standalone) { fail(FailureCode::InvalidConfig, "duplicate or reordered XML version", line); }
                    if (value != "1.0") { fail(FailureCode::UnsupportedContract, "only XML 1.0 is supported", line); }
                    version = true;
                } else if (attribute == "encoding") {
                    if (!version || encoding || standalone) { fail(FailureCode::InvalidConfig, "duplicate or reordered XML encoding", line); }
                    if (value.size() != 5 || (value[0] != 'U' && value[0] != 'u') || (value[1] != 'T' && value[1] != 't') ||
                        (value[2] != 'F' && value[2] != 'f') || value.substr(3) != "-8") {
                        fail(FailureCode::UnsupportedContract, "only UTF-8 XML is supported", line);
                    }
                    encoding = true;
                } else if (attribute == "standalone") {
                    if (!version || standalone || (value != "yes" && value != "no")) { fail(FailureCode::InvalidConfig, "invalid XML standalone declaration", line); }
                    standalone = true;
                } else { fail(FailureCode::InvalidConfig, "unknown XML declaration attribute", line); }
            }
            if (!version || !terminated) { fail(FailureCode::InvalidConfig, "incomplete XML declaration", line); }
            continue;
        }
        if (xml[offset] != '<' || xml.substr(offset).starts_with("<!")) { fail(FailureCode::InvalidConfig, "DTD, declarations or text content are unsupported", line); }
        advance();
        if (offset < xml.size() && xml[offset] == '/') {
            advance(); const auto close = name(); space();
            if (depth == 0 || stack[depth - 1] != close || offset == xml.size() || xml[offset] != '>') { fail(FailureCode::InvalidConfig, "mismatched closing element", line); }
            --depth; advance(); continue;
        }
        const auto tag = name();
        if (!child_allowed(depth ? stack[depth - 1] : std::string_view{}, tag) || (depth == 0 && root_seen)) {
            fail(FailureCode::InvalidConfig, "unknown or misplaced XML element", line);
        }
        if (depth == 0) { root_seen = true; }
        if (depth == stack.size() || depth + 1 > limits.xml_depth || ++counts.elements > limits.xml_elements) {
            fail(FailureCode::InvalidConfig, "XML depth or element limit exceeded", line);
        }
        counts.depth = std::max(counts.depth, static_cast<std::uint64_t>(depth + 1));
        std::array<std::string_view, 16> attributes{};
        std::size_t attribute_count = 0;
        bool closed = false;
        while (offset < xml.size()) {
            const auto before_space = offset; space();
            if (xml.substr(offset).starts_with("/>")) { advance(); advance(); closed = true; break; }
            if (offset < xml.size() && xml[offset] == '>') { advance(); break; }
            if (offset == before_space || attribute_count == attributes.size()) { fail(FailureCode::InvalidConfig, "attribute separator/count invalid", line); }
            const auto attribute = name();
            if (attribute == "xmlns" || std::find(attributes.begin(), attributes.begin() + static_cast<std::ptrdiff_t>(attribute_count), attribute) != attributes.begin() + static_cast<std::ptrdiff_t>(attribute_count)) {
                fail(FailureCode::InvalidConfig, "namespace or duplicate attribute", line);
            }
            attributes[attribute_count++] = attribute;
            space();
            if (offset == xml.size() || xml[offset] != '=') { fail(FailureCode::InvalidConfig, "attribute equals sign required", line); }
            advance(); space(); quoted();
        }
        if (!closed) { stack[depth++] = tag; }
    }
    if (!root_seen || depth != 0) { fail(FailureCode::InvalidConfig, "incomplete XML composition", line); }
    return counts;
}

using Element = tinyxml2::XMLElement;
void attributes(const Element& element, std::initializer_list<std::string_view> expected) {
    std::size_t count = 0;
    for (auto* attribute = element.FirstAttribute(); attribute; attribute = attribute->Next()) {
        if (std::find(expected.begin(), expected.end(), attribute->Name()) == expected.end()) { fail(FailureCode::InvalidConfig, "unknown attribute", element.GetLineNum()); }
        ++count;
    }
    if (count != expected.size()) { fail(FailureCode::InvalidConfig, "missing required attribute", element.GetLineNum()); }
}
std::string text(const Element& element, const char* name, std::uint64_t maximum, bool empty = false) {
    const char* value = element.Attribute(name);
    if (!value) { fail(FailureCode::InvalidConfig, "missing attribute", element.GetLineNum()); }
    checked(validate_text(value, maximum, empty));
    return value;
}
template<class T> T number(const Element& element, const char* name) {
    const char* value = element.Attribute(name);
    if (!value) { fail(FailureCode::InvalidConfig, "missing integer attribute", element.GetLineNum()); }
    auto result = parse_integer<T>(value);
    if (const auto* error = std::get_if<Diagnostic>(&result)) { fail(error->code, error->text(), element.GetLineNum()); }
    return std::get<T>(result);
}
void equals(std::string_view actual, std::string_view expected, const Element& element) {
    if (actual != expected) { fail(FailureCode::UnsupportedContract, "unsupported explicit policy", element.GetLineNum()); }
}
void absolute(const std::string& value, const Element& element) {
    if (!std::filesystem::path{value}.is_absolute()) { fail(FailureCode::InvalidConfig, "absolute path required", element.GetLineNum()); }
}
MissingIntervalPolicy missing_policy(const Element& element, std::uint64_t maximum) {
    const auto value = text(element, "missing_intervals", maximum);
    if (value == "strict") { return MissingIntervalPolicy::Strict; }
    if (value == "skip") { return MissingIntervalPolicy::Skip; }
    fail(FailureCode::UnsupportedContract, "unsupported missing-interval policy", element.GetLineNum());
}
OperationKey operation_key(const Element& element, std::uint64_t maximum) {
    auto qualified = text(element, "operation", maximum);
    const auto separator = qualified.rfind('.');
    if (separator == std::string::npos || separator == 0 || separator + 1 == qualified.size()) { fail(FailureCode::InvalidConfig, "qualified operation name required", element.GetLineNum()); }
    const auto version = number<std::uint32_t>(element, "semantic_version");
    if (!version) { fail(FailureCode::InvalidConfig, "operation version must be positive", element.GetLineNum()); }
    return {qualified.substr(0, separator), qualified.substr(separator + 1), version, text(element, "implementation_id", maximum)};
}
ParamSet parameters(const Element& element, std::uint64_t maximum) {
    ParamSet result;
    bool inputs_seen = false;
    for (const auto* child = element.FirstChildElement(); child; child = child->NextSiblingElement()) {
        if (std::string_view{child->Name()} == "input") { inputs_seen = true; continue; }
        if (std::string_view{child->Name()} != "param" || inputs_seen) { fail(FailureCode::InvalidConfig, "parameters must precede inputs", child->GetLineNum()); }
        attributes(*child, {"name", "value"});
        auto name = text(*child, "name", maximum);
        if (std::any_of(result.values.begin(), result.values.end(), [&](const auto& value) { return value.name == name; })) { fail(FailureCode::InvalidConfig, "duplicate parameter", child->GetLineNum()); }
        result.values.push_back({std::move(name), text(*child, "value", maximum, true), {"<memory>", static_cast<std::uint64_t>(child->GetLineNum())}});
    }
    return result;
}
}

Result<ParsedComposition> parse_xml(std::string_view xml, const ParseLimits& limits) noexcept {
    return guarded([&]() -> Result<ParsedComposition> {
        try {
            if (!limits.account) { fail(FailureCode::InvalidConfig, "explicit XML preparation account required"); }
            static_cast<void>(preflight(xml, limits));
            const auto payload = checked(checked_multiply(xml.size(), 64));
            const auto budget = checked(checked_add(payload, 65536));
            auto charge = checked(limits.account->reserve(ResourceKind::Graph, budget));
            tinyxml2::XMLDocument document;
            if (document.Parse(xml.data(), xml.size()) != tinyxml2::XML_SUCCESS) {
                fail(FailureCode::InvalidConfig, document.ErrorStr() ? document.ErrorStr() : "invalid XML", document.ErrorLineNum());
            }
            const auto* root = document.RootElement();
            if (!root || std::string_view{root->Name()} != "composition") { fail(FailureCode::InvalidConfig, "composition root required"); }
            attributes(*root, {"version"});
            if (number<std::uint32_t>(*root, "version") != 1) { fail(FailureCode::UnsupportedContract, "unsupported composition version", root->GetLineNum()); }
            constexpr std::array names{"run", "limits", "plugins", "cache", "sources", "nodes", "observations"};
            std::array<const Element*, names.size()> sections{};
            const auto* element = root->FirstChildElement();
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (!element || std::string_view{element->Name()} != names[i]) { fail(FailureCode::InvalidConfig, "missing, duplicate or reordered root section", root->GetLineNum()); }
                sections[i] = element; element = element->NextSiblingElement();
            }
            if (element) { fail(FailureCode::InvalidConfig, "unexpected root section", element->GetLineNum()); }
            auto data = std::make_shared<CompositionDeclaration>();
            data->charge = std::move(charge); data->account = limits.account; data->metadata_bytes = budget;
            const auto& bound = *sections[1];
            attributes(bound, {"xml_bytes", "xml_depth", "xml_elements", "string_bytes", "nodes", "edges", "requests", "source_updates", "managed_buffer_bytes", "trace_events", "trace_message_bytes"});
            data->limits = {number<std::uint64_t>(bound, "xml_bytes"), number<std::uint64_t>(bound, "xml_depth"), number<std::uint64_t>(bound, "xml_elements"),
                number<std::uint64_t>(bound, "string_bytes"), number<std::uint64_t>(bound, "nodes"), number<std::uint64_t>(bound, "edges"),
                number<std::uint64_t>(bound, "requests"), number<std::uint64_t>(bound, "source_updates"), number<std::uint64_t>(bound, "managed_buffer_bytes"),
                number<std::uint64_t>(bound, "trace_events"), number<std::uint64_t>(bound, "trace_message_bytes")};
            const auto& declared = data->limits;
            if (declared.xml_bytes > limits.xml_bytes || declared.xml_depth > limits.xml_depth || declared.xml_elements > limits.xml_elements ||
                declared.string_bytes > limits.string_bytes || declared.nodes > std::numeric_limits<DeclaredOrdinal>::max()) {
                fail(FailureCode::InvalidConfig, "document limits may only tighten supported bootstrap bounds", bound.GetLineNum());
            }
            static_cast<void>(preflight(xml, {declared.xml_bytes, declared.xml_depth, declared.xml_elements, declared.string_bytes, limits.account}));
            const auto maximum = declared.string_bytes;
            const auto& run = *sections[0];
            attributes(run, {"id", "clock", "driver", "execution", "sharing", "failure", "trace", "retention_warning_steps", "retention_log_path", "retention_log_format",
                             "retention_log_open", "retention_log_flush", "retention_log_failure", "retention_log_overflow", "retention_log_max_bytes", "retention_log_max_events"});
            const auto driver = text(run, "driver", maximum);
            if (driver != "standalone" && driver != "hosted") { fail(FailureCode::UnsupportedContract, "unsupported driver", run.GetLineNum()); }
            data->run = {text(run, "id", maximum), text(run, "clock", maximum), driver == "standalone" ? Driver::Standalone : Driver::Hosted,
                text(run, "execution", maximum), text(run, "sharing", maximum), text(run, "failure", maximum), text(run, "trace", maximum),
                number<std::uint64_t>(run, "retention_warning_steps"), {text(run, "retention_log_path", maximum), text(run, "retention_log_format", maximum),
                text(run, "retention_log_open", maximum), text(run, "retention_log_flush", maximum), text(run, "retention_log_failure", maximum),
                text(run, "retention_log_overflow", maximum), number<std::uint64_t>(run, "retention_log_max_bytes"), number<std::uint64_t>(run, "retention_log_max_events")}};
            equals(data->run.execution, "synchronous", run); equals(data->run.failure, "stop", run);
            if ((data->run.sharing != "exact" && data->run.sharing != "disabled") || (data->run.trace != "steps" && data->run.trace != "summary")) { fail(FailureCode::UnsupportedContract, "unsupported sharing/trace policy", run.GetLineNum()); }
            const auto& log = data->run.retention_log;
            absolute(log.path, run); equals(log.format, "key_value_lines_v1", run); equals(log.open, "create_new", run);
            equals(log.flush, "on_event", run); equals(log.failure, "stop", run); equals(log.overflow, "stop", run);
            attributes(*sections[2], {});
            for (auto* plugin = sections[2]->FirstChildElement(); plugin; plugin = plugin->NextSiblingElement()) {
                attributes(*plugin, {"path", "build_contract_id"});
                auto path = text(*plugin, "path", maximum); absolute(path, *plugin);
                data->plugins.push_back({std::move(path), text(*plugin, "build_contract_id", maximum)});
            }
            const auto& cache = *sections[3];
            const auto cache_mode = text(cache, "mode", maximum);
            if (cache_mode == "disabled") { attributes(cache, {"mode"}); data->cache = DisabledCache{}; }
            else {
                attributes(cache, {"mode", "backend", "directory", "matching", "trust", "integrity", "writer", "corruption", "capacity", "cleanup", "max_entries", "max_store_bytes", "max_entry_bytes", "max_decoded_bytes"});
                LocalCacheConfiguration configuration{cache_mode, text(cache, "backend", maximum), text(cache, "directory", maximum), text(cache, "matching", maximum),
                    text(cache, "trust", maximum), text(cache, "integrity", maximum), text(cache, "writer", maximum), text(cache, "corruption", maximum),
                    text(cache, "capacity", maximum), text(cache, "cleanup", maximum), number<std::uint64_t>(cache, "max_entries"), number<std::uint64_t>(cache, "max_store_bytes"),
                    number<std::uint64_t>(cache, "max_entry_bytes"), number<std::uint64_t>(cache, "max_decoded_bytes")};
                equals(configuration.mode, "automatic_read_write", cache); equals(configuration.backend, "local_files", cache);
                absolute(configuration.directory, cache); equals(configuration.matching, "exact_full_scope", cache);
                equals(configuration.trust, "same_build_local_writer", cache); equals(configuration.integrity, "sha256", cache);
                equals(configuration.writer, "single_process_lock", cache); equals(configuration.corruption, "fail_preparation", cache);
                equals(configuration.capacity, "bypass_new_entries", cache); equals(configuration.cleanup, "explicit_only", cache);
                data->cache = std::move(configuration);
            }
            DeclaredOrdinal ordinal = 0;
            attributes(*sections[4], {});
            for (auto* source = sections[4]->FirstChildElement(); source; source = source->NextSiblingElement()) {
                attributes(*source, {"id", "operation", "semantic_version", "implementation_id", "source_version", "clock", "begin_step", "end_step", "completeness", "empty", "missing", "missing_intervals", "availability"});
                if (ordinal >= declared.nodes) { fail(FailureCode::InvalidConfig, "logical node limit exceeded", source->GetLineNum()); }
                SourceDeclaration value{text(*source, "id", maximum), ordinal++, operation_key(*source, maximum), text(*source, "source_version", maximum),
                    {text(*source, "clock", maximum), number<std::uint64_t>(*source, "begin_step"), number<std::uint64_t>(*source, "end_step")},
                    text(*source, "completeness", maximum), text(*source, "empty", maximum), text(*source, "missing", maximum), text(*source, "availability", maximum),
                    missing_policy(*source, maximum), parameters(*source, maximum), {"<memory>", static_cast<std::uint64_t>(source->GetLineNum())}};
                if (value.scope.end < value.scope.begin || value.scope.end - value.scope.begin > declared.source_updates || value.scope.clock != data->run.clock) {
                    fail(FailureCode::InvalidConfig, "source clock/scope exceeds declared run contract", source->GetLineNum());
                }
                data->sources.push_back(std::move(value));
            }
            std::uint64_t edges = 0;
            attributes(*sections[5], {});
            for (auto* node = sections[5]->FirstChildElement(); node; node = node->NextSiblingElement()) {
                attributes(*node, {"id", "operation", "semantic_version", "implementation_id", "alignment", "initialization"});
                if (ordinal >= declared.nodes) { fail(FailureCode::InvalidConfig, "logical node limit exceeded", node->GetLineNum()); }
                NodeDeclaration value{text(*node, "id", maximum), ordinal++, operation_key(*node, maximum), text(*node, "alignment", maximum), text(*node, "initialization", maximum),
                    parameters(*node, maximum), {}, {"<memory>", static_cast<std::uint64_t>(node->GetLineNum())}};
                if (value.alignment != "same_step" && value.alignment != "same_step_activation") { fail(FailureCode::UnsupportedContract, "unsupported node alignment", node->GetLineNum()); }
                for (auto* input = node->FirstChildElement("input"); input; input = input->NextSiblingElement("input")) {
                    attributes(*input, {"port", "producer", "output", "prior_outputs", "missing_intervals"});
                    if (++edges > declared.edges) { fail(FailureCode::InvalidConfig, "edge limit exceeded", input->GetLineNum()); }
                    value.inputs.push_back({text(*input, "port", maximum), text(*input, "producer", maximum), text(*input, "output", maximum),
                        number<std::uint64_t>(*input, "prior_outputs"), Alignment::SameStep, missing_policy(*input, maximum)});
                    for (std::size_t i = 0; i + 1 < value.inputs.size(); ++i) {
                        if (value.inputs[i].input_name == value.inputs.back().input_name) { fail(FailureCode::InvalidConfig, "duplicate input port", input->GetLineNum()); }
                    }
                }
                data->nodes.push_back(std::move(value));
            }
            auto producer_exists = [&](std::string_view id) {
                return std::any_of(data->sources.begin(), data->sources.end(), [&](const auto& item) { return item.id == id; }) ||
                       std::any_of(data->nodes.begin(), data->nodes.end(), [&](const auto& item) { return item.id == id; });
            };
            for (const auto& source : data->sources) {
                const auto count = std::count_if(data->sources.begin(), data->sources.end(), [&](const auto& item) { return item.id == source.id; }) +
                                   std::count_if(data->nodes.begin(), data->nodes.end(), [&](const auto& item) { return item.id == source.id; });
                if (count != 1) { fail(FailureCode::InvalidConfig, "duplicate producer ID", source.location.line); }
            }
            for (const auto& node : data->nodes) {
                if (std::count_if(data->nodes.begin(), data->nodes.end(), [&](const auto& item) { return item.id == node.id; }) != 1) { fail(FailureCode::InvalidConfig, "duplicate producer ID", node.location.line); }
                for (const auto& input : node.inputs) { if (!producer_exists(input.producer)) { fail(FailureCode::InvalidConfig, "undeclared input producer", node.location.line); } }
            }
            attributes(*sections[6], {});
            for (auto* observe = sections[6]->FirstChildElement(); observe; observe = observe->NextSiblingElement()) {
                attributes(*observe, {"id", "producer", "output", "begin_step", "end_step", "absence", "prior_outputs", "missing_intervals"});
                equals(text(*observe, "absence", maximum), "require_value", *observe);
                if (data->observations.size() >= declared.requests) { fail(FailureCode::InvalidConfig, "request limit exceeded", observe->GetLineNum()); }
                ObservationRequest value{missing_policy(*observe, maximum), text(*observe, "id", maximum), text(*observe, "producer", maximum), text(*observe, "output", maximum),
                    {data->run.clock, number<std::uint64_t>(*observe, "begin_step"), number<std::uint64_t>(*observe, "end_step")}, AbsencePolicy::RequireValue,
                    number<std::uint64_t>(*observe, "prior_outputs"), text(*observe, "id", maximum)};
                if (value.evaluation.end < value.evaluation.begin || !producer_exists(value.producer) ||
                    std::any_of(data->observations.begin(), data->observations.end(), [&](const auto& item) { return item.id == value.id; })) { fail(FailureCode::InvalidConfig, "invalid/duplicate observation or producer", observe->GetLineNum()); }
                data->observations.push_back(std::move(value));
            }
            return FrontendAccess::parsed(std::move(data));
        } catch (const ParseFailure& failure) { return failure.error; }
    });
}

Result<ParsedComposition> apply_overrides(const ParsedComposition& parsed, const CompositionOverrides& overrides) noexcept {
    return guarded([&]() -> Result<ParsedComposition> {
        try {
            const auto& original=parsed.declaration();const auto maximum=original.limits.string_bytes;
            if(overrides.parameters.size()>original.limits.xml_elements||overrides.observations.size()>original.limits.requests){fail(FailureCode::InvalidConfig,"override count exceeds declared bounds");}
            auto bytes=checked(checked_add(original.metadata_bytes,4096));
            auto count_text=[&](const std::string& text,bool empty=false){checked(validate_text(text,maximum,empty));bytes=checked(checked_add(bytes,checked(checked_add(256,checked(checked_multiply(text.size(),4))))));};
            for(const auto& p:overrides.parameters){count_text(p.producer);count_text(p.parameter);count_text(p.value,true);}
            for(const auto& id:overrides.observations){count_text(id);}
            for(const auto* option:{&overrides.retention_log,&overrides.sharing,&overrides.reuse}){if(*option){count_text(**option);}}
            auto charge=checked(original.account->reserve(ResourceKind::Graph,bytes));
            auto data=std::make_shared<CompositionDeclaration>();data->charge=std::move(charge);data->account=original.account;data->metadata_bytes=bytes;
            data->run=original.run;data->limits=original.limits;data->plugins=original.plugins;data->cache=original.cache;
            data->sources=original.sources;data->nodes=original.nodes;data->observations=original.observations;data->overrides=original.overrides;
            auto record=[&](std::string target,std::string value){
                checked(validate_text(target,maximum,false));
                if(std::any_of(data->overrides.begin(),data->overrides.end(),[&](const auto& p){return p.target==target;})){fail(FailureCode::InvalidConfig,"duplicate configuration override target");}
                data->overrides.push_back({std::move(target),std::move(value)});
            };
            for(const auto& p:overrides.parameters){
                ParamSet* parameters=nullptr;
                for(auto& source:data->sources){if(source.id==p.producer){parameters=&source.parameters;}}
                for(auto& node:data->nodes){if(node.id==p.producer){parameters=&node.parameters;}}
                if(!parameters){fail(FailureCode::InvalidConfig,"override names an undeclared producer");}
                auto literal=std::find_if(parameters->values.begin(),parameters->values.end(),[&](const auto& value){return value.name==p.parameter;});
                if(literal==parameters->values.end()){fail(FailureCode::InvalidConfig,"override names an undeclared parameter");}
                record("parameter:"+p.producer+"."+p.parameter,p.value);
                literal->text=p.value;literal->source_location={"<cli>",p.argument};
            }
            if(!overrides.observations.empty()){
                for(const auto& id:overrides.observations){
                    if(std::none_of(data->observations.begin(),data->observations.end(),[&](const auto& o){return o.id==id;})){fail(FailureCode::InvalidConfig,"selected observation is undeclared");}
                    record("observe:"+id,id);
                }
                std::erase_if(data->observations,[&](const auto& o){return std::find(overrides.observations.begin(),overrides.observations.end(),o.id)==overrides.observations.end();});
            }
            if(overrides.retention_log){
                if(!std::filesystem::path{*overrides.retention_log}.is_absolute()){fail(FailureCode::InvalidConfig,"override evidence path must be absolute");}
                record("retention_log",*overrides.retention_log);data->run.retention_log.path=*overrides.retention_log;
            }
            if(overrides.sharing){
                if(*overrides.sharing!="exact"&&*overrides.sharing!="disabled"){fail(FailureCode::InvalidConfig,"unsupported sharing override");}
                record("sharing",*overrides.sharing);data->run.sharing=*overrides.sharing;
            }
            if(overrides.reuse){
                if(*overrides.reuse=="disabled"){data->cache=DisabledCache{};}
                else if(*overrides.reuse!="automatic_read_write"||!std::holds_alternative<LocalCacheConfiguration>(data->cache)){fail(FailureCode::InvalidConfig,"reuse override requires complete explicit store policies");}
                record("reuse",*overrides.reuse);
            }
            return FrontendAccess::parsed(std::move(data));
        }catch(const ParseFailure& failure){return failure.error;}
    });
}

Result<Composition> resolve_composition(const ParsedComposition& parsed, const Registry& registry) noexcept {
    return guarded([&]() -> Result<Composition> {
        try {
            if (!registry.frozen()) { fail(FailureCode::InvalidConfig, "freeze the complete plugin registry before schema resolution"); }
            const auto& declaration = parsed.declaration();
            auto charge = checked(declaration.account->reserve(ResourceKind::Graph, declaration.metadata_bytes));
            std::vector<ResolvedParameters> sources, nodes;
            sources.reserve(declaration.sources.size()); nodes.reserve(declaration.nodes.size());
            for (const auto& source : declaration.sources) {
                auto definition = checked(registry.resolve(source.operation));
                const auto& supported = definition->contract().missing_protocols;
                if (std::find(supported.begin(), supported.end(), source.missing_intervals) == supported.end()) {
                    fail(FailureCode::UnsupportedContract, "source definition does not accept the declared missing policy", source.location.line);
                }
                sources.push_back({definition, checked(definition->prepare_parameters(source.parameters, declaration.limits.string_bytes))});
            }
            for (const auto& node : declaration.nodes) {
                auto definition = checked(registry.resolve(node.operation));
                const auto& contract = definition->contract();
                if (node.initialization != contract.sharing.initialization.id || node.inputs.size() != contract.inputs.size()) {
                    fail(FailureCode::UnsupportedContract, "initialization/input arity disagrees with registered operation", node.location.line);
                }
                if (node.alignment == "same_step_activation" && std::find(contract.timing_protocols.begin(), contract.timing_protocols.end(), node.alignment) == contract.timing_protocols.end()) {
                    fail(FailureCode::UnsupportedContract, "operation does not declare lag activation protocol", node.location.line);
                }
                if (std::find(contract.alignment_protocols.begin(), contract.alignment_protocols.end(), Alignment::SameStep) == contract.alignment_protocols.end()) {
                    fail(FailureCode::UnsupportedContract, "operation does not accept same-step input alignment", node.location.line);
                }
                for (std::size_t i = 0; i < node.inputs.size(); ++i) {
                    if (node.inputs[i].input_name != contract.inputs[i].name) { fail(FailureCode::InvalidConfig, "ordered input name mismatch", node.location.line); }
                    if (node.inputs[i].missing_intervals != node.inputs.front().missing_intervals ||
                        std::find(contract.missing_protocols.begin(), contract.missing_protocols.end(), node.inputs[i].missing_intervals) == contract.missing_protocols.end()) {
                        fail(FailureCode::UnsupportedContract, "operation input missing policies are incompatible", node.location.line);
                    }
                }
                nodes.push_back({definition, checked(definition->prepare_parameters(node.parameters, declaration.limits.string_bytes))});
            }
            auto producer = [&](std::string_view id) -> const OperationContract& {
                for (std::size_t i = 0; i < declaration.sources.size(); ++i) { if (declaration.sources[i].id == id) { return sources[i].definition->contract(); } }
                for (std::size_t i = 0; i < declaration.nodes.size(); ++i) { if (declaration.nodes[i].id == id) { return nodes[i].definition->contract(); } }
                fail(FailureCode::InvalidConfig, "unknown producer during schema resolution");
            };
            auto output = [&](std::string_view id, std::string_view port) -> const OutputContract& {
                const auto& ports = producer(id).outputs;
                const auto found = std::find_if(ports.begin(), ports.end(), [&](const auto& item) { return item.name == port; });
                if (found == ports.end()) { fail(FailureCode::InvalidConfig, "unknown named output"); }
                return *found;
            };
            auto producer_policy = [&](std::string_view id) -> std::optional<MissingIntervalPolicy> {
                for (const auto& source : declaration.sources) { if (source.id == id) { return source.missing_intervals; } }
                for (const auto& node : declaration.nodes) {
                    if (node.id == id && !node.inputs.empty()) { return node.inputs.front().missing_intervals; }
                }
                return std::nullopt; // No source-clock policy is invented for an unbound root.
            };
            for (std::size_t index = 0; index < declaration.nodes.size(); ++index) {
                for (std::size_t port = 0; port < declaration.nodes[index].inputs.size(); ++port) {
                    const auto& input = declaration.nodes[index].inputs[port];
                    if (const auto policy = producer_policy(input.producer); policy && *policy != input.missing_intervals) {
                        fail(FailureCode::UnsupportedContract, "producer/input missing policy mismatch", declaration.nodes[index].location.line);
                    }
                    checked(validate_connection(nodes[index].definition->contract().inputs[port], output(input.producer, input.output_name).type,
                                                declaration.run.clock, declaration.run.clock, input.missing_intervals));
                }
            }
            for (const auto& observation : declaration.observations) {
                static_cast<void>(output(observation.producer, observation.output_name));
                const auto& supported = producer(observation.producer).missing_protocols;
                if (std::find(supported.begin(), supported.end(), observation.missing_intervals) == supported.end() ||
                    (producer_policy(observation.producer) && *producer_policy(observation.producer) != observation.missing_intervals)) {
                    fail(FailureCode::UnsupportedContract, "observation missing policy mismatch");
                }
            }
            return FrontendAccess::resolved(std::move(charge), parsed, std::move(sources), std::move(nodes));
        } catch (const ParseFailure& failure) { return failure.error; }
    });
}

} // namespace lexongine
