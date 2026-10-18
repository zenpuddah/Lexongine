#include "integration_test_support.hpp"
#include "backtest_host_probe.hpp"
#include "file_result_store.hpp"
#include "file_evidence.hpp"
#include "lexongine/build_contract.hpp"
#include <unistd.h>
namespace {
using namespace lexongine;using namespace lexongine::integration_test;
std::filesystem::path scratch;
std::filesystem::path executable;
struct Directory {std::filesystem::path path;Directory(){auto pattern=(scratch/"hosted-probe.XXXXXX").string();std::vector<char> value(pattern.begin(),pattern.end());value.push_back('\0');require(::mkdtemp(value.data())!=nullptr,"scratch creation failed");path=value.data();}~Directory(){std::error_code e;std::filesystem::remove_all(path,e);}};
struct Environment {
    std::shared_ptr<ResourceAccount> account=resources();Registry registry=take(Registry::create());ImplementationIdentity identity;
    quant::lexongine::ComponentTypes types;SourceResolver resolver;std::shared_ptr<BarInput> data;MissingIntervalPolicy policy;
    Environment(std::shared_ptr<BarInput> input,MissingIntervalPolicy missing=MissingIntervalPolicy::Skip,ImplementationIdentity id=build()):identity(id),types(take(quant::lexongine::register_components(registry,{id,{},missing}))),resolver(take(SourceResolver::create(account,65536,4))),data(std::move(input)),policy(missing){register_bar_source(registry,resolver,types.bar,data,id,missing);}
    void freeze(){success(registry.freeze());success(resolver.freeze());}
    PrepareOptions settings(){return options(bar_request(data->end),account,policy);}
};
std::string graph(std::uint64_t end,MissingIntervalPolicy missing=MissingIntervalPolicy::Skip,Driver driver=Driver::Hosted,bool sharing=true,const LocalCacheConfiguration* cache=nullptr,std::string retention="/test/retention.log"){
    const std::string policy=missing==MissingIntervalPolicy::Skip?"skip":"strict";
    auto nodes=node("close","qaedalus.bar_close","stateless",input("bar","s","bar",policy))+sma("a","close",2,policy)+sma("b","close",2,policy)+node("lag","series.lag_price","empty_at_step_0","<param name=\"lag_steps\" value=\"1\"/><param name=\"count_basis\" value=\"complete_bar_updates\"/>"+input("price","close","price",policy));
    return xml(nodes,observe("trial_A","a","mean",2,end,policy)+observe("trial_B","b","mean",2,end,policy)+observe("lag","lag","lagged",2,end,policy),end,missing,driver,sharing,cache,"test.bars","v1",retention);
}
struct Run {RunReport report;std::shared_ptr<Evidence> evidence;std::vector<StepStatus> statuses;std::vector<std::uint64_t> keys;};
Run execute_probe(std::shared_ptr<const PreparedPlan> plan,const RegisteredType<Price>& price,std::shared_ptr<host_probe::Controller> host={}){
    auto captured=std::make_shared<Evidence>();auto callbacks=services(*plan,price,captured,host?host->experiment():Sha256{std::byte{9}});auto instance=take(instantiate(plan,std::move(callbacks)));Run result{{},captured,{},{}};
    if(plan->configuration().run.driver==Driver::Standalone){result.report=take(execute(instance));return result;}
    for(;;){auto step=take(advance_one(instance));result.statuses.push_back(step.status);if(step.key){result.keys.push_back(step.key->sequence);}if(host){success(host->consume(step));}
        if(step.source){const auto* frame=step.source->frame();rejects(step.source->read<Bar>(0,frame->available_at_ns-1),FailureCode::FutureRead);require(take(step.source->read<Bar>(0,frame->available_at_ns)).value.close==take(step.source->read<Bar>(0,frame->available_at_ns)).value.close,"repeated source reads changed data");}
        if(step.status==StepStatus::Exhausted||step.status==StepStatus::Stopped){break;}}
    result.report=take(finish(instance,completion(host?host->experiment():Sha256{std::byte{9}})));rejects(advance_one(instance),FailureCode::AlreadyExecuted);return result;
}
void skip_history_receipts_and_reuse(){Environment env(std::make_shared<BarInput>());env.freeze();auto config=cache();auto memory=MemoryStore::make(config,env.types.price);
    auto p=take(prepare_text(graph(4,env.policy,Driver::Hosted,true,&config),env.registry,env.resolver,env.settings(),memory.store()));require(p.plan&&p.rejected.empty(),"gap fixture did not prepare");auto first=execute_probe(p.plan,env.types.price);
    require(first.report.acceptance==AcceptanceStatus::Accepted&&first.report.complete_updates==3&&first.report.gap_events==1&&first.report.missing_intervals==1,"skip report lost coverage");
    require(first.keys==std::vector<std::uint64_t>{0,1,2,3}&&first.statuses==std::vector<StepStatus>{StepStatus::Processed,StepStatus::Gap,StepStatus::Processed,StepStatus::Processed,StepStatus::Exhausted},"host timeline compressed keys or omitted warming/gap");
    require(first.evidence->values.size()==6,"gap delivered a stale/zero observation");
    const std::array<std::int64_t,6> expected{105,105,100,115,115,110};for(std::size_t i=0;i<expected.size();++i){require(first.evidence->values[i].raw==expected[i],"Skip SMA/lag shifted at gap");}
    require(first.evidence->values[2].parents[0].publication.key.sequence==0&&first.evidence->values[5].parents[0].publication.key.sequence==2&&first.evidence->values[2].available==30,"lag lost displaced parent or backdated availability");
    auto next=take(prepare_text(graph(4,env.policy,Driver::Hosted,true,&config),env.registry,env.resolver,env.settings(),memory.store()));auto second=execute_probe(next.plan,env.types.price);
    require(second.evidence->values==first.evidence->values&&second.keys==first.keys&&second.statuses==first.statuses&&second.report.producers[2].advances==0&&second.report.producers[2].cached_deliveries==3,"same-gap cached outcomes differ");
    auto standalone=take(prepare_text(graph(4,env.policy,Driver::Standalone,false),env.registry,env.resolver,env.settings()));auto fresh=execute_probe(standalone.plan,env.types.price);
    require(fresh.evidence->values==first.evidence->values&&fresh.report.producers[3].advances==3,"standalone/unshared gap execution changed outputs");
    const auto lookups=memory.lookups();env.data->corrupt=true;auto invalid=take(prepare_text(graph(4,env.policy,Driver::Hosted,true,&config),env.registry,env.resolver,env.settings(),memory.store()));require(!invalid.plan&&memory.lookups()==lookups,"cached artifacts bypassed invalid present source");
    env.data->corrupt=false;env.data->gaps[0].declared_reason=8;
    auto changed=take(prepare_text(graph(4,env.policy,Driver::Hosted,true,&config),env.registry,env.resolver,env.settings(),memory.store()));require(!changed.plan->artifact(2)&&changed.plan->identity(2)!=p.plan->identity(2),"changed gap ledger reused incompatible artifacts");
    env.data->gaps[0].declared_reason=7;memory.corrupt_first_payload();rejects(prepare_text(graph(4,env.policy,Driver::Hosted,true,&config),env.registry,env.resolver,env.settings(),memory.store()),FailureCode::CacheCorrupt);
}
void strict_all_gap_missing_tail_and_identity(){
    Environment strict(std::make_shared<BarInput>(),MissingIntervalPolicy::Strict);strict.freeze();auto rejected=take(prepare_text(graph(4,strict.policy),strict.registry,strict.resolver,strict.settings()));require(!rejected.plan&&!rejected.rejected.empty()&&rejected.rejected[0].cause.code==FailureCode::SourceCoverage,"Strict admitted known gap");
    auto empty=std::make_shared<BarInput>();empty->values.clear();empty->gaps={{{"c",0,4},{0,40},7}};Environment all(empty);all.freeze();require(!take(prepare_text(graph(4),all.registry,all.resolver,all.settings())).plan,"all-gap source satisfied require-value");
    auto tail=std::make_shared<BarInput>();tail->end=5;Environment incomplete(tail);incomplete.freeze();require(!take(prepare_text(graph(5),incomplete.registry,incomplete.resolver,incomplete.settings())).plan,"undeclared missing tail accepted");tail->gaps.push_back({{"c",4,5},{40,50},8});auto complete=take(prepare_text(graph(5),incomplete.registry,incomplete.resolver,incomplete.settings()));auto done=execute_probe(complete.plan,incomplete.types.price);require(done.report.acceptance==AcceptanceStatus::Accepted&&done.report.gap_events==2&&done.report.complete_updates==3,"declared tail gap lost closure");
    auto contiguous=std::make_shared<BarInput>();contiguous->values={{0,100},{1,100},{2,110},{3,120}};contiguous->gaps.clear();Environment skip(contiguous),plain(contiguous,MissingIntervalPolicy::Strict);skip.freeze();plain.freeze();auto a=take(prepare_text(graph(4,skip.policy),skip.registry,skip.resolver,skip.settings()));auto b=take(prepare_text(graph(4,plain.policy),plain.registry,plain.resolver,plain.settings()));require(a.plan->identity(2)!=b.plan->identity(2),"Strict/Skip identity collapsed on equal complete values");
    Environment history(std::make_shared<BarInput>());history.freeze();const auto nodes=node("close","qaedalus.bar_close","stateless",input("bar","s","bar","skip"))+sma("mean","close",3,"skip");
    auto insufficient=take(prepare_text(xml(nodes,observe("mean","mean","mean",2,4,"skip"),4,MissingIntervalPolicy::Skip),history.registry,history.resolver,history.settings()));require(!insufficient.plan&&insufficient.rejected[0].cause.code==FailureCode::HistoryInsufficient,"gap key distance substituted for complete-input history");
    auto no_gap_budget=history.settings();no_gap_budget.source_services.limits.coverage.max_gap_entries=0;rejects(prepare_text(graph(4),history.registry,history.resolver,no_gap_budget),FailureCode::ResourcePlanExceeded);
}
void independent_trials_and_cache_state(){Environment env(std::make_shared<BarInput>());env.freeze();auto config=cache();auto memory=MemoryStore::make(config,env.types.price);
    auto make_host=[](bool enabled){return take(host_probe::Controller::create({host_probe::Trial{"A","trial_A",true,0},host_probe::Trial{"B","trial_B",enabled,0}},resources(),16));};
    auto first_plan=take(prepare_text(graph(4,env.policy,Driver::Hosted,true,&config),env.registry,env.resolver,env.settings(),memory.store()));auto host=make_host(true);auto first=execute_probe(first_plan.plan,env.types.price,host);require(host->intents().size()==4&&host->gaps()==1&&host->processed()==3,"two trials lost independent intents or acted on gaps/warmup");
    require(host->intents()[0].trial==0&&host->intents()[1].trial==1&&host->intents()[0].step==2&&host->intents()[2].step==3,"toy intent order/key changed");
    auto changed=make_host(false);auto next=take(prepare_text(graph(4,env.policy,Driver::Hosted,true,&config),env.registry,env.resolver,env.settings(),memory.store()));require(next.plan->identity(2)==first_plan.plan->identity(2)&&changed->experiment()!=host->experiment(),"host state leaked into pure feature identity");
    auto replayed=execute_probe(next.plan,env.types.price,changed);require(changed->intents().size()==2&&changed->trials()[1].decisions==2&&replayed.report.producers[2].advances==0&&replayed.evidence->values==first.evidence->values,"cache restored host state or suppressed trial evaluation");
}
struct EffectEvent {std::string name;std::uint64_t key;friend bool operator==(const EffectEvent&,const EffectEvent&)=default;};
struct EffectLedger {std::vector<EffectEvent> events;std::optional<std::string> fail;std::uint64_t instances=0;EffectLedger(){events.reserve(32);}};
void register_effect(Environment& env,std::shared_ptr<EffectLedger> ledger){
    OperationContract definition{{"test","effect",1,"builtin-v1"},env.identity,{1,{{"name",ParamKind::Text,true,{}}}},{{"price",env.types.price.policy.contract,Alignment::SameStep,env.policy,AbsencePolicy::RequireValue}},
        {{"receipt",env.types.price.policy.contract,StorageKind::ScalarCopy,protocol("unsupported_effect_codec"),protocol("qaedalus.price9"),false}},
        {SharingClass::Unique,protocol("stateless"),protocol("same_step"),protocol("test.intent_occurrence"),false,false,false,{"unique-effect-order-probe"}},{"interval_v1"},{Alignment::SameStep},{env.policy},{"count"},"Test-only unique effect with explicit ledger and bounded echo receipt"};
    const auto schema=definition.parameters;const auto price=env.types.price;
    success(env.registry.register_definition(std::move(definition),{},[ledger,schema,price](const ParamSet& input,const PreparationFacts&)->Result<PreparedKernel>{return memory_detail::protect([&]()->Result<PreparedKernel>{
        auto parameters=memory_detail::checked(parse_parameters(input,schema,64));const auto name=std::get<std::string>(parameters.values[0].value);
        return PreparedKernel::create({},std::move(parameters),CountHistory{0},{0,4096,0,16384,4096,8192,0},
            [ledger,name,price](InstanceServices& services)->Result<KernelInstance>{return memory_detail::protect([&]()->Result<KernelInstance>{
                auto charge=memory_detail::checked(services.account->reserve(ResourceKind::State,4096));auto account=services.account;auto inputs=services.inputs;++ledger->instances;
                return KernelInstance::create({},std::move(charge),[ledger,name,price,account,inputs=std::move(inputs)](const UpdateContext& context)->Result<PendingOutcome>{return memory_detail::protect([&]()->Result<PendingOutcome>{
                    auto value=memory_detail::checked(inputs[0].slot->read<Price>(inputs[0].port,context,0));
                    if(ledger->fail&&*ledger->fail==name){return diagnostic(FailureCode::CallbackException,"injected unique action failure");}
                    if(ledger->events.size()==32){return diagnostic(FailureCode::EvidenceQuota,"unique-action ledger capacity exceeded");}
                    auto pending=memory_detail::checked(PendingOutcome::create(account,context,1));PublicationAllocator allocator{account,1};
                    auto frozen=memory_detail::checked(allocator.freeze(value.value,price));memory_detail::checked(pending.set_output(0,memory_detail::checked(ErasedValue::from(std::move(frozen),price.policy.contract))));
                    memory_detail::checked(ProvenanceValidator::attach(pending,memory_detail::checked(allocator.parents({value.parent()})),inputs,0,context));
                    ledger->events.push_back({name,context.key.sequence});return pending;
                });});
            });});
    });}));
}
std::string effect_graph(bool shared,const LocalCacheConfiguration* config,bool duplicate=false){const auto p=input("bar","s","bar","skip");auto nodes=node("p1","qaedalus.bar_close","stateless",p)+node("B","test.effect","stateless","<param name=\"name\" value=\""+std::string{duplicate?"A":"B"}+"\"/>"+input("price","p2","price","skip"))+node("A","test.effect","stateless","<param name=\"name\" value=\"A\"/>"+input("price","p1","price","skip"))+node("p2","qaedalus.bar_close","stateless",p);
    return xml(nodes,observe("price","p1","price",0,4,"skip"),4,MissingIntervalPolicy::Skip,Driver::Hosted,shared,config);
}
void unique_order_multiplicity_failure_and_cache(){
    for(const auto& fail:std::array<std::string,3>{"","A","B"}){std::vector<EffectEvent> expected;
        for(bool shared:{false,true}){Environment env(std::make_shared<BarInput>());auto ledger=std::make_shared<EffectLedger>();if(!fail.empty()){ledger->fail=fail;}register_effect(env,ledger);env.freeze();auto settings=env.settings();settings.effects={{"A",{"c",2,4}},{"B",{"c",2,4}}};
            auto prepared=take(prepare_text(effect_graph(shared,nullptr),env.registry,env.resolver,settings));auto result=execute_probe(prepared.plan,env.types.price);
            if(!shared){expected=ledger->events;}else{require(ledger->events==expected,"sharing changed deterministic unique-action failure prefix");}
            require(ledger->instances==2,"unique actions shared mutable instance state");
            if(fail.empty()){require(result.report.acceptance==AcceptanceStatus::Accepted&&ledger->events==std::vector<EffectEvent>{{"A",2},{"B",2},{"A",3},{"B",3}},"physical sorting reversed A/B actions or actions ran during warmup/gap");}
            else{require(result.report.acceptance==AcceptanceStatus::Rejected&&result.report.cause->code==FailureCode::CallbackException,"unique action failure accepted");if(fail=="A"){require(!result.report.last_reach[4].reached&&!result.report.last_reach[2].reached,"failure before alias eagerly reached downstream nodes");}else{require(ledger->events==std::vector<EffectEvent>{{"A",2}},"later failure erased earlier unique action");}}
        }
    }
    Environment env(std::make_shared<BarInput>());auto ledger=std::make_shared<EffectLedger>();register_effect(env,ledger);env.freeze();auto config=cache();auto memory=MemoryStore::make(config,env.types.price);auto settings=env.settings();settings.effects={{"A",{"c",2,4}},{"B",{"c",2,4}}};
    auto first=take(prepare_text(effect_graph(true,&config,true),env.registry,env.resolver,settings,memory.store()));require(first.plan->identity(2)==first.plan->identity(3)&&first.plan->graph().representative(2)!=first.plan->graph().representative(3),"equal unique computations were merged");
    require(execute_probe(first.plan,env.types.price).report.acceptance==AcceptanceStatus::Accepted,"unique cached seed failed");const auto entries=memory.entries();ledger->events.clear();
    auto second=take(prepare_text(effect_graph(true,&config,true),env.registry,env.resolver,settings,memory.store()));auto replayed=execute_probe(second.plan,env.types.price);
    require(entries==1&&replayed.report.producers[1].advances==0&&ledger->events==std::vector<EffectEvent>{{"A",2},{"A",2},{"A",3},{"A",3}},"cached pure producer suppressed or deduplicated unique effects");
}
MetadataLimits metadata_limits(){return {{1048576,65536,64,4096,8388608},4194304};}
FileStoreLimits native_limits(){return {{{1048576,1024,32768,4194304},32768,4096,4096,4096,4096},metadata_limits(),{1048576,65536,1024,4096,4096,1048576,16777216},1048576,512,1048576};}
void native_host_evidence_failure_gates(){
    // Build inspection simultaneously retains the original executable read and
    // dependency/recheck hashing buffers. Instrumented debug binaries need a
    // separate bootstrap allowance; this does not alter the plan/run budgets.
    const auto bootstrap=take(ResourceAccount::create({{16777216,4194304,536870912,33554432,4194304,67108864,8388608},4096,1024,8192,128}));
    std::cout<<"HOST_BOOTSTRAP executable_bytes="<<std::filesystem::file_size(executable)<<" scratch_limit=536870912\n";
    const auto host_build=take(inspect_host_build(build_contract_id,LEXONGINE_HOST_SOURCE_BUILD,bootstrap,{134217728,1048576,512}));
    for(unsigned mode=0;mode<5;++mode){Directory directory;auto config=cache((directory.path/"cache").string());Environment env(std::make_shared<BarInput>(),MissingIntervalPolicy::Skip,host_build.identity());env.freeze();auto store_account=resources(),evidence_account=resources();auto native=take(FileResultStore::create(config,env.types.price,host_build,store_account,native_limits()));
        auto run=[&](std::string id,unsigned failure){auto controller=take(host_probe::Controller::create({host_probe::Trial{"A","trial_A",true,0},host_probe::Trial{"B","trial_B",true,0}},evidence_account,16));
            auto settings=env.settings();auto domain=host_probe::evidence_spec((directory.path/(id+".intents")).string());RequiredEvidenceSpec snapshot_spec{"snapshot","run_snapshot_v1",(directory.path/(id+".xml")).string(),"create_new",1048576,128,"on_close","stop"};
            settings.evidence={domain,snapshot_spec};settings.evidence_account=evidence_account;settings.evidence_resources={0,0,16777216,1048576,0,1048576,262144};
            auto prepared=take(prepare_text(graph(4,env.policy,Driver::Hosted,true,&config,(directory.path/(id+".log")).string()),env.registry,env.resolver,settings,native.service()));require(prepared.plan!=nullptr,"native gap plan rejected");auto seen=std::make_shared<Evidence>();auto callbacks=services(*prepared.plan,env.types.price,seen,controller->experiment());
            std::vector<std::string> names;for(const auto& node:prepared.plan->nodes()){names.push_back(node.spec.id);}auto retention=take(FileEvidence::create(prepared.plan->evidence_specification(),"integration-probe","c",std::move(names),evidence_account,{65536,8192,8388608}));callbacks.evidence=retention.callbacks();
            auto snapshot=take(SnapshotEvidence::create(prepared.plan,snapshot_spec,evidence_account,{metadata_limits(),1048576,4096}));callbacks.required={take(host_probe::evidence(controller,domain,evidence_account,failure==3)),snapshot.callbacks()};auto instance=take(instantiate(prepared.plan,std::move(callbacks)));auto completed=completion(controller->experiment());
            for(;;){auto receipt=take(advance_one(instance));auto applied=controller->consume(receipt,failure==0?std::optional<std::uint32_t>{1}:std::nullopt);
                if(auto* error=std::get_if<Diagnostic>(&applied)){success(fail_host(instance,*error));completed.kind=IntegrationOutcome::Failed;completed.cause=*error;rejects(advance_one(instance),FailureCode::UpdateProtocol);break;}
                if(failure==2&&receipt.status==StepStatus::Gap){auto error=diagnostic(FailureCode::CallbackException,"injected host abort");success(abort_host(instance,error));completed.kind=IntegrationOutcome::Aborted;completed.cause=error;break;}
                if(receipt.status==StepStatus::Exhausted){if(failure==1){auto error=diagnostic(FailureCode::CallbackException,"host failed after numerical exhaustion");success(fail_host(instance,error));completed.kind=IntegrationOutcome::Failed;completed.cause=error;}break;}}
            auto report=take(finish(instance,completed));if(failure==0){require(controller->intents().size()==1&&controller->intents()[0].trial==0,"host failure lost earlier partial trial intent");}
            if(failure==4){require(controller->intents().size()==4&&controller->trials()[0].decisions==2&&controller->trials()[1].decisions==2,"native feature reuse suppressed independent domain decisions");}
            return report;
        };
        auto first=run("first",mode);if(mode<4){require(first.acceptance==AcceptanceStatus::Rejected&&first.cache==CachePublicationStatus::NotPublished,"host/evidence failure promoted cache entries");for(const auto& file:std::filesystem::directory_iterator(directory.path/"cache")){require(!file.path().filename().string().ends_with(".accepted.xml"),"failed host left accepted marker");}
            if(mode==1||mode==3){require(first.numerical==NumericalOutcome::Completed,"post-exhaustion failure changed completed numerical outcome");}
        }else{require(first.acceptance==AcceptanceStatus::Accepted&&first.evidence.size()==3,"host domain evidence missing from accepted inventory");}
        auto retry=run("retry",4);require(retry.acceptance==AcceptanceStatus::Accepted&&retry.producers[2].advances==(mode==4?0U:3U),"native gap reuse or failed-origin isolation changed");
        if(mode==4){auto failed_hit=run("failed-hit",3);require(failed_hit.acceptance==AcceptanceStatus::Rejected,"cached host evidence failure accepted");auto later=run("later",4);require(later.acceptance==AcceptanceStatus::Accepted&&later.producers[2].advances==0,"failed cached run invalidated prior accepted origin");}
    }
}
}
int main(int argc,char** argv){if(argc!=2){return 1;}scratch=argv[1];executable=argv[0];const Case cases[]{{"Skip history, hosted receipts and exact-gap reuse",skip_history_receipts_and_reuse},{"Strict, all-gap, tail coverage and policy identity",strict_all_gap_missing_tail_and_identity},{"independent trial decisions across cached feature runs",independent_trials_and_cache_state},{"unique order, multiplicity, failure prefix and cached effects",unique_order_multiplicity_failure_and_cache},{"native host/domain-evidence acceptance gates",native_host_evidence_failure_gates}};return lexongine::test::run(cases);}
