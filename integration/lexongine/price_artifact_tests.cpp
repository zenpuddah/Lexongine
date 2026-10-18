#include "price_artifact.hpp"
#include "quant/lexongine/components.hpp"
#include "../../tests/lexongine_test_support.hpp"

namespace {
using namespace lexongine;
using namespace lexongine::test;
using quant::data::Price;
std::shared_ptr<ResourceAccount> account() {
    return take(ResourceAccount::create({{1048576, 1048576, 8388608, 2097152, 1048576, 2097152, 1048576}, 256, 128, 4096, 128}));
}
PricePayloadLimits limits() { return {1048576, 16384, 256, 120, 128, 1048576, 8388608}; }
Contract protocol(std::string value) { return {std::move(value), 1, {1, {}}}; }
std::vector<OutputContract> ports() {
    return {{"price", quant::lexongine::price_contract(), StorageKind::ScalarCopy, protocol("qaedalus.price9_xml"), protocol("qaedalus.price9"), true}};
}
std::string replace(std::string value, std::string_view from, std::string_view to) {
    const auto where = value.find(from); require(where != std::string::npos, "test replacement missing"); value.replace(where, from.size(), to); return value;
}
std::string fixture() {
    return R"(<price_outcomes version="1" clock="c&amp;q" begin_step="0" end_step="3">
<outcome step="0" kind="warming" time_kind="point" event_time_ns="10" available_at_ns="11" remaining_preceding="1"><parents/></outcome>
<outcome step="2" kind="published" time_kind="interval" interval_begin_ns="20" interval_end_ns="30" available_at_ns="31">
<port name="price" type="qaedalus.Price" type_version="1" scale="9" raw="-9223372036854775808"/>
<parents>
<parent input_port="0" computation_digest="0100000000000000000000000000000000000000000000000000000000000000" clock="c&amp;q" step="0" port="2" time_kind="point" event_time_ns="10" available_at_ns="11" source_reference_digest="0200000000000000000000000000000000000000000000000000000000000000"/>
<parent input_port="1" computation_digest="0100000000000000000000000000000000000000000000000000000000000000" clock="c&amp;q" step="2" port="2" time_kind="interval" interval_begin_ns="20" interval_end_ns="30" available_at_ns="31"/>
</parents></outcome></price_outcomes>)";
}
struct Fixture {
    std::vector<OutputContract> outputs = ports();
    std::vector<UpdateKey> keys{{"c&q", 0}, {"c&q", 2}};
    PricePayloadContract contract() const { return {{"c&q", 0, 3}, outputs, keys}; }
    std::string xml() const { return replace(fixture(), "qaedalus.Price", outputs[0].type.nominal_name); }
};
void independent_payload_and_ownership() {
    Fixture f; auto resources = account(); const auto before = resources->current(ResourceKind::Graph);
    auto input = f.xml(); auto decoded = take(decode_price_payload(input, f.contract(), resources, limits())); input.clear();
    require(resources->current(ResourceKind::Graph) > before && resources->current(ResourceKind::Scratch) == 0, "decoded payload lost its charge or retained parser workspace");
    const auto& records = decoded.outcomes();
    require(decoded.scope() == f.contract().scope && records.size() == 2 && records[0].kind == OutcomeKind::Warming && records[0].remaining_preceding == 1,
        "startup or exact gap-aware coverage changed");
    require(records[1].values[0].raw() == std::numeric_limits<std::int64_t>::min() && records[1].parents.size() == 2 &&
        records[1].parents[0].input_port == 0 && records[1].parents[1].input_port == 1 && records[1].parents[0].publication.port == 2 &&
        std::holds_alternative<PointTime>(records[1].parents[0].publication.extent) && std::holds_alternative<Interval>(records[1].extent),
        "raw coefficient, consumer/producer ports or temporal kinds were lost");
    auto encoded = take(encode_price_payload(decoded.outcomes(), f.contract(), resources, limits()));
    require(encoded.bytes().find("input_port=\"1\"") != std::string_view::npos && encoded.bytes().find("clock=\"c&amp;q\"") != std::string_view::npos,
        "encoded fields or escaping missing");
    auto again = take(decode_price_payload(encoded.bytes(), f.contract(), resources, limits()));
    require(again.outcomes()[1].parents == records[1].parents && again.outcomes()[1].values[0].raw() == records[1].values[0].raw(), "payload round trip changed independent expected fields");
    std::weak_ptr<ResourceAccount> weak = resources; resources.reset();
    require(!weak.expired() && decoded.outcomes()[1].parents[0].publication.key.clock == "c&q", "decoded ownership borrowed the parser/account");
}
void strict_parent_and_scalar_fields() {
    Fixture f; auto resources = account(); const auto xml = f.xml();
    for (const auto& malformed : {
        replace(xml, "input_port=\"0\" ", ""), replace(xml, "input_port=\"0\"", "input_port=\"00\""),
        replace(xml, "input_port=\"1\"", "input_port=\"4294967296\""), replace(xml, "input_port=\"0\"", "input_port=\"2\""),
        replace(xml, "port=\"2\"", "port=\"-1\""), replace(xml, "scale=\"9\"", "scale=\"8\""),
        replace(xml, "raw=\"-9223372036854775808\"", "raw=\"9223372036854775808\""),
        replace(xml, "version=\"1\"", "version=\"2\""), replace(xml, "raw=\"-9223372036854775808\"", "raw=\"-0\""),
        replace(xml, "source_reference_digest=\"020", "source_reference_digest=\"02A"),
        replace(xml, "<parents>", "<parents unknown=\"x\">"), replace(xml, "type_version=\"1\"", "type_version=\"1\" type_version=\"1\"")}) {
        rejects(decode_price_payload(malformed, f.contract(), resources, limits()), FailureCode::CacheCorrupt);
    }
}
void coverage_timing_and_grammar() {
    Fixture f; auto resources = account(); const auto xml = f.xml();
    for (const auto& malformed : {
        replace(xml, "<outcome step=\"2\"", "<outcome step=\"1\""),
        replace(xml, "end_step=\"3\"", "end_step=\"4\""),
        replace(xml, "event_time_ns=\"10\"", "event_time_ns=\"10\" interval_begin_ns=\"0\""),
        replace(xml, "interval_end_ns=\"30\"", "interval_end_ns=\"20\""),
        replace(xml, "available_at_ns=\"11\" source_reference", "available_at_ns=\"32\" source_reference"),
        replace(xml, "<price_outcomes", "<!DOCTYPE price_outcomes><price_outcomes"),
        replace(xml, "<parents/>", "<parents>not metadata</parents>"),
        replace(xml, "clock=\"c&amp;q\"", "clock=\"c&bogus;q\""),
        replace(xml, "<parents/>", "<parents><?hidden x?></parents>"),
        replace(xml, "<parents/>", "<parents xmlns=\"hidden\"/>"),
        replace(xml, "kind=\"warming\"", "kind=\"failure\"")}) {
        rejects(decode_price_payload(malformed, f.contract(), resources, limits()), FailureCode::CacheCorrupt);
    }
}
void explicit_bounds_and_equal_updates() {
    Fixture f; auto resources = account();
    auto small = limits(); small.decoded_bytes = 1;
    const auto graph = resources->current(ResourceKind::Graph), scratch = resources->current(ResourceKind::Scratch);
    rejects(decode_price_payload(f.xml(), f.contract(), resources, small), FailureCode::ResourcePlanExceeded);
    require(resources->current(ResourceKind::Graph) == graph && resources->current(ResourceKind::Scratch) == scratch, "failed decode leaked reservations");
    small = limits(); small.scratch_bytes = 1; rejects(decode_price_payload(f.xml(), f.contract(), resources, small), FailureCode::ResourcePlanExceeded);
    small = limits(); small.parents_per_outcome = 1; rejects(decode_price_payload(f.xml(), f.contract(), resources, small), FailureCode::CacheCorrupt);
    const std::vector<UpdateKey> keys{{"clock", 0}, {"clock", 1}};
    const PricePayloadContract contract{{"clock", 0, 2}, f.outputs, keys};
    const std::vector<PriceOutcomeRecord> records{
        {{"clock", 0}, PointTime{0}, 0, OutcomeKind::Published, 0, 0, {Price::from_raw(42)}, {}},
        {{"clock", 1}, PointTime{1}, 1, OutcomeKind::Published, 0, 0, {Price::from_raw(42)}, {}}};
    auto encoded = take(encode_price_payload(records, contract, resources, limits()));
    require(take(measure_price_payload(records, contract, resources, limits())) == encoded.bytes().size(), "payload measurement differs from emitted bytes");
    auto decoded = take(decode_price_payload(encoded.bytes(), contract, resources, limits()));
    require(decoded.outcomes().size() == 2 && decoded.outcomes()[1].key.sequence == 1 && decoded.outcomes()[0].values[0] == decoded.outcomes()[1].values[0], "equal values collapsed distinct updates");
    small = limits(); small.xml_bytes = 8;
    require(take(measure_price_payload(records, contract, resources, small)) == encoded.bytes().size(), "capacity measurement silently truncated an oversized entry");
    rejects(encode_price_payload(records, contract, resources, small), FailureCode::ResourcePlanExceeded);
}
void distinct_history_groups_survive_codec() {
    auto resources = account(); const auto outputs = ports();
    const std::vector<UpdateKey> keys{{"clock", 10}, {"clock", 11}, {"clock", 12}, {"clock", 13}};
    const PricePayloadContract contract{{"clock", 10, 14}, outputs, keys};
    std::vector<PriceOutcomeRecord> records;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        records.push_back({keys[i], PointTime{static_cast<std::int64_t>(i)}, static_cast<std::int64_t>(i),
            i == 3 ? OutcomeKind::Published : OutcomeKind::Warming, i == 3 ? 0U : 3 - i, 0, {}, {}});
    }
    records.back().values.push_back(Price::from_raw(42));
    for (std::size_t i = 0; i < keys.size(); ++i) {
        records.back().parents.push_back({i < 2 ? 0U : 1U, {Sha256{std::byte{1}}, keys[i], 2, PointTime{static_cast<std::int64_t>(i)}, static_cast<std::int64_t>(i), std::nullopt}});
    }
    auto first = take(encode_price_payload(records, contract, resources, limits()));
    records.back().parents[1].input_port = 1;
    auto second = take(encode_price_payload(records, contract, resources, limits()));
    require(first.bytes() != second.bytes(), "wire encoding erased history grouping for identical upstream publications");
    auto a = take(decode_price_payload(first.bytes(), contract, resources, limits()));
    auto b = take(decode_price_payload(second.bytes(), contract, resources, limits()));
    require(a.outcomes().back().parents[1].input_port == 0 && b.outcomes().back().parents[1].input_port == 1 &&
        a.outcomes().back().parents[1].publication == b.outcomes().back().parents[1].publication, "decoded consumer edge changed producer identity or grouping");
}
}
int main() {
    const Case cases[]{{"independent Price payload and owned provenance", independent_payload_and_ownership},
        {"strict parent input and scalar fields", strict_parent_and_scalar_fields},
        {"exact coverage, temporal kinds and XML grammar", coverage_timing_and_grammar},
        {"explicit codec bounds and equal-valued updates", explicit_bounds_and_equal_updates},
        {"shared producer history groups survive strict codec", distinct_history_groups_survive_codec}};
    return lexongine::test::run(cases);
}
