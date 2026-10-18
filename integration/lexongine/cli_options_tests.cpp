#include "cli_options.hpp"
#include "fixture_source.hpp"
#include "xml_frontend.hpp"
#include "../../tests/lexongine_test_support.hpp"
#include <fstream>
#include <sstream>

namespace {
using namespace lexongine;
using namespace lexongine::test;
std::string example;
std::shared_ptr<ResourceAccount> resources() {
    return take(ResourceAccount::create({{1048576,1048576,8388608,1048576,1048576,16777216,1048576},128,128,1024,128}));
}
Result<cli::Options> options(std::initializer_list<std::string_view> args) { return cli::parse_options({args.begin(),args.size()},resources()); }
void exact_commands() {
    require(take(options({"list"})).command==cli::Command::List,"list command changed");
    const auto describe=take(options({"describe","--operation","statistics.sma_price","--version","1","--implementation","builtin-v1"}));
    require(describe.version==1&&describe.operation=="statistics.sma_price","describe selector changed");
    const auto plan=take(options({"plan","--config","input.xml","--plan","/tmp/plan.xml","--before-dot","/tmp/before.dot","--after-dot","/tmp/after.dot"}));
    require(plan.command==cli::Command::Plan&&plan.config=="input.xml","explicit plan options changed");
    const auto run=take(options({"run","--config","input.xml","--report","/tmp/report.xml","--set","fast_A.window=21","--observe","fast_A_trial","--reuse","disabled","--sharing","disabled"}));
    require(run.overrides.parameters.size()==1&&run.overrides.parameters[0].producer=="fast_A"&&run.overrides.parameters[0].value=="21","parameter override not split exactly");
}
void invalid_arguments() {
    for(const auto args:std::initializer_list<std::initializer_list<std::string_view>>{
        {},{"unknown"},{"list","--config","x"},{"run","--config","x"},
        {"run","--config","x","--report","relative"},
        {"describe","--operation","x.y","--version","01","--implementation","x"},
        {"validate","--config","x","--config","y"},
        {"validate","--config","x","--set","bad"},
        {"validate","--config","x","--set","n.p=1","--set","n.p=2"},
        {"validate","--config","x","--observe","a","--observe","a"},
        {"validate","--config","x","--sharing","automatic"},
        {"validate","--config","x","--reuse","on"},
        {"validate","--config","x","--report","/tmp/out"},
        {"validate","--config","--observe","x"}}) {
        rejects(options(args),FailureCode::InvalidConfig);
    }
}
void immutable_overrides_use_registered_schema() {
    auto account=resources();auto parsed=take(parse_xml(example,{1048576,32,16384,1024,account}));
    auto choice=take(options({"validate","--config","input.xml","--set","fast_A.window=21","--observe","slow_trial","--observe","fast_A_trial","--retention-log","/tmp/override.log","--reuse","disabled"}));
    auto overridden=take(apply_overrides(parsed,choice.overrides));
    require(parsed.declaration().nodes[1].parameters.values[0].text=="20"&&parsed.declaration().observations.size()==4,"overrides mutated original immutable input");
    require(overridden.declaration().nodes[1].parameters.values[0].source_location.path=="<cli>"&&overridden.declaration().observations[0].id=="fast_A_trial"&&overridden.declaration().observations[1].id=="slow_trial","override provenance or declaration order lost");
    require(overridden.declaration().run.retention_log.path=="/tmp/override.log"&&std::holds_alternative<DisabledCache>(overridden.declaration().cache)&&!overridden.declaration().overrides.empty(),"explicit override ledger missing");
    auto registry=take(Registry::create());quant::lexongine::ComponentRegistration build{{Sha256{std::byte{1}},Sha256{std::byte{2}},Sha256{std::byte{3}},"cli-test"},{},MissingIntervalPolicy::Strict};
    auto types=take(quant::lexongine::register_components(registry,build));auto source=take(SourceResolver::create(account,65536,4));
    success(fixture::register_source(registry,source,types.bar,build,{120,65536,131072,65536}));success(registry.freeze());
    const auto resolved=take(resolve_composition(overridden,registry));
    const auto& params=resolved.node_parameters()[1].parameters.values;
    const auto found=std::find_if(params.begin(),params.end(),[](const auto& p){return p.name=="window";});
    require(found!=params.end()&&std::get<std::uint64_t>(found->value)==21&&found->provenance.starts_with("<cli>:"),"override bypassed typed schema or lost origin");
    choice.overrides.parameters[0].value="01";
    rejects(resolve_composition(take(apply_overrides(parsed,choice.overrides)),registry),FailureCode::InvalidConfig);
    choice.overrides.parameters[0].parameter="unknown";rejects(apply_overrides(parsed,choice.overrides),FailureCode::InvalidConfig);
    choice.overrides.parameters.clear();choice.overrides.observations={"unknown"};rejects(apply_overrides(parsed,choice.overrides),FailureCode::InvalidConfig);
    CompositionOverrides enable;enable.reuse="automatic_read_write";rejects(apply_overrides(overridden,enable),FailureCode::InvalidConfig);
}
}
int main(int argc,char** argv) {
    if(argc!=2){std::cerr<<"explicit example path required\n";return 1;}
    std::ifstream input(argv[1]);std::ostringstream contents;contents<<input.rdbuf();example=contents.str();
    const Case cases[]{{"exact CLI command grammar",exact_commands},{"invalid and duplicate CLI arguments",invalid_arguments},{"immutable overrides and registry validation",immutable_overrides_use_registered_schema}};
    return lexongine::test::run(cases);
}
