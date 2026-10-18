#include "qaedalus_replay_source.hpp"
#include "quant/storage/parquet_backend.hpp"
#include "integrity_fixture.hpp"
#include "../../tests/lexongine_test_support.hpp"
#if defined(LEXONGINE_EXECUTION_INTEGRATION)
#include "point_replay_test_support.hpp"
#endif

#include <filesystem>
#include <unistd.h>

namespace {
using namespace lexongine;
using namespace lexongine::test;
using namespace lexongine::qaedalus;
namespace q = quant;
std::filesystem::path scratch_root;
template<class T> T arrow_value(arrow::Result<T> result) {
    if (!result.ok()) { throw std::runtime_error(result.status().ToString()); }
    return std::move(result).ValueOrDie();
}
void persisted_checked_replay() {
    auto pattern = (scratch_root / "lexongine-replay-storage.XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end()); writable.push_back('\0');
    if (!::mkdtemp(writable.data())) { throw std::runtime_error("could not create explicit storage-test root"); }
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); } } cleanup{writable.data()};
    auto reader = std::make_shared<q::storage::ParquetStorageBackend>(cleanup.path, q::ingestion::decode_xnas_record);
    const auto prepared = q::fixture::prepared("lexongine-replay-acquisition");
    const auto published = arrow_value(reader->publish(prepared, "lexongine-replay-version"));
    const auto trade = std::find_if(published.fragments.begin(), published.fragments.end(), [](const auto& fragment) { return fragment.data_kind == q::storage::DataKind::Trades; });
    require(trade != published.fragments.end(), "published Trade partition missing");
    ReplaySourceBinding binding{prepared.acquisition.query, {*trade}, {true, true, false, false, false}, ReplayDeliveryMode::OrderedEvent,
        q::storage::DataKind::Trades, "persisted-trade-test", {"persisted-clock", 0, 1}, {"qaedalus.ordinal_availability_table", 1, {1, {}}},
        {{2, 200}}, MissingIntervalPolicy::Strict, {16, 65536, 65536, 65536}, {"qaedalus", "checked_trade_replay", 1, "integration-test"},
        {Sha256{std::byte{1}}, Sha256{std::byte{2}}, Sha256{std::byte{3}}, "integration-test-build"}, {}};
    auto account = take(ResourceAccount::create({{1048576, 1048576, 1048576, 1048576, 1048576, 1048576, 1048576}, 128, 128, 2048, 128}));
    auto snapshot = take(prepare_trade_replay(reader, binding, account));
    require(snapshot.metadata().version == "lexongine-replay-version" && snapshot.statistics().selected == 1, "exact persisted version lost");
    auto cursor = take(snapshot.cursor(account));
    auto timing = *cursor.descriptor();
    auto delivered = take(cursor.next({timing.key, timing.extent, timing.available_at_ns, timing.available_at_ns}));
    const auto& record = std::get<TradeReplayRecord>(delivered);
    require(record.acquisition_ordinal == 2 && record.stream_id == q::data::StreamId{1} &&
        q::integrity::same_observation(q::integrity::Observation{record.value}, prepared.rows[2].event), "checked stored event association changed");
    require(snapshot.binding().selected_fragments[0] == *trade, "original pinned fragment was replaced");
    auto registry = take(Registry::create());
    auto resolver = take(SourceResolver::create(account, 65536, 4));
    success(register_trade_replay_source(resolver, "persisted", take(register_trade_type(registry, {})), reader, binding, account));
    success(resolver.freeze());
    SourceValidationServices services{account, {{16, 16, 0, 16}, 1024, 16384, 32768, 16384}, MissingIntervalPolicy::Strict,
        {1048576, 1048576, 1048576, 1048576, 1048576, 1048576, 1048576}};
    SourceRequest request{"persisted", binding.snapshot_name, "lexongine-replay-version", trade_interpretation(), binding.expected_scope, {1, {}}};
    auto candidate = take(resolver.inspect(request, services));
    auto wrapped = take(resolver.validate(std::move(candidate), request, services));
    require(wrapped.metadata().content_digest == snapshot.metadata().content_digest, "source wrapper changed persisted binding identity");
    const auto expected_price = record.value.price;
#if defined(LEXONGINE_EXECUTION_INTEGRATION)
    auto pipeline_binding=binding;
    pipeline_binding.expected_scope={"c",0,1};
    pipeline_binding.adapter_operation={"test","trades",1,"builtin-v1"};
    lexongine::integration_test::PointEnvironment pipeline(reader,pipeline_binding);
    auto cache=lexongine::integration_test::cache();
    auto memory=MemoryStore::make(cache,pipeline.types.price);
    auto plan=take(pipeline.prepare(1,Driver::Hosted,&cache,memory.store()));
    require(plan.plan&&plan.rejected.empty(),"persisted checked Trade did not admit the numerical pipeline");
#endif
    // Corrupt only this disposable test package after capture. The immutable
    // source data must remain readable; new preparation must fail its reader gate.
    std::filesystem::remove(std::filesystem::path{published.package_locator} / ".attempt-attempt-1" / "references.qref");
    auto again = take(snapshot.cursor(account));
    auto retained = take(again.next({timing.key, timing.extent, timing.available_at_ns, timing.available_at_ns}));
    require(std::get<TradeReplayRecord>(retained).value.price == expected_price, "delivery reopened a mutable locator");
    rejects(prepare_trade_replay(reader, binding, account), FailureCode::SourceInvalid);
#if defined(LEXONGINE_EXECUTION_INTEGRATION)
    auto [report,values]=lexongine::integration_test::execute_point(plan.plan,pipeline.types.price);
    require(report.acceptance==AcceptanceStatus::Accepted&&report.complete_updates==1&&values->values.size()==1&&values->values[0].raw==expected_price.raw()&&values->values[0].available==200,
        "pinned persisted Trade numerical pipeline changed data/availability after provider corruption");
    const auto lookups=memory.lookups();
    auto rejected=take(pipeline.prepare(1,Driver::Hosted,&cache,memory.store()));
    require(!rejected.plan&&memory.lookups()==lookups,"cached numerical artifacts bypassed missing persisted reference evidence");
#endif
    auto run = take(ResourceAccount::create({{1048576, 1048576, 1048576, 1048576, 1048576, 1048576, 1048576}, 128, 128, 2048, 128}));
    PublicationAllocator allocator{run, 0}; auto core_cursor = take(wrapped.cursor(allocator));
    UpdateContext context{timing.key, timing.extent, timing.available_at_ns, timing.available_at_ns};
    auto event = take(core_cursor.next(context));
    auto slot = take(ResultSlot::create(run, {wrapped.output_contract()}, 0, wrapped.metadata().content_digest, wrapped.metadata().source_reference_digest));
    slot->commit(take(validate_pending(std::get<PendingOutcome>(std::move(event)), *slot, context)));
    require(take(slot->read<q::data::Trade>(0, context)).value.price == expected_price, "core publication reopened corrupted source");
    static_cast<void>(take(core_cursor.finish()));
    candidate = take(resolver.inspect(request, services));
    rejects(resolver.validate(std::move(candidate), request, services), FailureCode::SourceInvalid);
    static_cast<void>(take(cursor.finish()));
}
}
int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "explicit scratch root required\n"; return 1; }
    scratch_root = argv[1];
    const Case cases[]{{"persisted acquisition through checked replay adapter", persisted_checked_replay}};
    return lexongine::test::run(cases);
}
