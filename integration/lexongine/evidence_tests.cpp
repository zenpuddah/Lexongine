#include "file_evidence.hpp"
#include "quant/storage/file_integrity.hpp"
#include "../../tests/lexongine_test_support.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace {
using namespace lexongine;
using namespace lexongine::test;
std::filesystem::path root;
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (root / "lexongine-evidence.XXXXXX").string(); std::vector<char> value(pattern.begin(), pattern.end()); value.push_back('\0');
        if (!::mkdtemp(value.data())) { throw std::runtime_error("could not create explicit evidence test directory"); } path = value.data();
    }
    ~Directory() { std::error_code error; std::filesystem::remove_all(path, error); }
};
std::shared_ptr<ResourceAccount> account() { return take(ResourceAccount::create({{1048576, 1048576, 8388608, 1048576, 1048576, 1048576, 1048576}, 128, 128, 1024, 128})); }
RetentionLogConfiguration spec(const std::filesystem::path& path) { return {path.string(), "key_value_lines_v1", "create_new", "on_event", "stop", "stop", 1048576, 4096}; }
std::string read(const std::filesystem::path& path) { std::ifstream input{path}; std::ostringstream text; text << input.rdbuf(); return text.str(); }
void file_is_visible_and_exclusive() {
    Directory directory; auto config = spec(directory.path / "retention.log");
    auto file = take(FileEvidence::create(config, "run\"one", "clock", {"source", "mean"}, account(), {65536, 8192, 8388608}));
    auto sink = file.callbacks(); success(sink.open());
    RetentionEvent event{RetentionEventKind::Allocated, 7, 8, 0}; event.step = 49; event.owner = AllocationOwner{1, 49};
    success(sink.write(event));
    const auto live = read(config.path);
    require(live.find("run=\"run\\\"one\"") != std::string::npos && live.find("producer=\"mean\"") != std::string::npos &&
        live.find("step=49") != std::string::npos && live.find("publication=\"clock:49\"") != std::string::npos,
        "live retention output lost escaped run, producer or activation identity");
    RunReport report; report.numerical = NumericalOutcome::Completed;
    success(sink.finalize(report)); const auto reference = take(file.reference());
    require(reference.finalized && reference.byte_count == live.size() && reference.absolute_path == config.path && reference.digest != Sha256{}, "closed evidence reference is incomplete");
    auto other = take(FileEvidence::create(config, "other", "clock", {"source"}, account(), {65536, 8192, 8388608}));
    rejects(other.callbacks().open(), FailureCode::EvidenceIO); require(read(config.path) == live, "create_new replaced existing evidence");
    rejects(sink.write(event), FailureCode::EvidenceIO);
    require(take(file.reference()).digest == reference.digest, "API misuse retroactively invalidated finalized evidence");
}
void quota_precedes_write() {
    Directory directory; auto config = spec(directory.path / "small.log"); config.max_bytes = 8;
    auto file = take(FileEvidence::create(config, "run", "clock", {"source"}, account(), {65536, 8192, 8388608}));
    auto sink = file.callbacks(); success(sink.open());
    RetentionEvent event{RetentionEventKind::Allocated, 1, 8, 0};
    rejects(sink.write(event), FailureCode::EvidenceQuota); require(read(config.path).empty(), "over-limit record was partially written");
    RunReport report; rejects(sink.finalize(report), FailureCode::EvidenceQuota); rejects(file.reference(), FailureCode::EvidenceIO);
}
void allocation_ownership_survives_retention_copy() {
    auto resources = account(); success(resources->advance_age(19)); resources->set_owner(AllocationOwner{2, 19});
    auto value = take(make_owned<std::int64_t>(resources, ResourceKind::Retention, sizeof(std::int64_t), {}, [] { return std::int64_t{42}; }));
    resources->set_owner(std::nullopt); success(resources->advance_age(20));
    auto shared = take(value.share("external", "saved value"));
    const auto& event = resources->events().back();
    require(event.owner && event.owner->producer == 2 && event.owner->publication == 19 && event.step == 20,
        "holder event used current allocation context instead of original payload owner");
}
void scalar_copy_inherits_original_owner() {
    auto resources = account(); success(resources->advance_age(19)); resources->set_owner(AllocationOwner{2, 19});
    TypeContract type{"test.scalar", 1, {1, {}}};
    auto slot = take(ResultSlot::create(resources, {{type, typeid(std::int64_t), StorageKind::ScalarCopy, {}}}, 0, {}, {}));
    UpdateContext context{{"clock", 19}, PointTime{19}, 19, 19}; auto pending = take(PendingOutcome::create(resources, context, 1));
    auto original = take(make_owned<std::int64_t>(resources, ResourceKind::Retention, 8, {}, [] { return std::int64_t{42}; }));
    success(pending.set_output(0, take(ErasedValue::from(std::move(original), type)))); slot->commit(take(validate_pending(std::move(pending), *slot, context)));
    success(resources->advance_age(20)); resources->set_owner(AllocationOwner{5, 20});
    auto copy = take(take(slot->read<std::int64_t>(0, context)).retain("external", "copy"));
    const auto retained = std::find_if(resources->events().rbegin(), resources->events().rend(), [](const auto& event) { return event.kind == RetentionEventKind::Allocated && event.resource_kind == ResourceKind::Retention; });
    require(copy.get() == 42 && retained != resources->events().rend() && retained->owner && retained->owner->producer == 2 && retained->owner->publication == 19 &&
        resources->owner_context()->producer == 5, "retained scalar copy changed original owner or leaked an owner override");
}
}
int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "explicit scratch root required\n"; return 1; } root = argv[1];
    const Case cases[]{{"live exclusive file evidence and closed reference", file_is_visible_and_exclusive},
        {"evidence quota before write", quota_precedes_write}, {"allocation owner survives external hold", allocation_ownership_survives_retention_copy},
        {"scalar copy retains original allocation owner", scalar_copy_inherits_original_owner}};
    return lexongine::test::run(cases);
}
