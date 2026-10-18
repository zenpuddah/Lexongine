#include "metadata_xml.hpp"
#include "../../tests/lexongine_test_support.hpp"
namespace {
using namespace lexongine;
using namespace lexongine::test;
std::shared_ptr<ResourceAccount> account() { return take(ResourceAccount::create({{1048576,1048576,16777216,1048576,1048576,1048576,1048576},128,128,1024,128})); }
wire::Limits limits() { return {1048576,16384,32,4096,1048576}; }
void complete_records_and_canonical_bytes() {
    auto resources = account(); auto document = take(wire::Document::create(resources, limits()));
    const SourceCoverage expected{{"clock&x",0,3}, {{"clock&x",0},{"clock&x",2}}, {{{"clock&x",1,2},{10,20},7}}, {"intervals",1,{1,{}}}};
    document.dom().InsertEndChild(wire::project(document, expected)); const auto xml = take(document.render());
    auto parsed = take(wire::Document::parse(xml.bytes(), resources, limits())); require(wire::coverage(parsed.root()) == expected, "coverage projection lost keys/gap/clock meaning");
    auto small = take(wire::Document::create(resources, limits())); small.dom().InsertEndChild(wire::project(small, SequenceScope{"c",2,4}));
    const auto bytes = wire::canonical(small.root(), 4, 1024);
    std::vector<std::byte> reference;
    auto n = [&](std::uint64_t v) { for (int i=7;i>=0;--i) { reference.push_back(static_cast<std::byte>((v>>(i*8))&255)); } };
    constexpr std::string_view domain="lexongine.identity.v1"; n(domain.size()); for(char c:domain){reference.push_back(static_cast<std::byte>(c));}
    reference.insert(reference.end(), {std::byte{0},std::byte{4},std::byte{0},std::byte{1}}); n(1); reference.push_back(std::byte{'c'});
    reference.insert(reference.end(), {std::byte{0},std::byte{2}}); n(8); n(2); reference.insert(reference.end(), {std::byte{0},std::byte{3}}); n(8); n(4);
    require(bytes == reference, "canonical metadata framing differs from independently assembled bytes");
}
void malformed_metadata_rejects() {
    auto resources=account();
    for(const auto* xml:{"<!DOCTYPE x><record type=\"SequenceScopeV1\"/>", "<record type=\"SequenceScopeV1\" type=\"SequenceScopeV1\"/>", "<record xmlns=\"x\"/>", "<record>&unknown;</record>", "<?other?><record/>"}) {
        rejects(wire::Document::parse(xml,resources,limits()),FailureCode::CacheCorrupt);
    }
    auto missing=take(wire::Document::parse("<record type=\"SequenceScopeV1\"><field name=\"clock\"><text>c</text></field></record>",resources,limits()));
    auto result=wire::protect([&]()->Result<SequenceScope>{return wire::scope(missing.root());}); rejects(std::move(result),FailureCode::CacheCorrupt);
    auto bad=take(wire::Document::parse("<u64>01</u64>",resources,limits()));
    rejects(wire::protect([&]()->Result<std::uint64_t>{return wire::u64(bad.root());}),FailureCode::CacheCorrupt);
}
void record_value_types_reject() {
    for (const auto* xml : {
        "<record type=\"SequenceScopeV1\"><field name=\"clock\"><u64>1</u64></field><field name=\"begin\"><u64>0</u64></field><field name=\"end\"><u64>2</u64></field></record>",
        "<variant kind=\"unsigned64\"><text>1</text></variant>",
        "<record type=\"ParentReference\"><field name=\"input_port\"><u64>0</u64></field><field name=\"publication\"><text>fake</text></field></record>",
        "<record type=\"EffectiveParametersV1\"><field name=\"schema_version\"><u64>1</u64></field><field name=\"values\"><list><item><text>fake</text></item></list></field></record>"}) {
        auto document = take(wire::Document::parse(xml, account(), limits()));
        rejects(wire::protect([&]() -> Result<void> { wire::validate_value(document.root()); return ok(); }), FailureCode::CacheCorrupt);
    }
}
void decoded_text_and_comment_workspace_are_bounded() {
    auto bounded=limits();bounded.string_bytes=4;
    rejects(wire::Document::parse("<text>12345</text>",account(),bounded),FailureCode::CacheCorrupt);
    std::string comments="<list>";for(unsigned i=0;i<5000;++i){comments+="<!--x-->";}comments+="</list>";
    rejects(wire::Document::parse(comments,account(),limits()),FailureCode::ResourcePlanExceeded);
    for(const auto& shape:schema_value_shapes){const auto* fields=schema_descriptor(shape.name);require(fields!=nullptr,"typed projection has no field schema");
        const auto count=[](std::string_view s){return s.empty()?0:1+std::count(s.begin(),s.end(),',');};
        require(count(fields->fields)==count(shape.fields),"typed projection does not cover all ordered fields");}
}
}
int main(){const Case cases[]{{"complete metadata records and independent framing",complete_records_and_canonical_bytes},{"strict metadata grammar and fields",malformed_metadata_rejects},{"record fields and variants enforce exact value types",record_value_types_reject},{"decoded text and comment workspace bounds",decoded_text_and_comment_workspace_are_bounded}};return lexongine::test::run(cases);}
