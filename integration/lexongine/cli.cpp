#include "cli.hpp"
#include "xml_report.hpp"
#include "file_evidence.hpp"
#include "fixture_source.hpp"
#include "native_files.hpp"
#include "lexongine/build_contract.hpp"
#include <filesystem>

namespace lexongine::cli {
namespace {
using wire::checked;
constexpr std::uint64_t mib=1048576;
std::shared_ptr<ResourceAccount> account(){return checked(ResourceAccount::create({{16*mib,8*mib,512*mib,64*mib,16*mib,128*mib,8*mib},4096,2048,4096,128}));}
void escaped(std::ostream& out,std::string_view text){for(char c:text){switch(c){case '&':out<<"&amp;";break;case '<':out<<"&lt;";break;case '>':out<<"&gt;";break;case '"':out<<"&quot;";break;case '\n':out<<"&#10;";break;case '\r':out<<"&#13;";break;case '\t':out<<"&#9;";break;default:if(static_cast<unsigned char>(c)<32){out<<'?';}else{out.put(c);}}}}
void diagnostic_output(std::ostream& out,const Diagnostic& e,int exit){
    constexpr std::string_view codes[]{"InvalidConfig","UnknownDefinition","DuplicateDefinition","UnsupportedContract","GraphCycle","TypeMismatch","SourceInvalid","SourceCoverage","SourceChanged","SourceKnownBoundViolation","HistoryInsufficient","ResourcePlanExceeded","ResourceExceeded","UpdateProtocol","FutureRead","MissingRequired","NumericBound","NumericOverflow","CallbackException","OutputInvalid","EvidenceQuota","EvidenceIO","CacheCorrupt","CacheIO","CacheBusy","CommitUnconfirmed","AlreadyExecuted"};
    const auto index=static_cast<std::size_t>(e.code);out<<"<diagnostic version=\"1\" exit=\""<<exit<<"\" code=\""<<(index<std::size(codes)?codes[index]:"Internal")<<"\" message=\"";escaped(out,e.text());out<<"\" truncated=\""<<(e.truncated?"true":"false")<<"\"/>\n";out.flush();
}
void deliver(std::ostream& out,std::string_view text){out.write(text.data(),static_cast<std::streamsize>(text.size()));out.put('\n');out.flush();if(!out){throw wire::Error{diagnostic(FailureCode::EvidenceIO,"optional stdout delivery failed")};}}
RequiredEvidenceSpec spec(std::string name,std::string format,const std::string& path,std::uint64_t records){return {std::move(name),std::move(format),path,"create_new",cli_metadata_limits.xml.bytes,records,"on_close","stop"};}
FileStoreLimits store_limits(const LocalCacheConfiguration& cache){
    return {{{mib,1024,32768,4*mib},32768,4096,4096,4096,16384},{{mib,65536,64,4096,8*mib},4*mib},
        {std::max<std::uint64_t>(mib,cache.max_entry_bytes),262144,1024,4096,16384,std::max<std::uint64_t>(mib,cache.max_decoded_bytes),32*mib},
        mib,512,cli_metadata_limits.xml.bytes,cli_metadata_limits};
}
void distinct_outputs(const Options& options,const CompositionDeclaration& config){
    std::array<std::string_view,5> paths{options.report,options.plan,options.before_dot,options.after_dot,config.run.retention_log.path};
    const auto input=std::filesystem::weakly_canonical(std::filesystem::absolute(options.config));
    for(std::size_t i=0;i<paths.size();++i){if(paths[i].empty()){continue;}const auto path=std::filesystem::weakly_canonical(std::filesystem::path{paths[i]});
        if(path==input){throw wire::Error{diagnostic(FailureCode::InvalidConfig,"output path aliases configuration input")};}
        for(std::size_t j=0;j<i;++j){if(!paths[j].empty()&&path==std::filesystem::weakly_canonical(std::filesystem::path{paths[j]})){throw wire::Error{diagnostic(FailureCode::InvalidConfig,"required output paths alias each other")};}}
    }
}
}
int dispatch(std::span<const std::string_view> args,std::string_view source_build,std::ostream& out,std::ostream& errors,
             std::function<Result<void>(StoreFaultPoint)> fault) noexcept {
    int failure_exit=4;
    auto result=wire::protect([&]()->Result<int>{
        auto preparation=account();failure_exit=2;auto options=checked(parse_options(args,preparation));
        failure_exit=4;auto build=checked(inspect_host_build(build_contract_id,source_build,preparation,{128*mib,mib,512}));
        auto registry=checked(Registry::create());quant::lexongine::ComponentRegistration registration{build.identity(),{},MissingIntervalPolicy::Strict};
        std::optional<ParsedComposition> parsed;
        if(options.command!=Command::List&&options.command!=Command::Describe){
            failure_exit=2;auto bytes=checked(files::read(std::filesystem::absolute(options.config),preparation,mib));
            parsed=checked(parse_xml(bytes.value,{mib,32,16384,1024,preparation}));
            const auto& changes=options.overrides;
            if(!changes.parameters.empty()||!changes.observations.empty()||changes.retention_log||changes.sharing||changes.reuse){parsed=checked(apply_overrides(*parsed,changes));}
            distinct_outputs(options,parsed->declaration());
            if(parsed->declaration().sources.size()!=1){return diagnostic(FailureCode::UnsupportedContract,"CLI v1 requires one explicitly bound source");}
            registration.missing_intervals=parsed->declaration().sources[0].missing_intervals;
        }
        failure_exit=4;auto types=checked(quant::lexongine::register_components(registry,registration));
        auto sources=checked(SourceResolver::create(preparation,65536,16));checked(fixture::register_source(registry,sources,types.bar,registration,{120,65536,131072,65536}));
        if(parsed){failure_exit=2;for(const auto& plugin:parsed->declaration().plugins){checked(load_plugin(plugin.path,plugin.build_contract_id,registry,preparation,{65536,128*mib,mib,4*mib,512}));}}
        checked(registry.freeze());checked(sources.freeze());
        if(!parsed){
            std::optional<OperationKey> selected;
            if(options.command==Command::Describe){failure_exit=2;const auto dot=options.operation.rfind('.');if(dot==std::string::npos||dot==0||dot+1==options.operation.size()){return diagnostic(FailureCode::InvalidConfig,"describe requires a qualified operation name");}
                selected=OperationKey{options.operation.substr(0,dot),options.operation.substr(dot+1),options.version,options.implementation};}
            auto xml=checked(export_registry_xml(registry,selected?&*selected:nullptr,preparation,cli_metadata_limits));failure_exit=3;deliver(out,xml.bytes());return 0;
        }
        failure_exit=2;auto composition=checked(resolve_composition(*parsed,registry));const auto& config=composition.declaration();
        if(options.command==Command::Run&&config.run.driver!=Driver::Standalone){return diagnostic(FailureCode::UnsupportedContract,"CLI run requires standalone driver; hosted runs use the host API");}
        auto store_account=account(),evidence_account=account();std::optional<FileResultStore> native;
        if(const auto* cache=std::get_if<LocalCacheConfiguration>(&config.cache)){native=checked(FileResultStore::create(*cache,types.price,build,store_account,store_limits(*cache),std::move(fault)));}
        std::vector<RequiredEvidenceSpec> required;
        if(options.command==Command::Run){required.push_back(spec("snapshot","run_snapshot_v1",options.report,config.limits.trace_events));}
        if(options.command==Command::Plan){required.push_back(spec("plan","plan_xml_v1",options.plan,1));required.push_back(spec("before_dot","dot_v1",options.before_dot,1));required.push_back(spec("after_dot","dot_v1",options.after_dot,1));}
        if(config.run.retention_log.max_events>65536||config.limits.trace_events>65536){return diagnostic(FailureCode::ResourcePlanExceeded,"CLI v1 evidence event ceiling exceeded");}
        AccountLimits runtime{{0,mib,mib,mib,mib,mib,16*mib},2048,2048,config.run.retention_log.max_events,config.run.retention_warning_steps};
        PrepareOptions prepare_options{config.run.id,checked(fixture::source_request(config.sources[0])),
            {preparation,{{120,120,0,120},1024,32768,131072,32768},registration.missing_intervals,{mib,mib,mib,mib,mib,mib,mib}},runtime,
            {mib,mib,mib,mib,mib,16384,mib},131072,mib,static_cast<Result<Sha256>(*)(std::span<const std::byte>) noexcept>(files::digest),{},required,evidence_account,
            {0,0,cli_metadata_limits.xml.workspace_bytes,16*mib,0,8*mib,65536}};
        auto prepared=checked(prepare(composition,registry,sources,native?native->service():nullptr,prepare_options));
        if(!prepared.plan){if(!prepared.rejected.empty()){return prepared.rejected.front().cause;}return diagnostic(FailureCode::InvalidConfig,"no requested composition was admitted");}
        const auto& plan=*prepared.plan;
        if(options.command==Command::Validate){
            auto xml=checked(export_plan_xml(plan,evidence_account,cli_metadata_limits));checked(decode_plan_xml(xml.bytes(),evidence_account,cli_metadata_limits));failure_exit=3;deliver(out,xml.bytes());return prepared.rejected.empty()?0:2;
        }
        if(options.command==Command::Plan){
            auto xml=checked(export_plan_xml(plan,evidence_account,cli_metadata_limits));checked(decode_plan_xml(xml.bytes(),evidence_account,cli_metadata_limits));
            auto before=checked(export_dot(plan,GraphView::Logical,evidence_account,cli_metadata_limits.xml.bytes));auto after=checked(export_dot(plan,GraphView::Physical,evidence_account,cli_metadata_limits.xml.bytes));
            failure_exit=3;checked(files::write_new("plan",options.plan,xml.bytes(),evidence_account,cli_metadata_limits.xml.bytes,FailureCode::EvidenceIO));
            checked(files::write_new("before_dot",options.before_dot,before.bytes(),evidence_account,cli_metadata_limits.xml.bytes,FailureCode::EvidenceIO));
            checked(files::write_new("after_dot",options.after_dot,after.bytes(),evidence_account,cli_metadata_limits.xml.bytes,FailureCode::EvidenceIO));deliver(out,xml.bytes());return prepared.rejected.empty()?0:2;
        }
        std::vector<ObservationAdapter> observations;
        for(const auto& request:plan.observations()){
            // Observation is an owned typed view; recording is handled by the
            // preconfigured snapshot. No numerical or financial action is hidden here.
            observations.push_back({{},request.request.id,plan.outputs(request.producer)[request.port],
                [](const RequestContext&,const ResultSlot&,PortIndex,const UpdateContext&)->Result<void>{return ok();}});
        }
        std::vector<std::string> names;for(const auto& node:plan.nodes()){names.push_back(node.spec.id);}
        auto retention=checked(FileEvidence::create(plan.evidence_specification(),config.run.id,config.run.clock,std::move(names),evidence_account,{65536,config.run.retention_log.max_events,32*mib}));
        auto snapshot=checked(SnapshotEvidence::create(prepared.plan,required[0],evidence_account,{cli_metadata_limits,cli_capture_bytes,cli_max_parents}));
        auto host_identity=checked(plan_digest(plan,evidence_account,cli_metadata_limits));
        RunServices services{std::move(observations),retention.callbacks(),{IntegrationOutcome::Completed,{},"lexongine.cli.standalone",1,host_identity},{snapshot.callbacks()}};
        failure_exit=4;auto instance=checked(instantiate(prepared.plan,std::move(services)));failure_exit=3;auto report=checked(execute(instance));
        const auto exit=report_exit_code(report);
        // Optional delivery follows the confirmed transition. It is not required
        // evidence and cannot rewrite snapshot or originating acceptance.
        auto receipt=checked(export_report_xml(report,plan,evidence_account,cli_metadata_limits));deliver(out,receipt.bytes());return exit;
    });
    if(const auto* error=std::get_if<Diagnostic>(&result)){try{diagnostic_output(errors,*error,failure_exit);}catch(...){ }return failure_exit;}
    return std::get<int>(result);
}
}
