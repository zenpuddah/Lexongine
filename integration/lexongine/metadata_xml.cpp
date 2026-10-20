#include "metadata_xml.hpp"

namespace lexongine::wire {
[[noreturn]] void fail(std::string_view message) {
    throw Error{diagnostic(FailureCode::CacheCorrupt, message)};
}
namespace {
bool space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}
bool name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-' || c == '.';
}
bool xml_char(std::uint32_t c) {
    return c == 9 || c == 10 || c == 13 || (c >= 32 && c <= 0xd7ff) ||
           (c >= 0xe000 && c <= 0xfffd) || (c >= 0x10000 && c <= 0x10ffff);
}
void xml_text(std::string_view value) {
    if (std::holds_alternative<Diagnostic>(validate_text(value, value.size(), true))) {
        fail("invalid metadata UTF-8");
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        if ((static_cast<unsigned char>(value[i]) < 32 && !space(value[i])) ||
            value.substr(i, 3) == "\xef\xbf\xbe" || value.substr(i, 3) == "\xef\xbf\xbf") {
            fail("invalid XML character");
        }
    }
}
void entities(std::string_view value) {
    for (std::size_t p = 0; (p = value.find('&', p)) != std::string_view::npos;) {
        const auto end = value.find(';', p + 1);
        if (end == std::string_view::npos) {
            fail("unterminated metadata entity");
        }
        auto entity = value.substr(p + 1, end - p - 1);
        if (entity.starts_with('#')) {
            entity.remove_prefix(1);
            int base = 10;
            if (entity.starts_with('x')) {
                base = 16;
                entity.remove_prefix(1);
            }
            std::uint32_t scalar = 0;
            auto parsed =
                std::from_chars(entity.data(), entity.data() + entity.size(), scalar, base);
            if (entity.empty() || parsed.ec != std::errc{} ||
                parsed.ptr != entity.data() + entity.size() || !xml_char(scalar)) {
                fail("invalid metadata character reference");
            }
        } else if (entity != "amp" && entity != "lt" && entity != "gt" && entity != "quot" &&
                   entity != "apos") {
            fail("unknown metadata entity");
        }
        p = end + 1;
    }
}
struct Counts {
    std::uint64_t nodes = 0, attrs = 0, elements = 0;
};
Counts preflight(std::string_view xml, const Limits &limits) {
    if (xml.empty() || xml.size() > limits.bytes || limits.depth > 64 || !limits.depth) {
        fail("metadata XML exceeds explicit bounds");
    }
    xml_text(xml);
    Counts counts;
    std::array<std::string_view, 64> stack{};
    std::size_t depth = 0, p = xml.starts_with("\xef\xbb\xbf") ? 3 : 0;
    bool root = false;
    auto whitespace = [&] {
        while (p < xml.size() && space(xml[p])) {
            ++p;
        }
    };
    auto name = [&] {
        const auto begin = p;
        while (p < xml.size() && name_char(xml[p])) {
            ++p;
        }
        if (begin == p || (p < xml.size() && xml[p] == ':')) {
            fail("invalid or namespaced metadata name");
        }
        return xml.substr(begin, p - begin);
    };
    auto value = [&] {
        if (p == xml.size() || (xml[p] != '"' && xml[p] != '\'')) {
            fail("quoted metadata attribute required");
        }
        const auto quote = xml[p++];
        const auto begin = p;
        while (p < xml.size() && xml[p] != quote) {
            if (xml[p] == '<') {
                fail("invalid metadata attribute character");
            }
            ++p;
        }
        if (p == xml.size()) {
            fail("unterminated metadata attribute");
        }
        auto result = xml.substr(begin, p - begin);
        ++p;
        entities(result);
        if (result.size() > checked(checked_multiply(limits.string_bytes, 6))) {
            fail("metadata attribute exceeds text bound");
        }
        return result;
    };
    if (xml.substr(p).starts_with("<?xml")) {
        p += 5;
        ++counts.nodes;
        bool version = false, encoding = false, standalone = false;
        for (;;) {
            const auto before = p;
            whitespace();
            if (xml.substr(p).starts_with("?>")) {
                p += 2;
                break;
            }
            if (p == before) {
                fail("invalid XML declaration");
            }
            const auto key = name();
            whitespace();
            if (p == xml.size() || xml[p++] != '=') {
                fail("invalid declaration attribute");
            }
            whitespace();
            const auto v = value();
            if (key == "version" && !version && !encoding && !standalone && v == "1.0") {
                version = true;
            } else if (key == "encoding" && version && !encoding && !standalone &&
                       (v == "UTF-8" || v == "utf-8")) {
                encoding = true;
            } else if (key == "standalone" && version && !standalone && (v == "yes" || v == "no")) {
                standalone = true;
            } else {
                fail("unsupported XML declaration");
            }
        }
        if (!version) {
            fail("missing XML version");
        }
    }
    while (p < xml.size()) {
        if (xml[p] != '<') {
            const auto begin = p;
            while (p < xml.size() && xml[p] != '<') {
                ++p;
            }
            ++counts.nodes;
            auto content = xml.substr(begin, p - begin);
            entities(content);
            if (depth == 0 && !std::all_of(content.begin(), content.end(), space)) {
                fail("text outside metadata root");
            }
            continue;
        }
        if (xml.substr(p).starts_with("<!--")) {
            ++counts.nodes;
            auto end = xml.find("-->", p + 4);
            if (end == std::string_view::npos ||
                xml.substr(p + 4, end - p - 4).find("--") != std::string_view::npos) {
                fail("invalid metadata comment");
            }
            p = end + 3;
            continue;
        }
        ++p;
        if (p == xml.size() || xml[p] == '!' || xml[p] == '?') {
            fail("unsupported metadata declaration");
        }
        if (xml[p] == '/') {
            ++p;
            auto close = name();
            whitespace();
            if (!depth || stack[depth - 1] != close || p == xml.size() || xml[p++] != '>') {
                fail("mismatched metadata closing tag");
            }
            --depth;
            continue;
        }
        const auto tag = name();
        ++counts.nodes;
        if ((!depth && root) || depth >= limits.depth || ++counts.elements > limits.elements) {
            fail("metadata root/depth/count bound");
        }
        if (!depth) {
            root = true;
        }
        std::array<std::string_view, 32> names{};
        std::size_t attributes = 0;
        bool closed = false, ended = false;
        while (p < xml.size()) {
            auto before = p;
            whitespace();
            if (xml.substr(p).starts_with("/>")) {
                p += 2;
                closed = ended = true;
                break;
            }
            if (p < xml.size() && xml[p] == '>') {
                ++p;
                ended = true;
                break;
            }
            if (before == p || attributes == names.size()) {
                fail("metadata attribute separation/count");
            }
            auto key = name();
            if (key == "xmlns" ||
                std::find(names.begin(), names.begin() + static_cast<std::ptrdiff_t>(attributes),
                          key) != names.begin() + static_cast<std::ptrdiff_t>(attributes)) {
                fail("duplicate or namespace attribute");
            }
            names[attributes++] = key;
            ++counts.attrs;
            whitespace();
            if (p == xml.size() || xml[p++] != '=') {
                fail("missing attribute equals");
            }
            whitespace();
            value();
        }
        if (!ended) {
            fail("unterminated metadata element");
        }
        if (!closed) {
            stack[depth++] = tag;
        }
    }
    if (!root || depth) {
        fail("incomplete metadata XML");
    }
    return counts;
}
std::string scalar(const Element &e, std::string_view kind) {
    if (e.Name() != kind) {
        fail("wrong scalar metadata kind");
    }
    attributes(e, {});
    std::string result;
    for (auto *n = e.FirstChild(); n; n = n->NextSibling()) {
        const auto *t = n->ToText();
        if (!t || t->CData()) {
            fail("scalar metadata requires plain text");
        }
        result += t->Value();
    }
    return result;
}
void whitespace_nodes(const Element &e) {
    for (auto *n = e.FirstChild(); n; n = n->NextSibling()) {
        if (n->ToElement() || n->ToComment()) {
            continue;
        }
        const auto *t = n->ToText();
        if (!t || t->CData() ||
            !std::all_of(t->Value(), t->Value() + std::char_traits<char>::length(t->Value()),
                         space)) {
            fail("unexpected text in metadata container");
        }
    }
}
std::vector<std::string_view> names(std::string_view schema) {
    std::vector<std::string_view> result;
    while (!schema.empty()) {
        auto comma = schema.find(',');
        result.push_back(schema.substr(0, comma));
        if (comma == std::string_view::npos) {
            break;
        }
        schema.remove_prefix(comma + 1);
    }
    return result;
}
std::uint8_t discriminator(std::string_view kind) {
    if (kind == "signed64" || kind == "point" || kind == "count") {
        return 1;
    }
    if (kind == "unsigned64" || kind == "interval" || kind == "time") {
        return 2;
    }
    if (kind == "boolean" || kind == "prefix") {
        return 3;
    }
    if (kind == "text") {
        return 4;
    }
    fail("unsupported metadata variant");
}
void tree_limits(const Element &e, const Limits &limits, std::uint64_t depth = 1) {
    if (depth > limits.depth) {
        fail("metadata tree exceeds depth bound");
    }
    for (auto *a = e.FirstAttribute(); a; a = a->Next()) {
        if (std::string_view{a->Value()}.size() > limits.string_bytes) {
            fail("decoded metadata attribute exceeds text bound");
        }
    }
    const auto *parent = e.Parent() ? e.Parent()->ToElement() : nullptr;
    const bool identity =
        parent && (std::string_view{parent->Name()} == "canonical_identity" ||
                   (std::string_view{parent->Name()} == "field" && parent->Attribute("name") &&
                    std::string_view{parent->Attribute("name")} == "canonical_identity"));
    std::uint64_t text_size = 0;
    for (auto *n = e.FirstChild(); n; n = n->NextSibling()) {
        if (const auto *child = n->ToElement()) {
            tree_limits(*child, limits, depth + 1);
        } else if (const auto *text = n->ToText()) {
            text_size = checked(checked_add(text_size, std::string_view{text->Value()}.size()));
        }
    }
    if (text_size > (identity ? limits.bytes : limits.string_bytes)) {
        fail("decoded metadata text exceeds field bound");
    }
}
} // namespace
Result<Document> Document::create(std::shared_ptr<ResourceAccount> account,
                                  Limits limits) noexcept {
    return protect([&]() -> Result<Document> {
        if (!account || limits.workspace_bytes < 65536 || !limits.bytes || !limits.elements ||
            !limits.string_bytes || !limits.depth || limits.depth > 64) {
            return diagnostic(FailureCode::InvalidConfig, "invalid metadata codec limits");
        }
        auto charge = checked(account->reserve(ResourceKind::Scratch, limits.workspace_bytes));
        return Document{std::move(charge), limits, std::make_unique<tinyxml2::XMLDocument>(),
                        std::move(account)};
    });
}
Result<Document> Document::parse(std::string_view xml, std::shared_ptr<ResourceAccount> account,
                                 Limits limits) noexcept {
    return protect([&]() -> Result<Document> {
        const auto count = preflight(xml, limits);
        auto needed = checked(checked_add(
            65536, checked(checked_add(
                       checked(checked_multiply(xml.size(), 2)),
                       checked(checked_add(checked(checked_multiply(count.nodes, 256)),
                                           checked(checked_multiply(count.attrs, 128))))))));
        if (needed > limits.workspace_bytes) {
            return diagnostic(FailureCode::ResourcePlanExceeded,
                              "metadata DOM exceeds admitted workspace");
        }
        auto result = checked(create(std::move(account), limits));
        if (result.dom().Parse(xml.data(), xml.size()) != tinyxml2::XML_SUCCESS) {
            fail("malformed metadata XML");
        }
        tree_limits(result.root(), limits);
        return result;
    });
}
const Element &Document::root() const {
    const auto *r = document_->RootElement();
    if (!r || r->NextSiblingElement()) {
        fail("missing or duplicate metadata root");
    }
    return *r;
}
Element *Document::element(std::string_view name) {
    if (++elements_ > limits_.elements ||
        checked(
            checked_add(65536, checked(checked_add(checked(checked_multiply(elements_, 512)),
                                                   checked(checked_multiply(text_bytes_, 3)))))) >
            limits_.workspace_bytes) {
        throw Error{diagnostic(FailureCode::ResourcePlanExceeded,
                               "metadata construction exceeds workspace")};
    }
    return document_->NewElement(std::string{name}.c_str());
}
Element *Document::text(std::string_view value) {
    xml_text(value);
    text_bytes_ = checked(checked_add(text_bytes_, value.size()));
    if (value.size() > limits_.bytes) {
        fail("metadata text exceeds byte limit");
    }
    auto *e = element("text");
    e->SetText(std::string{value}.c_str());
    return e;
}
Element *Document::u64(std::uint64_t value) {
    auto *e = element("u64");
    e->SetText(std::to_string(value).c_str());
    return e;
}
Element *Document::i64(std::int64_t value) {
    auto *e = element("i64");
    e->SetText(std::to_string(value).c_str());
    return e;
}
Element *Document::boolean(bool value) {
    auto *e = element("bool");
    e->SetText(value ? "true" : "false");
    return e;
}
Element *Document::digest(const Sha256 &value) {
    auto *e = element("digest");
    e->SetText(hex(value).c_str());
    return e;
}
Element *Document::list() {
    return element("list");
}
void Document::item(Element *list, Element *value) {
    auto *i = element("item");
    i->InsertEndChild(value);
    list->InsertEndChild(i);
}
Element *Document::record(std::string_view type,
                          std::initializer_list<std::pair<std::string_view, Element *>> fields) {
    auto *r = element("record");
    attribute(r, "type", type);
    const auto *schema = schema_descriptor(type);
    if (!schema) {
        fail("unknown metadata record");
    }
    auto expected = names(schema->fields);
    if (expected.size() != fields.size()) {
        fail("incomplete metadata projection");
    }
    std::size_t i = 0;
    for (const auto &[name, value] : fields) {
        if (expected[i++] != name) {
            fail("reordered metadata projection");
        }
        auto *f = element("field");
        attribute(f, "name", name);
        f->InsertEndChild(value);
        r->InsertEndChild(f);
    }
    return r;
}
Element *Document::optional(Element *value) {
    auto *e = element("optional");
    attribute(e, "present", value ? "true" : "false");
    if (value) {
        e->InsertEndChild(value);
    }
    return e;
}
Element *Document::variant(std::string_view kind, Element *value) {
    static_cast<void>(discriminator(kind));
    auto *e = element("variant");
    attribute(e, "kind", kind);
    e->InsertEndChild(value);
    return e;
}
void Document::attribute(Element *e, std::string_view name, std::string_view value) {
    xml_text(value);
    if (value.size() > limits_.string_bytes) {
        fail("metadata attribute text exceeds bound");
    }
    text_bytes_ = checked(checked_add(text_bytes_, name.size() + value.size() + 32));
    if (checked(
            checked_add(65536, checked(checked_add(checked(checked_multiply(elements_, 512)),
                                                   checked(checked_multiply(text_bytes_, 3)))))) >
        limits_.workspace_bytes) {
        throw Error{
            diagnostic(FailureCode::ResourcePlanExceeded, "metadata attributes exceed workspace")};
    }
    e->SetAttribute(std::string{name}.c_str(), std::string{value}.c_str());
}
void Document::attribute(Element *e, std::string_view name, std::uint64_t value) {
    attribute(e, name, std::to_string(value));
}
Element *Document::copy(const Element &source) {
    auto *result = element(source.Name());
    for (auto *a = source.FirstAttribute(); a; a = a->Next()) {
        attribute(result, a->Name(), a->Value());
    }
    for (auto *n = source.FirstChild(); n; n = n->NextSibling()) {
        if (const auto *child = n->ToElement()) {
            result->InsertEndChild(copy(*child));
        } else if (const auto *text = n->ToText()) {
            if (text->CData()) {
                fail("CDATA cannot enter a metadata projection");
            }
            text_bytes_ = checked(checked_add(text_bytes_, std::string_view{text->Value()}.size()));
            if (checked(checked_add(
                    65536, checked(checked_add(checked(checked_multiply(elements_, 512)),
                                               checked(checked_multiply(text_bytes_, 3)))))) >
                limits_.workspace_bytes) {
                throw Error{diagnostic(FailureCode::ResourcePlanExceeded,
                                       "metadata copy exceeds workspace")};
            }
            result->InsertEndChild(document_->NewText(text->Value()));
        } else if (!n->ToComment()) {
            fail("unsupported copied metadata node");
        }
    }
    return result;
}
namespace {
struct XmlOutput {
    std::uint64_t maximum, size = 0;
    std::string *output = nullptr;
    void add(std::string_view value) {
        if (value.size() > maximum - size) {
            throw Error{
                diagnostic(FailureCode::ResourcePlanExceeded, "metadata XML exceeds byte limit")};
        }
        if (output) {
            std::copy(value.begin(), value.end(),
                      output->begin() + static_cast<std::ptrdiff_t>(size));
        }
        size += value.size();
    }
    void escaped(std::string_view value) {
        for (char c : value) {
            if (c == '&') {
                add("&amp;");
            } else if (c == '<') {
                add("&lt;");
            } else if (c == '>') {
                add("&gt;");
            } else if (c == '"') {
                add("&quot;");
            } else if (c == '\'') {
                add("&apos;");
            } else if (c == '\r') {
                add("&#13;");
            } else if (c == '\n') {
                add("&#10;");
            } else if (c == '\t') {
                add("&#9;");
            } else {
                add({&c, 1});
            }
        }
    }
};
void output_xml(XmlOutput &output, const Element &e) {
    output.add("<");
    output.add(e.Name());
    for (auto *a = e.FirstAttribute(); a; a = a->Next()) {
        output.add(" ");
        output.add(a->Name());
        output.add("=\"");
        output.escaped(a->Value());
        output.add("\"");
    }
    if (!e.FirstChild()) {
        output.add("/>");
        return;
    }
    output.add(">");
    for (auto *n = e.FirstChild(); n; n = n->NextSibling()) {
        if (auto *child = n->ToElement()) {
            output_xml(output, *child);
        } else if (auto *t = n->ToText()) {
            if (t->CData()) {
                fail("CDATA is unsupported in metadata");
            }
            output.escaped(t->Value());
        } else if (!n->ToComment()) {
            fail("unsupported metadata node");
        }
    }
    output.add("</");
    output.add(e.Name());
    output.add(">");
}
} // namespace
Result<EncodedXml> Document::render() const noexcept {
    return protect([&]() -> Result<EncodedXml> {
        tree_limits(root(), limits_);
        XmlOutput counter{limits_.bytes};
        output_xml(counter, root());
        auto charge = checked(
            account_->reserve(ResourceKind::Staging, checked(checked_add(counter.size, 256))));
        std::string text(static_cast<std::size_t>(counter.size), '\0');
        XmlOutput writer{counter.size, 0, &text};
        output_xml(writer, root());
        return EncodedXml{std::move(charge), std::move(text)};
    });
}
void attributes(const Element &e, std::initializer_list<std::string_view> expected) {
    std::size_t n = 0;
    for (auto *a = e.FirstAttribute(); a; a = a->Next()) {
        if (std::find(expected.begin(), expected.end(), a->Name()) == expected.end()) {
            fail("unknown metadata attribute");
        }
        ++n;
    }
    if (n != expected.size()) {
        fail("missing metadata attribute");
    }
}
std::string_view attribute(const Element &e, const char *name) {
    const char *value = e.Attribute(name);
    if (!value) {
        fail("missing metadata attribute");
    }
    return value;
}
std::vector<const Element *> children(const Element &e) {
    whitespace_nodes(e);
    std::vector<const Element *> out;
    for (auto *c = e.FirstChildElement(); c; c = c->NextSiblingElement()) {
        out.push_back(c);
    }
    return out;
}
const Element &only_child(const Element &e) {
    auto list = children(e);
    if (list.size() != 1) {
        fail("metadata field requires exactly one value");
    }
    return *list[0];
}
std::vector<const Element *> fields(const Element &e, std::string_view type) {
    if (std::string_view{e.Name()} != "record") {
        fail("metadata record required");
    }
    attributes(e, {"type"});
    if (attribute(e, "type") != type) {
        fail("metadata record type mismatch");
    }
    const auto *schema = schema_descriptor(type);
    if (!schema) {
        fail("unknown metadata schema");
    }
    auto expected = names(schema->fields);
    auto list = children(e);
    if (list.size() != expected.size()) {
        fail("missing or duplicate metadata fields");
    }
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (std::string_view{list[i]->Name()} != "field") {
            fail("metadata field required");
        }
        attributes(*list[i], {"name"});
        if (attribute(*list[i], "name") != expected[i]) {
            fail("unknown or reordered metadata field");
        }
        list[i] = &only_child(*list[i]);
    }
    return list;
}
std::string text(const Element &e) {
    return scalar(e, "text");
}
std::uint64_t u64(const Element &e) {
    auto r = parse_integer<std::uint64_t>(scalar(e, "u64"));
    if (std::holds_alternative<Diagnostic>(r)) {
        fail("noncanonical metadata uint64");
    }
    return std::get<std::uint64_t>(r);
}
std::int64_t i64(const Element &e) {
    auto r = parse_integer<std::int64_t>(scalar(e, "i64"));
    if (std::holds_alternative<Diagnostic>(r)) {
        fail("noncanonical metadata int64");
    }
    return std::get<std::int64_t>(r);
}
bool boolean(const Element &e) {
    auto v = scalar(e, "bool");
    if (v != "true" && v != "false") {
        fail("invalid metadata boolean");
    }
    return v == "true";
}
std::string hex(std::span<const std::byte> bytes) {
    constexpr std::string_view chars = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (auto b : bytes) {
        auto v = std::to_integer<unsigned>(b);
        out.push_back(chars[v >> 4]);
        out.push_back(chars[v & 15]);
    }
    return out;
}
std::vector<std::byte> unhex(std::string_view value, std::uint64_t maximum) {
    if (value.size() % 2 || value.size() / 2 > maximum) {
        fail("metadata hex length exceeds bound");
    }
    std::vector<std::byte> out;
    out.reserve(value.size() / 2);
    auto digit = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') {
            return static_cast<unsigned>(c - '0');
        }
        if (c >= 'a' && c <= 'f') {
            return static_cast<unsigned>(c - 'a' + 10);
        }
        fail("metadata hex must be lowercase");
    };
    for (std::size_t i = 0; i < value.size(); i += 2) {
        out.push_back(static_cast<std::byte>((digit(value[i]) << 4) | digit(value[i + 1])));
    }
    return out;
}
Sha256 digest_hex(std::string_view value) {
    if (value.size() != 64) {
        fail("metadata digest must have 64 hex digits");
    }
    auto bytes = unhex(value, 32);
    Sha256 out{};
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return out;
}
Sha256 digest(const Element &e) {
    return digest_hex(scalar(e, "digest"));
}
namespace {
void shape(const Element &e, std::string_view expected) {
    const std::string_view tag = e.Name();
    if (expected.starts_with("[]")) {
        if (tag != "list") {
            fail("typed metadata list required");
        }
        for (auto *item : children(e)) {
            shape(only_child(*item), expected.substr(2));
        }
    } else if (expected.starts_with('?')) {
        if (tag != "optional") {
            fail("typed metadata optional required");
        }
        if (attribute(e, "present") == "true") {
            shape(only_child(e), expected.substr(1));
        }
    } else if (expected == "param" || expected == "extent") {
        if (tag != "variant") {
            fail("typed metadata variant required");
        }
        const auto kind = attribute(e, "kind");
        if (expected == "param"
                ? kind != "signed64" && kind != "unsigned64" && kind != "boolean" && kind != "text"
                : kind != "point" && kind != "interval") {
            fail("variant is outside its registered field alternatives");
        }
    } else if (expected == "u32") {
        if (u64(e) > std::numeric_limits<std::uint32_t>::max()) {
            fail("metadata uint32 overflow");
        }
    } else if (expected == "text" || expected == "u64" || expected == "i64" || expected == "bool" ||
               expected == "digest") {
        if (tag != expected) {
            fail("metadata field scalar type mismatch");
        }
    } else {
        if (tag != "record") {
            fail("typed metadata record required");
        }
        const auto type = attribute(e, "type");
        bool match = false;
        while (!expected.empty()) {
            auto end = expected.find('|');
            if (type == expected.substr(0, end)) {
                match = true;
                break;
            }
            if (end == std::string_view::npos) {
                break;
            }
            expected.remove_prefix(end + 1);
        }
        if (!match) {
            fail("metadata field record type mismatch");
        }
    }
}
} // namespace
void validate_value(const Element &e) {
    const std::string_view tag = e.Name();
    if (tag == "record") {
        const auto type = attribute(e, "type");
        const auto *schema = schema_value_shape(type);
        if (!schema) {
            fail("metadata record has no supported typed projection");
        }
        const auto values = fields(e, type);
        const auto shapes = names(schema->fields);
        if (values.size() != shapes.size()) {
            fail("metadata field shape schema mismatch");
        }
        for (std::size_t i = 0; i < values.size(); ++i) {
            validate_value(*values[i]);
            shape(*values[i], shapes[i]);
        }
    } else if (tag == "list") {
        attributes(e, {});
        for (auto *i : children(e)) {
            if (std::string_view{i->Name()} != "item") {
                fail("list item required");
            }
            attributes(*i, {});
            validate_value(only_child(*i));
        }
    } else if (tag == "optional") {
        attributes(e, {"present"});
        auto c = children(e);
        auto p = attribute(e, "present");
        if ((p != "true" && p != "false") || c.size() != (p == "true" ? 1U : 0U)) {
            fail("invalid optional metadata");
        }
        if (!c.empty()) {
            validate_value(*c[0]);
        }
    } else if (tag == "variant") {
        attributes(e, {"kind"});
        const auto kind = attribute(e, "kind");
        const auto &value = only_child(e);
        validate_value(value);
        if (kind == "signed64") {
            shape(value, "i64");
        } else if (kind == "unsigned64") {
            shape(value, "u64");
        } else if (kind == "boolean") {
            shape(value, "bool");
        } else if (kind == "text") {
            shape(value, "text");
        } else if (kind == "point") {
            shape(value, "PointTimeV1");
        } else if (kind == "interval") {
            shape(value, "IntervalV1");
        } else if (kind == "count") {
            shape(value, "CountHistoryV1");
        } else {
            fail("unsupported typed metadata variant");
        }
    } else if (tag == "text") {
        static_cast<void>(text(e));
    } else if (tag == "u64") {
        static_cast<void>(u64(e));
    } else if (tag == "i64") {
        static_cast<void>(i64(e));
    } else if (tag == "bool") {
        static_cast<void>(boolean(e));
    } else if (tag == "digest") {
        static_cast<void>(digest(e));
    } else {
        fail("unknown metadata value kind");
    }
}
namespace {
struct Encoder {
    std::vector<std::byte> bytes;
    std::uint64_t max;
    void raw(std::span<const std::byte> v) {
        if (v.size() > max - bytes.size()) {
            throw Error{
                diagnostic(FailureCode::ResourcePlanExceeded, "canonical metadata exceeds bound")};
        }
        bytes.insert(bytes.end(), v.begin(), v.end());
    }
    void n(std::uint64_t v) {
        std::array<std::byte, 8> b{};
        for (unsigned i = 0; i < 8; ++i) {
            b[i] = static_cast<std::byte>((v >> ((7 - i) * 8)) & 255);
        }
        raw(b);
    }
    void one(std::uint8_t v) {
        const std::byte b{v};
        raw({&b, 1});
    }
    void tag(std::uint16_t v) {
        one(static_cast<std::uint8_t>(v >> 8));
        one(static_cast<std::uint8_t>(v & 255));
    }
    template <class F> void frame(F f) {
        const auto at = bytes.size();
        n(0);
        const auto begin = bytes.size();
        f();
        const auto len = bytes.size() - begin;
        for (unsigned i = 0; i < 8; ++i) {
            bytes[at + i] = static_cast<std::byte>((len >> ((7 - i) * 8)) & 255);
        }
    }
};
void encode(Encoder &out, const Element &e) {
    const std::string_view kind = e.Name();
    if (kind == "record") {
        auto fs = fields(e, attribute(e, "type"));
        for (std::size_t i = 0; i < fs.size(); ++i) {
            out.tag(static_cast<std::uint16_t>(i + 1));
            out.frame([&] { encode(out, *fs[i]); });
        }
    } else if (kind == "list") {
        auto cs = children(e);
        out.n(cs.size());
        for (auto *c : cs) {
            out.frame([&] { encode(out, only_child(*c)); });
        }
    } else if (kind == "text") {
        auto s = text(e);
        out.raw(std::as_bytes(std::span{s.data(), s.size()}));
    } else if (kind == "digest") {
        out.raw(digest(e));
    } else if (kind == "u64") {
        out.n(u64(e));
    } else if (kind == "i64") {
        out.n(static_cast<std::uint64_t>(i64(e)));
    } else if (kind == "bool") {
        out.one(boolean(e) ? 1 : 0);
    } else if (kind == "optional") {
        const bool present = attribute(e, "present") == "true";
        out.one(present ? 1 : 0);
        if (present) {
            encode(out, only_child(e));
        }
    } else if (kind == "variant") {
        out.one(discriminator(attribute(e, "kind")));
        encode(out, only_child(e));
    } else {
        fail("unsupported canonical metadata value");
    }
}
} // namespace
std::vector<std::byte> canonical(const Element &e, std::uint16_t kind, std::uint64_t maximum) {
    validate_value(e);
    Encoder out{{}, maximum};
    constexpr std::string_view domain = "lexongine.identity.v1";
    out.n(domain.size());
    out.raw(std::as_bytes(std::span{domain.data(), domain.size()}));
    out.tag(kind);
    encode(out, e);
    return std::move(out.bytes);
}
Element *project(Document &d, const CanonicalParams &p) {
    auto *values = d.list();
    for (const auto &v : p.values) {
        auto *scalar_value = std::visit(
            [&](const auto &x) -> Element * {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<T, std::string>) {
                    return d.variant("text", d.text(x));
                } else if constexpr (std::is_same_v<T, bool>) {
                    return d.variant("boolean", d.boolean(x));
                } else if constexpr (std::is_same_v<T, std::int64_t>) {
                    return d.variant("signed64", d.i64(x));
                } else {
                    return d.variant("unsigned64", d.u64(x));
                }
            },
            v.value);
        d.item(values,
               d.record("NamedParamV1", {{"name", d.text(v.name)}, {"value", scalar_value}}));
    }
    return d.record("CanonicalParamsV1",
                    {{"schema_version", d.u64(p.schema_version)}, {"values", values}});
}
Element *project(Document &d, const Contract &v) {
    return d.record("ContractV1", {{"id", d.text(v.id)},
                                   {"version", d.u64(v.version)},
                                   {"parameters", project(d, v.parameters)}});
}
Element *project(Document &d, const TypeContract &v) {
    return d.record("TypeContractV1", {{"nominal_name", d.text(v.nominal_name)},
                                       {"semantic_version", d.u64(v.semantic_version)},
                                       {"meaning", project(d, v.meaning)}});
}
Element *project(Document &d, const SequenceScope &v) {
    return d.record("SequenceScopeV1",
                    {{"clock", d.text(v.clock)}, {"begin", d.u64(v.begin)}, {"end", d.u64(v.end)}});
}
Element *project(Document &d, const UpdateKey &v) {
    return d.record("UpdateKeyV1", {{"clock", d.text(v.clock)}, {"sequence", d.u64(v.sequence)}});
}
Element *project(Document &d, const Interval &v) {
    return d.record("IntervalV1", {{"begin_ns", d.i64(v.begin_ns)}, {"end_ns", d.i64(v.end_ns)}});
}
Element *project(Document &d, const TemporalExtent &v) {
    if (const auto *p = std::get_if<PointTime>(&v)) {
        return d.variant("point",
                         d.record("PointTimeV1", {{"event_time_ns", d.i64(p->event_time_ns)}}));
    }
    return d.variant("interval", project(d, std::get<Interval>(v)));
}
Element *project(Document &d, const SourceCoverage &v) {
    auto *keys = d.list();
    for (const auto &k : v.complete_keys) {
        d.item(keys, project(d, k));
    }
    auto *gaps = d.list();
    for (const auto &g : v.gaps) {
        d.item(gaps, d.record("GapV1", {{"missing_scope", project(d, g.missing_scope)},
                                        {"interval", project(d, g.interval)},
                                        {"declared_reason", d.u64(g.declared_reason)}}));
    }
    return d.record("CoverageV1",
                    {{"expected_scope", project(d, v.expected_scope)},
                     {"complete_keys", keys},
                     {"gaps", gaps},
                     {"expected_timeline_contract", project(d, v.expected_timeline_contract)}});
}
Element *project(Document &d, const InitializationSpec &v) {
    return d.record("InitializationV1", {{"contract", project(d, v.contract)},
                                         {"actual_prefix", project(d, v.actual_prefix)},
                                         {"explicit_values", project(d, v.explicit_values)}});
}
Element *project(Document &d, const EvidenceReference &v) {
    return d.record("EvidenceReferenceV1", {{"name", d.text(v.name)},
                                            {"absolute_path", d.text(v.absolute_path)},
                                            {"byte_count", d.u64(v.byte_count)},
                                            {"digest", d.digest(v.digest)},
                                            {"finalized", d.boolean(v.finalized)}});
}
namespace {
std::uint32_t narrow(std::uint64_t v) {
    if (v > std::numeric_limits<std::uint32_t>::max()) {
        fail("metadata uint32 overflow");
    }
    return static_cast<std::uint32_t>(v);
}
std::vector<const Element *> items(const Element &e) {
    if (std::string_view{e.Name()} != "list") {
        fail("metadata list required");
    }
    attributes(e, {});
    auto cs = children(e);
    for (auto *&c : cs) {
        if (std::string_view{c->Name()} != "item") {
            fail("metadata list item required");
        }
        attributes(*c, {});
        c = &only_child(*c);
    }
    return cs;
}
} // namespace
CanonicalParams parameters(const Element &e) {
    auto fs = fields(e, "CanonicalParamsV1");
    CanonicalParams result{narrow(u64(*fs[0])), {}};
    for (auto *p : items(*fs[1])) {
        auto values = fields(*p, "NamedParamV1");
        auto name = text(*values[0]);
        const auto &v = *values[1];
        if (std::string_view{v.Name()} != "variant") {
            fail("parameter variant required");
        }
        attributes(v, {"kind"});
        auto kind = attribute(v, "kind");
        const auto &scalar_value = only_child(v);
        ParamValue value;
        if (kind == "signed64") {
            value = i64(scalar_value);
        } else if (kind == "unsigned64") {
            value = u64(scalar_value);
        } else if (kind == "boolean") {
            value = boolean(scalar_value);
        } else if (kind == "text") {
            value = text(scalar_value);
        } else {
            fail("unknown parameter kind");
        }
        result.values.push_back(
            {std::move(name), std::move(value), Origin::Derived, "decoded metadata"});
    }
    auto canonical = checked(canonical_params(result.schema_version, result.values,
                                              std::numeric_limits<std::uint64_t>::max()));
    if (!(result == canonical)) {
        fail("metadata parameters are not canonical");
    }
    return result;
}
Contract contract(const Element &e) {
    auto f = fields(e, "ContractV1");
    Contract v{text(*f[0]), narrow(u64(*f[1])), parameters(*f[2])};
    if (v.id.empty() || !v.version) {
        fail("invalid metadata contract");
    }
    return v;
}
TypeContract type(const Element &e) {
    auto f = fields(e, "TypeContractV1");
    TypeContract v{text(*f[0]), narrow(u64(*f[1])), parameters(*f[2])};
    if (v.nominal_name.empty() || !v.semantic_version) {
        fail("invalid metadata type");
    }
    return v;
}
SequenceScope scope(const Element &e) {
    auto f = fields(e, "SequenceScopeV1");
    SequenceScope v{text(*f[0]), u64(*f[1]), u64(*f[2])};
    if (v.clock.empty() || v.end < v.begin) {
        fail("invalid metadata scope");
    }
    return v;
}
UpdateKey key(const Element &e) {
    auto f = fields(e, "UpdateKeyV1");
    UpdateKey v{text(*f[0]), u64(*f[1])};
    if (v.clock.empty()) {
        fail("empty metadata clock");
    }
    return v;
}
Interval interval(const Element &e) {
    auto f = fields(e, "IntervalV1");
    Interval v{i64(*f[0]), i64(*f[1])};
    if (v.end_ns <= v.begin_ns) {
        fail("invalid metadata interval");
    }
    return v;
}
TemporalExtent extent(const Element &e) {
    if (std::string_view{e.Name()} != "variant") {
        fail("temporal variant required");
    }
    attributes(e, {"kind"});
    if (attribute(e, "kind") == "point") {
        auto f = fields(only_child(e), "PointTimeV1");
        return PointTime{i64(*f[0])};
    }
    if (attribute(e, "kind") == "interval") {
        return interval(only_child(e));
    }
    fail("unknown temporal kind");
}
SourceCoverage coverage(const Element &e) {
    auto f = fields(e, "CoverageV1");
    SourceCoverage v{scope(*f[0]), {}, {}, contract(*f[3])};
    for (auto *k : items(*f[1])) {
        v.complete_keys.push_back(key(*k));
    }
    for (auto *g : items(*f[2])) {
        auto gf = fields(*g, "GapV1");
        v.gaps.push_back({scope(*gf[0]), interval(*gf[1]), narrow(u64(*gf[2]))});
    }
    return v;
}
InitializationSpec initialization(const Element &e) {
    auto f = fields(e, "InitializationV1");
    return {contract(*f[0]), scope(*f[1]), parameters(*f[2])};
}
EvidenceReference evidence(const Element &e) {
    auto f = fields(e, "EvidenceReferenceV1");
    return {text(*f[0]), text(*f[1]), u64(*f[2]), digest(*f[3]), boolean(*f[4])};
}
} // namespace lexongine::wire
