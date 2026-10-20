#include "cli.hpp"
#include "xml_report.hpp"
#include "native_files.hpp"
#include "../../tests/lexongine_test_support.hpp"
#include <fstream>
#include <sstream>
#include <unistd.h>
namespace {
using namespace lexongine;
using namespace lexongine::test;
std::string example;
std::filesystem::path scratch;
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (scratch / "inspection.XXXXXX").string();
        std::vector<char> data(pattern.begin(), pattern.end());
        data.push_back('\0');
        require(::mkdtemp(data.data()) != nullptr, "scratch creation failed");
        path = data.data();
    }
    ~Directory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};
std::string replace(std::string text, std::string_view from, std::string_view to) {
    auto p = text.find(from);
    require(p != std::string::npos, "test substitution missing");
    text.replace(p, from.size(), to);
    return text;
}
std::shared_ptr<ResourceAccount> resources() {
    return take(ResourceAccount::create(
        {{1048576, 1048576, 536870912, 67108864, 1048576, 67108864, 8388608},
         4096,
         2048,
         4096,
         128}));
}
std::filesystem::path config(const Directory &d) {
    auto text =
        replace(example, "managed_buffer_bytes=\"8388608\"", "managed_buffer_bytes=\"536870912\"");
    text = replace(std::move(text), "/tmp/lexongine-fixture-cache-v1", (d.path / "cache").string());
    text = replace(std::move(text), "/tmp/lexongine-fixture-retention.log",
                   (d.path / "original.log").string());
    for (const auto &[a, b] : std::array<std::pair<std::string, std::string>, 4>{
             {{"name=\"window\" value=\"20\"", "name=\"window\" value=\"2\""},
              {"name=\"window\" value=\"50\"", "name=\"window\" value=\"3\""},
              {"begin_step=\"49\"", "begin_step=\"2\""},
              {"end_step=\"120\"", "end_step=\"3\""}}}) {
        while (text.find(a) != std::string::npos) {
            text = replace(std::move(text), a, b);
        }
    }
    const auto path = d.path / "config.xml";
    take(files::write_new("config", path, text, resources(), 1048576, FailureCode::EvidenceIO));
    return path;
}
int invoke(const std::vector<std::string> &arguments, std::ostream &out, std::ostream &err,
           std::function<Result<void>(StoreFaultPoint)> fault = {},
           std::string_view build = LEXONGINE_HOST_SOURCE_BUILD) {
    std::vector<std::string_view> views;
    for (const auto &a : arguments) {
        views.push_back(a);
    }
    return cli::dispatch(views, build, out, err, std::move(fault));
}
void strict_plan_roundtrip() {
    Directory d;
    const auto path = config(d);
    std::ostringstream out, err;
    require(invoke({"validate", "--config", path.string()}, out, err) == 0,
            "inspection preparation failed: " + err.str());
    const auto xml = out.str();
    auto decoded = take(decode_plan_xml(xml, resources(), cli_metadata_limits));
    require(wire::attribute(decoded.root(), "run_id") == "fixture-run-v1",
            "plan run identity changed");
    rejects(decode_plan_xml(replace(xml, "<physical_nodes><list><item><u64>0</u64>",
                                    "<physical_nodes><list><item><u64>1</u64>"),
                            resources(), cli_metadata_limits),
            FailureCode::CacheCorrupt);
    rejects(decode_plan_xml(replace(xml, "<field name=\"decision\"><text>ineligible</text>",
                                    "<field name=\"decision\"><text>invented</text>"),
                            resources(), cli_metadata_limits),
            FailureCode::CacheCorrupt);
    rejects(
        decode_plan_xml(replace(xml, "<field name=\"managed_buffer_bytes\"><u64>536870912</u64>",
                                "<field name=\"managed_buffer_bytes\"><u64>536870911</u64>"),
                        resources(), cli_metadata_limits),
        FailureCode::CacheCorrupt);
    rejects(decode_plan_xml(replace(xml, "schedule_policy=\"logical_first_occurrence_v1\"",
                                    "schedule_policy=\"physical_sort\""),
                            resources(), cli_metadata_limits),
            FailureCode::CacheCorrupt);
}
void unconfirmed_and_delivery_exits() {
    Directory d;
    const auto path = config(d);
    std::ostringstream out, err;
    auto arguments = [&](std::string id) {
        return std::vector<std::string>{"run",
                                        "--config",
                                        path.string(),
                                        "--report",
                                        (d.path / (id + ".xml")).string(),
                                        "--retention-log",
                                        (d.path / (id + ".log")).string()};
    };
    require(invoke(arguments("unknown"), out, err,
                   [](auto point) -> Result<void> {
                       return point == StoreFaultPoint::AfterPublish ||
                                      point == StoreFaultPoint::Reconcile
                                  ? Result<void>{diagnostic(
                                        FailureCode::CacheIO,
                                        "injected lost publication acknowledgement")}
                                  : ok();
                   }) == 5,
            "unconfirmed acceptance did not map to exit 5: " + err.str());
    require(out.str().find("<text>unconfirmed</text>") != std::string::npos,
            "unconfirmed receipt lost terminal distinction");
    out.str({});
    err.str({});
    require(invoke(arguments("recovered"), out, err) == 0,
            "restart could not recover authoritative marker");
    std::ostringstream broken;
    broken.setstate(std::ios::badbit);
    require(invoke(arguments("delivery"), broken, err) == 3,
            "broken stdout did not map to delivery failure");
    out.str({});
    err.str({});
    require(invoke(arguments("later"), out, err) == 0,
            "delivery failure damaged prior accepted origin");
    require(invoke({"list"}, out, err, {}, "incorrect-build") == 4,
            "build prerequisite failure did not map to exit 4");
}
} // namespace
int main(int argc, char **argv) {
    if (argc != 3) {
        return 1;
    }
    std::ifstream file(argv[1]);
    std::ostringstream text;
    text << file.rdbuf();
    example = text.str();
    scratch = argv[2];
    const Case cases[]{
        {"strict plan projection roundtrip and corruption", strict_plan_roundtrip},
        {"unconfirmed, recovered and optional-delivery exits", unconfirmed_and_delivery_exits}};
    return lexongine::test::run(cases);
}
