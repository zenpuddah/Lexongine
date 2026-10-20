#include "file_result_store.hpp"
#include "file_evidence.hpp"
#include "fixture_source.hpp"
#include "xml_frontend.hpp"
#include "native_files.hpp"
#include "lexongine/build_contract.hpp"
#include "../../tests/lexongine_test_support.hpp"
#include <fstream>
#include <sstream>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
namespace {
using namespace lexongine;
using namespace lexongine::test;
namespace ql = quant::lexongine;
std::string example, executable, example_path;
std::filesystem::path scratch;
std::string replace(std::string text, std::string_view a, std::string_view b) {
    auto p = text.find(a);
    require(p != std::string::npos, "fixture substitution missing");
    text.replace(p, a.size(), b);
    return text;
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (scratch / "lexongine-persistence.XXXXXX").string();
        std::vector<char> data(pattern.begin(), pattern.end());
        data.push_back('\0');
        require(::mkdtemp(data.data()) != nullptr, "test directory creation failed");
        path = data.data();
    }
    ~Directory() {
        std::error_code e;
        std::filesystem::remove_all(path, e);
    }
};
std::shared_ptr<ResourceAccount> resources() {
    return take(ResourceAccount::create(
        {{16777216, 4194304, 134217728, 16777216, 4194304, 67108864, 8388608},
         4096,
         1024,
         16384,
         128}));
}
MetadataLimits metadata_limits() {
    return {{1048576, 65536, 64, 4096, 8388608}, 4194304};
}
FileStoreLimits store_limits() {
    return {{{1048576, 1024, 32768, 4194304}, 32768, 4096, 4096, 4096, 4096},
            metadata_limits(),
            {1048576, 65536, 1024, 4096, 4096, 1048576, 16777216},
            1048576,
            512,
            1048576};
}
HostBuild host() {
    return take(inspect_host_build(build_contract_id, LEXONGINE_HOST_SOURCE_BUILD, resources(),
                                   {67108864, 1048576, 512}));
}
std::string configuration(const std::filesystem::path &directory, std::string_view id) {
    auto text =
        replace(example, "managed_buffer_bytes=\"8388608\"", "managed_buffer_bytes=\"134217728\"");
    text = replace(std::move(text), "fixture-run-v1", id);
    text =
        replace(std::move(text), "/tmp/lexongine-fixture-cache-v1", (directory / "store").string());
    text = replace(std::move(text), "/tmp/lexongine-fixture-retention.log",
                   (directory / (std::string{id} + ".log")).string());
    for (const auto &[a, b] : std::array<std::pair<std::string, std::string>, 4>{
             {{"name=\"window\" value=\"20\"", "name=\"window\" value=\"2\""},
              {"name=\"window\" value=\"50\"", "name=\"window\" value=\"3\""},
              {"begin_step=\"49\"", "begin_step=\"2\""},
              {"end_step=\"120\"", "end_step=\"3\""}}}) {
        for (auto p = text.find(a); p != std::string::npos; p = text.find(a)) {
            text.replace(p, a.size(), b);
        }
    }
    return text;
}
struct Environment {
    std::shared_ptr<ResourceAccount> preparation = resources(), store_account = resources(),
                                     evidence_account = resources();
    HostBuild build = host();
    Registry registry = take(Registry::create());
    ql::ComponentRegistration registration{build.identity(), {}, MissingIntervalPolicy::Strict};
    ql::ComponentTypes types = take(ql::register_components(registry, registration));
    SourceResolver source = take(SourceResolver::create(preparation, 65536, 4));
    Environment() {
        success(fixture::register_source(registry, source, types.bar, registration,
                                         {120, 65536, 131072, 65536}));
        success(registry.freeze());
        success(source.freeze());
    }
    LocalCacheConfiguration cache(std::string_view xml) {
        auto parsed = take(parse_xml(xml, {1048576, 32, 16384, 1024, preparation}));
        return std::get<LocalCacheConfiguration>(parsed.declaration().cache);
    }
    Result<PrepareResult> prepare(std::string_view xml, std::shared_ptr<ResultStore> store,
                                  const std::filesystem::path &snapshot) {
        auto composition = take(resolve_composition(
            take(parse_xml(xml, {1048576, 32, 16384, 1024, preparation})), registry));
        RequiredEvidenceSpec spec{"snapshot",        "run_snapshot_v1",
                                  snapshot.string(), "create_new",
                                  1048576,           128,
                                  "on_close",        "stop"};
        PrepareOptions options{
            composition.declaration().run.id,
            take(fixture::source_request(composition.declaration().sources[0])),
            {preparation,
             {{120, 120, 0, 120}, 1024, 32768, 131072, 32768},
             MissingIntervalPolicy::Strict,
             {1048576, 1048576, 1048576, 1048576, 1048576, 1048576, 1048576}},
            {{0, 262144, 131072, 524288, 131072, 524288, 2097152}, 1024, 1024, 8192, 128},
            {1048576, 1048576, 1048576, 1048576, 1048576, 16384, 1048576},
            131072,
            1048576,
            static_cast<Result<Sha256> (*)(std::span<const std::byte>) noexcept>(files::digest),
            {},
            {spec},
            evidence_account,
            {0, 0, 16777216, 1048576, 0, 1048576, 65536}};
        return lexongine::prepare(composition, registry, source, std::move(store), options);
    }
    RunReport run(std::shared_ptr<const PreparedPlan> plan) {
        std::vector<ObservationAdapter> observers;
        auto values = std::make_shared<std::vector<std::int64_t>>();
        for (const auto &request : plan->observations()) {
            observers.push_back(make_observer<quant::data::Price>(
                request.request.id, types.price,
                [values](const auto &value, const RequestContext &) -> Result<void> {
                    values->push_back(value.value.raw());
                    return ok();
                }));
        }
        std::vector<std::string> names;
        for (const auto &n : plan->nodes()) {
            names.push_back(n.spec.id);
        }
        auto retention =
            take(FileEvidence::create(plan->evidence_specification(), plan->configuration().run.id,
                                      plan->configuration().run.clock, std::move(names),
                                      evidence_account, {65536, 8192, 8388608}));
        auto snapshot =
            take(SnapshotEvidence::create(plan, plan->required_evidence()[0], evidence_account,
                                          {metadata_limits(), 1048576, 4096}));
        RunServices services{
            std::move(observers),
            retention.callbacks(),
            {IntegrationOutcome::Completed, {}, "test.filesystem", 1, Sha256{std::byte{1}}},
            {snapshot.callbacks()}};
        auto run = take(instantiate(plan, std::move(services)));
        auto report = take(execute(run));
        if (report.acceptance == AcceptanceStatus::Accepted) {
            require(values->size() == 4 && values->back() == 5000000,
                    "persistent fixture numerical result differs");
        }
        return report;
    }
};
RunReport run_file(const std::filesystem::path &directory, std::string_view id,
                   std::function<Result<void>(StoreFaultPoint)> fault = {}) {
    Environment e;
    auto xml = configuration(directory, id);
    auto store = take(FileResultStore::create(e.cache(xml), e.types.price, e.build, e.store_account,
                                              store_limits(), std::move(fault)));
    auto plan =
        take(e.prepare(xml, store.service(), directory / (std::string{id} + ".snapshot.xml")));
    require(plan.plan && plan.rejected.empty(), "persistent fixture preparation rejected");
    return e.run(plan.plan);
}
void persisted_restart_and_inventory() {
    Directory d;
    auto first = run_file(d.path, "first");
    if (first.cause) {
        throw std::runtime_error(std::string{first.cause->text()});
    }
    require(first.acceptance == AcceptanceStatus::Accepted &&
                first.cache == CachePublicationStatus::Published &&
                first.cache_entries.size() == 4 && first.evidence.size() == 2,
            "cold persisted run did not close its inventory");
    auto second = run_file(d.path, "second");
    if (second.cause) {
        throw std::runtime_error(std::string{second.cause->text()});
    }
    require(second.acceptance == AcceptanceStatus::Accepted &&
                second.cache == CachePublicationStatus::NoNewEntries &&
                second.producers[2].advances == 0 && second.producers[2].cached_deliveries == 3,
            "reopened store did not replay exact scope");
    auto bytes = take(files::read(d.path / "first.snapshot.xml", resources(), 1048576));
    auto snapshot = take(decode_snapshot(bytes.value, resources(), metadata_limits()));
    require(snapshot.value.acceptance == "pending" && snapshot.value.inventory.size() == 1 &&
                snapshot.value.entries == first.cache_entries,
            "persisted snapshot claims acceptance or contains a self hash");
}
void lock_and_corrupt_payload() {
    Directory d;
    auto first = run_file(d.path, "first");
    require(first.acceptance == AcceptanceStatus::Accepted, "could not seed corruption test");
    Environment e;
    auto xml = configuration(d.path, "later");
    auto config = e.cache(xml);
    auto one = take(
        FileResultStore::create(config, e.types.price, e.build, e.store_account, store_limits()));
    rejects(FileResultStore::create(config, e.types.price, e.build, resources(), store_limits()),
            FailureCode::CacheBusy);
    const auto payload = d.path / "store" / (first.cache_entries[0].artifact_id + ".price.xml");
    {
        std::ofstream out(payload, std::ios::binary | std::ios::app);
        out.put('x');
        require(static_cast<bool>(out), "corruption injection failed");
    }
    rejects(e.prepare(xml, one.service(), d.path / "later.snapshot.xml"),
            FailureCode::CacheCorrupt);
}
void publication_failures_and_recovery() {
    for (auto point : {StoreFaultPoint::PayloadWrite, StoreFaultPoint::ManifestWrite,
                       StoreFaultPoint::MarkerWrite, StoreFaultPoint::BeforePublish}) {
        Directory d;
        auto report = run_file(d.path, "failed", [point](auto actual) -> Result<void> {
            return actual == point ? Result<void>{diagnostic(FailureCode::CacheIO,
                                                             "injected filesystem failure")}
                                   : ok();
        });
        require(report.acceptance == AcceptanceStatus::Rejected &&
                    report.cache == CachePublicationStatus::NotPublished,
                "failed filesystem publication was accepted");
        for (const auto &entry : std::filesystem::directory_iterator(d.path / "store")) {
            require(!entry.path().filename().string().ends_with(".accepted.xml"),
                    "failed run left an authoritative marker");
        }
        auto retry = run_file(d.path, "retry");
        require(retry.acceptance == AcceptanceStatus::Accepted && retry.producers[2].advances == 3,
                "provisional files became a cache hit");
    }
    Directory d;
    auto report = run_file(d.path, "uncertain", [](auto point) -> Result<void> {
        return (point == StoreFaultPoint::AfterPublish || point == StoreFaultPoint::Reconcile)
                   ? Result<void>{diagnostic(FailureCode::CacheIO,
                                             "injected acknowledgement/reconciliation loss")}
                   : ok();
    });
    require(report.acceptance == AcceptanceStatus::Unconfirmed && !report.cache_commit_id.empty(),
            "ambiguous native publication was reported as accepted/rejected");
    auto recovered = run_file(d.path, "recovered");
    require(recovered.acceptance == AcceptanceStatus::Accepted &&
                recovered.producers[2].advances == 0,
            "restart could not recover the authoritative marker");
}
void cross_process_reuse() {
    Directory d;
    auto first = run_file(d.path, "parent");
    require(first.acceptance == AcceptanceStatus::Accepted, "parent run failed");
    auto child = ::fork();
    require(child >= 0, "fork failed");
    if (child == 0) {
        ::execl(executable.c_str(), executable.c_str(), "--child", example_path.c_str(),
                d.path.c_str(), static_cast<char *>(nullptr));
        ::_exit(127);
    }
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "new process could not validate and reuse persisted outcomes");
}
void file_count_capacity_is_prewrite() {
    Directory d;
    Environment e;
    auto xml = configuration(d.path, "limited");
    auto limits = store_limits();
    limits.max_files = 8;
    auto store = take(
        FileResultStore::create(e.cache(xml), e.types.price, e.build, e.store_account, limits));
    auto prepared = take(e.prepare(xml, store.service(), d.path / "limited.snapshot.xml"));
    auto report = e.run(prepared.plan);
    require(report.acceptance == AcceptanceStatus::Accepted && report.cache_entries.size() == 3 &&
                report.cache_bypassed_entries == 1,
            "file-count capacity must bypass before writing a fourth entry plus marker");
    std::size_t count = 0;
    for (const auto &entry : std::filesystem::directory_iterator(d.path / "store")) {
        if (entry.path().filename() != ".lexongine.lock") {
            ++count;
        }
    }
    require(count == 7, "file-count capacity left hidden provisional writes");
}
void strict_snapshot_and_manifest_metadata() {
    Directory d;
    auto report = run_file(d.path, "seed");
    require(report.acceptance == AcceptanceStatus::Accepted, "metadata test seed failed");
    const auto raw = take(files::read(d.path / "seed.snapshot.xml", resources(), 1048576));
    const auto wrong_event = replace(raw.value, "<field name=\"kind\"><text>outcome</text>",
                                     "<field name=\"kind\"><text>unknown</text>");
    rejects(decode_snapshot(wrong_event, resources(), metadata_limits()),
            FailureCode::CacheCorrupt);
    const auto at = raw.value.find("<record type=\"RunResourcesV1\">");
    require(at != std::string::npos, "snapshot resources missing");
    const auto wrong_policy =
        raw.value.substr(0, at) +
        replace(raw.value.substr(at), "<text>retention</text>", "<text>renamed</text>");
    rejects(decode_snapshot(wrong_policy, resources(), metadata_limits()),
            FailureCode::CacheCorrupt);
    const auto manifest_path =
        d.path / "store" / (report.cache_entries[0].artifact_id + ".manifest.xml");
    const auto manifest_bytes = take(files::read(manifest_path, resources(), 1048576));
    auto manifest = take(decode_manifest(manifest_bytes.value, resources(), metadata_limits()));
    // A valid SHA-256 is insufficient when the bytes do not frame ArtifactIdentityV1.
    manifest.value.canonical_identity[29] = std::byte{9};
    manifest.value.identity_digest = take(files::digest(manifest.value.canonical_identity));
    rejects(encode_manifest(manifest.value, resources(), metadata_limits()),
            FailureCode::CacheCorrupt);
}
void immutable_files_and_pinned_plan() {
    Directory d;
    auto account = resources();
    take(files::write_new("existing", d.path / "existing", "old", account, 64,
                          FailureCode::EvidenceIO));
    rejects(files::write_new("existing", d.path / "existing", "new", account, 64,
                             FailureCode::EvidenceIO),
            FailureCode::EvidenceIO);
    take(files::write_new("pending", d.path / "pending", "new", account, 64,
                          FailureCode::EvidenceIO));
    rejects(files::publish(d.path / "pending", d.path / "existing"),
            FailureCode::CommitUnconfirmed);
    require(take(files::read(d.path / "existing", account, 64)).value == "old",
            "no-replace publication overwrote existing bytes");
    require(::mkfifo((d.path / "fifo").c_str(), 0600) == 0, "could not create FIFO boundary test");
    rejects(files::read(d.path / "fifo", account, 64), FailureCode::CacheCorrupt);
    auto first = run_file(d.path, "first");
    Environment e;
    auto xml = configuration(d.path, "pinned");
    auto config = e.cache(xml);
    std::shared_ptr<const PreparedPlan> plan;
    {
        auto store = take(FileResultStore::create(config, e.types.price, e.build, e.store_account,
                                                  store_limits()));
        plan = take(e.prepare(xml, store.service(), d.path / "pinned.snapshot.xml")).plan;
    }
    rejects(FileResultStore::create(config, e.types.price, e.build, resources(), store_limits()),
            FailureCode::CacheBusy);
    {
        std::ofstream out(d.path / "store" / (first.cache_entries[0].artifact_id + ".price.xml"),
                          std::ios::app);
        out.put('x');
        require(static_cast<bool>(out), "could not mutate pinned file");
    }
    auto report = e.run(plan);
    require(report.acceptance == AcceptanceStatus::Accepted && report.producers[2].advances == 0,
            "validated plan did not retain immutable decoded bytes");
    rejects(e.prepare(configuration(d.path, "new"), plan->store(), d.path / "new.snapshot.xml"),
            FailureCode::CacheCorrupt);
}
void corrupt_origin_evidence_rejects() {
    for (unsigned kind = 0; kind < 4; ++kind) {
        Directory d;
        auto first = run_file(d.path, "origin");
        std::filesystem::path path;
        if (kind == 0) {
            path = d.path / "origin.log";
        } else if (kind == 1) {
            path = d.path / "origin.snapshot.xml";
        } else if (kind == 2) {
            path = d.path / "store" / (first.cache_entries[0].artifact_id + ".manifest.xml");
        } else {
            path = d.path / "store" / (first.cache_commit_id + ".accepted.xml");
        }
        {
            std::ofstream out(path, std::ios::app);
            out.put('x');
            require(static_cast<bool>(out), "origin corruption injection failed");
        }
        Environment e;
        auto xml = configuration(d.path, "later");
        auto store = take(FileResultStore::create(e.cache(xml), e.types.price, e.build,
                                                  e.store_account, store_limits()));
        rejects(e.prepare(xml, store.service(), d.path / "later.snapshot.xml"),
                FailureCode::CacheCorrupt);
    }
}
void bypass_cleanup_and_snapshot_failure() {
    Directory d;
    {
        Environment e;
        auto xml = replace(configuration(d.path, "bypass"), "max_entry_bytes=\"1048576\"",
                           "max_entry_bytes=\"64\"");
        auto store = take(FileResultStore::create(e.cache(xml), e.types.price, e.build,
                                                  e.store_account, store_limits()));
        auto report =
            e.run(take(e.prepare(xml, store.service(), d.path / "bypass.snapshot.xml")).plan);
        require(report.acceptance == AcceptanceStatus::Accepted &&
                    report.cache == CachePublicationStatus::Bypassed &&
                    report.cache_entries.empty(),
                "byte capacity did not bypass cleanly");
        require(std::distance(std::filesystem::directory_iterator(d.path / "store"),
                              std::filesystem::directory_iterator{}) == 1,
                "capacity bypass wrote provisional data");
    }
    auto failed = run_file(d.path, "failed", [](auto point) -> Result<void> {
        return point == StoreFaultPoint::ManifestWrite
                   ? Result<void>{diagnostic(FailureCode::CacheIO, "manifest failure")}
                   : ok();
    });
    require(failed.acceptance == AcceptanceStatus::Rejected, "failed manifest accepted");
    {
        Environment e;
        auto xml = configuration(d.path, "cleanup");
        auto store = take(FileResultStore::create(e.cache(xml), e.types.price, e.build,
                                                  e.store_account, store_limits()));
        success(store.cleanup_unaccepted());
        require(std::distance(std::filesystem::directory_iterator(d.path / "store"),
                              std::filesystem::directory_iterator{}) == 1,
                "explicit cleanup did not remove provisional files");
    }
    take(files::write_new("preexisting", d.path / "blocked.snapshot.xml", "preserve", resources(),
                          64, FailureCode::EvidenceIO));
    auto blocked = run_file(d.path, "blocked");
    require(blocked.acceptance == AcceptanceStatus::Rejected && blocked.cause &&
                blocked.cause->code == FailureCode::EvidenceIO,
            "required snapshot open failure accepted");
    require(take(files::read(d.path / "blocked.snapshot.xml", resources(), 64)).value == "preserve",
            "failed evidence open changed existing file");
    auto accepted = run_file(d.path, "accepted");
    require(accepted.acceptance == AcceptanceStatus::Accepted, "cleanup retry failed");
    {
        Environment e;
        auto xml = configuration(d.path, "retained");
        auto store = take(FileResultStore::create(e.cache(xml), e.types.price, e.build,
                                                  e.store_account, store_limits()));
        success(store.cleanup_unaccepted());
        require(std::filesystem::exists(d.path / "store" /
                                        (accepted.cache_commit_id + ".accepted.xml")),
                "cleanup removed accepted marker");
        auto report =
            e.run(take(e.prepare(xml, store.service(), d.path / "retained.snapshot.xml")).plan);
        require(report.acceptance == AcceptanceStatus::Accepted &&
                    report.producers[2].advances == 0,
                "cleanup damaged accepted entries");
    }
}
void abrupt_process_exit_and_restart() {
    for (unsigned point = 0; point < 5; ++point) {
        Directory d;
        const auto token = std::to_string(point);
        const auto child = ::fork();
        require(child >= 0, "crash-probe fork failed");
        if (child == 0) {
            ::execl(executable.c_str(), executable.c_str(), "--crash", example_path.c_str(),
                    d.path.c_str(), token.c_str(), static_cast<char *>(nullptr));
            ::_exit(127);
        }
        int status = 0;
        require(::waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                    WEXITSTATUS(status) == 86,
                "child did not exit at the selected publication boundary");
        std::size_t markers = 0;
        for (const auto &file : std::filesystem::directory_iterator(d.path / "store")) {
            markers += file.path().filename().string().ends_with(".accepted.xml");
        }
        require(markers == (point == 4 ? 1U : 0U),
                "abrupt exit changed authoritative marker visibility");
        auto recovered = run_file(d.path, "restart");
        require(recovered.acceptance == AcceptanceStatus::Accepted &&
                    recovered.producers[2].advances == (point == 4 ? 0U : 3U),
                "restart consumed provisional files or lost the validated accepted origin");
    }
}
} // namespace
int main(int argc, char **argv) {
    executable = argv[0];
    bool child = argc == 4 && std::string_view{argv[1]} == "--child";
    bool crash = argc == 5 && std::string_view{argv[1]} == "--crash";
    if (!child && !crash && argc != 3) {
        std::cerr << "explicit example and scratch paths required\n";
        return 1;
    }
    example_path = argv[child || crash ? 2 : 1];
    std::ifstream file(example_path);
    std::ostringstream text;
    text << file.rdbuf();
    example = text.str();
    scratch = argv[child || crash ? 3 : 2];
    if (crash) {
        try {
            const auto index = take(parse_integer<unsigned>(argv[4]));
            const std::array points{StoreFaultPoint::PayloadWrite, StoreFaultPoint::ManifestWrite,
                                    StoreFaultPoint::MarkerWrite, StoreFaultPoint::BeforePublish,
                                    StoreFaultPoint::AfterPublish};
            require(index < points.size(), "unknown crash boundary");
            static_cast<void>(
                run_file(scratch, "crash", [point = points[index]](auto actual) -> Result<void> {
                    if (actual == point) {
                        ::_exit(86);
                    }
                    return ok();
                }));
            return 1;
        } catch (const std::exception &e) {
            std::cerr << e.what() << '\n';
            return 1;
        }
    }
    if (child) {
        try {
            auto report = run_file(scratch, "child");
            require(report.acceptance == AcceptanceStatus::Accepted &&
                        report.producers[2].advances == 0 &&
                        report.producers[2].cached_deliveries == 3,
                    "child cache mismatch");
            return 0;
        } catch (const std::exception &e) {
            std::cerr << e.what() << '\n';
            return 1;
        }
    }
    const Case cases[]{
        {"native restart reuse and complete evidence inventory", persisted_restart_and_inventory},
        {"exclusive store lock and corrupt payload rejection", lock_and_corrupt_payload},
        {"publication failures and unconfirmed restart recovery",
         publication_failures_and_recovery},
        {"cross-process exact-scope reuse", cross_process_reuse},
        {"file-count capacity is selected before writing", file_count_capacity_is_prewrite},
        {"strict snapshot semantics and canonical manifest framing",
         strict_snapshot_and_manifest_metadata},
        {"immutable files, bounded nonregular reads and pinned plans",
         immutable_files_and_pinned_plan},
        {"corrupt originating evidence blocks preparation", corrupt_origin_evidence_rejects},
        {"capacity bypass, explicit cleanup and required snapshot failure",
         bypass_cleanup_and_snapshot_failure},
        {"abrupt process exit at publication boundaries and restart",
         abrupt_process_exit_and_restart}};
    return lexongine::test::run(cases);
}
