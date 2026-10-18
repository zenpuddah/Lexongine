// Test/measurement harness over the production APIs, not another numerical engine.
#include "integration_test_support.hpp"
#include "file_result_store.hpp"
#include "file_evidence.hpp"
#include "xml_report.hpp"
#include "native_files.hpp"
#include "lexongine/build_contract.hpp"
#include <chrono>
#include <fstream>
#include <sstream>
#include <fcntl.h>
#include <unistd.h>

namespace {
using namespace lexongine;using namespace lexongine::integration_test;
using Clock=std::chrono::steady_clock;
std::uint64_t elapsed(Clock::time_point from){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-from).count());}
struct Metrics {std::uint64_t prepare=0,instantiate=0,advance=0,finalize=0,retention=0,snapshot=0,publish=0,receipt=0;};
MetadataLimits limits(){return {{1048576,65536,64,4096,8388608},4194304};}
FileStoreLimits store_limits(){return {{{1048576,1024,32768,4194304},32768,4096,4096,4096,4096},limits(),{1048576,65536,1024,4096,4096,1048576,16777216},1048576,512,1048576};}
std::uint64_t peaks(const ResourceAccount& account){std::uint64_t total=0;for(auto kind:{ResourceKind::Source,ResourceKind::State,ResourceKind::Scratch,ResourceKind::Staging,ResourceKind::Retention,ResourceKind::Graph,ResourceKind::Evidence}){total+=account.peak(kind);}return total;}
void write(std::string name,const std::filesystem::path& path,std::string_view bytes,std::shared_ptr<ResourceAccount> account){take(files::write_new(std::move(name),path,bytes,std::move(account),1048576,FailureCode::EvidenceIO));}
RequiredEvidenceSink observer_trace(std::shared_ptr<Evidence> seen,RequiredEvidenceSpec spec,std::shared_ptr<ResourceAccount> account){
    struct State {Reservation graph,buffer;std::shared_ptr<Evidence> seen;RequiredEvidenceSpec spec;std::shared_ptr<ResourceAccount> account;int fd=-1;~State(){if(fd>=0){static_cast<void>(::close(fd));}}};
    auto state=std::shared_ptr<State>(new State{take(account->reserve(ResourceKind::Graph,262144)),take(account->reserve(ResourceKind::Evidence,131072)),std::move(seen),spec,std::move(account)});
    return {{},std::move(spec),[state]()->Result<void>{state->fd=::open(state->spec.absolute_path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);if(state->fd<0){return diagnostic(FailureCode::EvidenceIO,"observer trace open failed");}return ok();},{},{},
        [state](const RunReport&)->Result<EvidenceReference>{return wire::protect([&]()->Result<EvidenceReference>{if(state->seen->values.size()>state->spec.max_records){return diagnostic(FailureCode::EvidenceQuota,"observer trace count exceeded");}std::string bytes="lexongine-observations-v1\n";bytes.reserve(65536);
            for(const auto& v:state->seen->values){const auto row=v.request+"\t"+std::to_string(v.key.sequence)+"\t"+std::to_string(v.raw)+"\t"+std::to_string(v.available)+"\n";if(row.size()>state->spec.max_bytes-bytes.size()){return diagnostic(FailureCode::EvidenceQuota,"observer trace bytes exceeded");}bytes+=row;}
            return files::finish_open(state->fd,state->spec.name,state->spec.absolute_path,bytes,state->account,state->spec.max_bytes,FailureCode::EvidenceIO);
        });}};
}
std::string graph(std::uint64_t end,std::uint64_t window,std::uint64_t lag,std::uint64_t begin,std::uint64_t later,std::string policy,bool shared,const LocalCacheConfiguration* cache,std::string retention){
    auto nodes=node("close","qaedalus.bar_close","stateless",input("bar","s","bar",policy))+sma("a","close",window,policy)+sma("b","close",window,policy)+node("lag","series.lag_price","empty_at_step_0","<param name=\"lag_steps\" value=\""+std::to_string(lag)+"\"/><param name=\"count_basis\" value=\"complete_bar_updates\"/>"+input("price","close","price",policy))+
        node("diff","qaedalus.price_difference","stateless","<param name=\"overflow\" value=\"fail\"/>"+input("left","a","mean",policy)+input("right","lag","lagged",policy));
    return xml(nodes,observe("A","a","mean",begin,end,policy)+observe("B","b","mean",later,end,policy)+observe("lag","lag","lagged",begin,end,policy)+observe("diff","diff","difference",begin,end,policy),end,policy=="strict"?MissingIntervalPolicy::Strict:MissingIntervalPolicy::Skip,Driver::Hosted,shared,cache,"test.bars","v1",retention);
}
void dataset(std::string id,std::uint64_t window,std::uint64_t lag,std::string policy,std::shared_ptr<BarInput> data,const HostBuild& host,const std::filesystem::path& output){
    auto preparation=resources(),storage=resources(),evidence_account=resources();auto registry=take(Registry::create());const auto missing=policy=="strict"?MissingIntervalPolicy::Strict:MissingIntervalPolicy::Skip;
    auto types=take(quant::lexongine::register_components(registry,{host.identity(),{},missing}));auto resolver=take(SourceResolver::create(preparation,65536,4));register_bar_source(registry,resolver,types.bar,data,host.identity(),missing);success(registry.freeze());success(resolver.freeze());
    auto configuration=cache((output/(id+".store")).string());Metrics metrics;Clock::time_point publishing;
    auto native=take(FileResultStore::create(configuration,types.price,host,storage,store_limits(),[&](StoreFaultPoint point)->Result<void>{if(point==StoreFaultPoint::BeforePublish){publishing=Clock::now();}if(point==StoreFaultPoint::AfterPublish){metrics.publish+=elapsed(publishing);}return ok();}));
    std::vector<EntryReference> first_entries;
    for(std::string_view mode:{"fresh","cached","shared","unshared","extended","invalid_source","changed_window","corrupt_artifact"}){
        if(mode=="extended"&&id!="extension"){continue;}
        const auto end=id=="extension"&&mode!="extended"?data->end-1:data->end;
        const bool disabled=mode=="shared"||mode=="unshared";
        metrics={};const auto active_window=window+(mode=="changed_window"?1:0);const auto ready=std::max(active_window-1,lag);
        const auto begin=ready<data->values.size()?data->values[ready].first:0;
        const auto later=ready+1<data->values.size()&&data->values[ready+1].first<end?data->values[ready+1].first:begin;
        data->corrupt=mode=="invalid_source";
        if(mode=="corrupt_artifact"){require(!first_entries.empty(),"missing corruption seed");std::ofstream file(std::filesystem::path{configuration.directory}/(first_entries[0].artifact_id+".price.xml"),std::ios::binary|std::ios::app);file.put('x');require(static_cast<bool>(file),"artifact corruption injection failed");}
        const auto prefix=id+"-"+std::string{mode};const auto snapshot_path=output/(prefix+".snapshot.xml"),receipt_path=output/(prefix+".receipt.xml");
        const auto text=graph(end,active_window,lag,begin,later,policy,mode!="unshared",disabled?nullptr:&configuration,(output/(prefix+".log")).string());
        write("config",output/(prefix+".config.xml"),text,evidence_account);
        auto settings=options(bar_request(end),preparation,missing);RequiredEvidenceSpec spec{"snapshot","run_snapshot_v1",snapshot_path.string(),"create_new",1048576,256,"on_close","stop"};RequiredEvidenceSpec observed{"observations","test.oracle_observations_v1",(output/(prefix+".observations.tsv")).string(),"create_new",65536,128,"on_close","stop"};settings.evidence={observed,spec};settings.evidence_account=evidence_account;settings.evidence_resources={0,0,16777216,1048576,0,1310720,196608};
        auto start=Clock::now();auto result=prepare_text(text,registry,resolver,settings,disabled?nullptr:native.service());metrics.prepare=elapsed(start);
        std::optional<Diagnostic> rejection;
        if(const auto* error=std::get_if<Diagnostic>(&result)){rejection=*error;}else{const auto& p=std::get<PrepareResult>(result);if(!p.plan){require(!p.rejected.empty(),"unattributed preparation rejection");rejection=p.rejected[0].cause;}}
        if(rejection){auto d=take(wire::Document::create(evidence_account,limits().xml));d.dom().InsertEndChild(project_diagnostic(d,*rejection));auto bytes=take(d.render());write("rejection",receipt_path,bytes.bytes(),evidence_account);
            std::cout<<id<<'\t'<<mode<<"\trejected\t-\t"<<receipt_path.filename().string()<<'\t'<<metrics.prepare<<'\n';if(mode=="fresh"){break;}continue;}
        auto prepared=std::get<PrepareResult>(std::move(result));require(prepared.rejected.empty(),"acceptance dataset unexpectedly partially admitted");const auto plan=prepared.plan;
        auto seen=std::make_shared<Evidence>();auto callbacks=services(*plan,types.price,seen);std::vector<std::string> names;for(const auto& n:plan->nodes()){names.push_back(n.spec.id);}
        auto retention=take(FileEvidence::create(plan->evidence_specification(),plan->configuration().run.id,"c",std::move(names),evidence_account,{65536,8192,8388608}));callbacks.evidence=retention.callbacks();
        auto open=callbacks.evidence.open;callbacks.evidence.open=[&metrics,open](){auto t=Clock::now();auto r=open();metrics.retention+=elapsed(t);return r;};
        auto record=callbacks.evidence.write;callbacks.evidence.write=[&metrics,record](const auto& e){auto t=Clock::now();auto r=record(e);metrics.retention+=elapsed(t);return r;};
        auto close=callbacks.evidence.finalize;callbacks.evidence.finalize=[&metrics,close](const auto& r){auto t=Clock::now();auto result=close(r);metrics.retention+=elapsed(t);return result;};
        auto snapshot=take(SnapshotEvidence::create(plan,spec,evidence_account,{limits(),1048576,4096}));auto sink=snapshot.callbacks();auto finalize=sink.finalize;sink.finalize=[&metrics,finalize](const auto& r){auto t=Clock::now();auto result=finalize(r);metrics.snapshot+=elapsed(t);return result;};callbacks.required={observer_trace(seen,observed,evidence_account),std::move(sink)};
        start=Clock::now();auto instance=take(instantiate(plan,std::move(callbacks)));metrics.instantiate=elapsed(start);
        start=Clock::now();for(;;){auto step=take(advance_one(instance));if(step.status==StepStatus::Exhausted||step.status==StepStatus::Stopped){break;}}metrics.advance=elapsed(start);
        start=Clock::now();auto report=take(finish(instance,completion()));metrics.finalize=elapsed(start);require(report.acceptance==AcceptanceStatus::Accepted,report.cause?std::string{report.cause->text()}:"acceptance failed");
        if(mode=="fresh"){first_entries=report.cache_entries;}
        start=Clock::now();auto receipt=take(export_report_xml(report,*plan,evidence_account,limits()));metrics.receipt=elapsed(start);write("receipt",receipt_path,receipt.bytes(),evidence_account);
        std::cout<<id<<'\t'<<mode<<"\taccepted\t"<<snapshot_path.filename().string()<<'\t'<<receipt_path.filename().string()<<'\t'<<metrics.prepare<<'\t'<<metrics.instantiate<<'\t'<<metrics.advance<<'\t'<<metrics.finalize<<'\t'<<metrics.retention<<'\t'<<metrics.snapshot<<'\t'<<metrics.publish<<'\t'<<metrics.receipt<<'\t'<<take(report.peak.total())<<'\t'<<peaks(*preparation)<<'\t'<<peaks(*storage)<<'\t'<<peaks(*evidence_account)<<'\t'<<wire::hex(plan->source().metadata().content_digest)<<'\t'<<wire::hex(take(plan_digest(*plan,evidence_account,limits())))<<'\t'<<wire::hex(host.identity().binary_digest)<<'\n';
    }
}
}
int main(int argc,char** argv){if(argc!=3){std::cerr<<"explicit bounded cases file and output directory required\n";return 2;}try{
    const std::filesystem::path output=argv[2];require(output.is_absolute()&&std::filesystem::is_directory(output),"explicit existing output directory required");
    auto bootstrap=take(ResourceAccount::create({{16777216,4194304,536870912,33554432,4194304,67108864,8388608},4096,1024,8192,128}));const auto host=take(inspect_host_build(build_contract_id,LEXONGINE_HOST_SOURCE_BUILD,bootstrap,{134217728,1048576,512}));
    auto bytes=take(files::read(std::filesystem::absolute(argv[1]),bootstrap,65536));std::istringstream input(bytes.value);std::string id;unsigned cases=0;
    while(input>>id){require(++cases<=32&&id.size()<=32&&std::all_of(id.begin(),id.end(),[](char c){return(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_';}),"invalid case identity/count");std::uint64_t n=0,window=0,lag=0;std::string policy;require(static_cast<bool>(input>>n>>window>>lag>>policy)&&n>0&&n<=16&&window>0&&window<n&&lag>0&&lag<n&&(policy=="strict"||policy=="skip"),"invalid bounded dataset header");
        auto data=std::make_shared<BarInput>();data->end=n;data->values.clear();data->gaps.clear();for(std::uint64_t key=0;key<n;++key){std::string token;require(static_cast<bool>(input>>token),"missing dataset input");if(token=="gap"){data->gaps.push_back({{"c",key,key+1},{static_cast<std::int64_t>(key*10),static_cast<std::int64_t>((key+1)*10)},7});}else{data->values.push_back({key,take(parse_integer<std::int64_t>(token))});}}
        dataset(id,window,lag,policy,std::move(data),host,output);
    }
    require(cases>0&&input.eof(),"no complete dataset input");return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
