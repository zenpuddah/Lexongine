#pragma once
// Test-only source/composition builders. No financial model or production provider.
#include "fixture.hpp"
#include "xml_frontend.hpp"
#include "test_memory_store.hpp"
#include "quant/data/validation.hpp"

namespace lexongine::integration_test {
using namespace lexongine::test;
using Price=quant::data::Price;
using Bar=quant::data::Bar;
inline Contract protocol(std::string id){return {std::move(id),1,{1,{}}};}
inline std::shared_ptr<ResourceAccount> resources(){return take(ResourceAccount::create({{16777216,4194304,134217728,33554432,4194304,67108864,8388608},4096,1024,8192,128}));}
inline ImplementationIdentity build(){return {Sha256{std::byte{1}},Sha256{std::byte{2}},Sha256{std::byte{3}},"integration-probe-v1"};}
inline LocalCacheConfiguration cache(std::string directory="/test/integration-cache"){return {"automatic_read_write","local_files",std::move(directory),"exact_full_scope","same_build_local_writer","sha256","single_process_lock","fail_preparation","bypass_new_entries","explicit_only",64,8388608,1048576,1048576};}
inline std::string input(std::string port,std::string producer,std::string output,std::string missing){return "<input port=\""+port+"\" producer=\""+producer+"\" output=\""+output+"\" prior_outputs=\"0\" missing_intervals=\""+missing+"\"/>";}
inline std::string node(std::string id,std::string operation,std::string init,std::string content){return "<node id=\""+id+"\" operation=\""+operation+"\" semantic_version=\"1\" implementation_id=\"builtin-v1\" alignment=\"same_step\" initialization=\""+init+"\">"+content+"</node>";}
inline std::string sma(std::string id,std::string producer,std::uint64_t window,std::string missing){return node(id,"statistics.sma_price","empty_at_step_0","<param name=\"window\" value=\""+std::to_string(window)+"\"/><param name=\"max_abs_input_raw\" value=\"102000000000\"/><param name=\"sum_width\" value=\"signed64\"/><param name=\"rounding\" value=\"nearest_ties_even\"/><param name=\"overflow\" value=\"fail\"/>"+input("price",producer,"price",missing));}
inline std::string observe(std::string id,std::string producer,std::string output,std::uint64_t begin,std::uint64_t end,std::string missing){return "<observe id=\""+id+"\" producer=\""+producer+"\" output=\""+output+"\" begin_step=\""+std::to_string(begin)+"\" end_step=\""+std::to_string(end)+"\" absence=\"require_value\" prior_outputs=\"0\" missing_intervals=\""+missing+"\"/>";}
inline std::string xml(std::string nodes,std::string observations,std::uint64_t end,MissingIntervalPolicy policy,Driver driver=Driver::Hosted,bool sharing=true,const LocalCacheConfiguration* store=nullptr,std::string source="test.bars",std::string version="v1",std::string retention="/test/retention.log"){
    const std::string missing=policy==MissingIntervalPolicy::Strict?"strict":"skip";
    std::string cache_xml="<cache mode=\"disabled\"/>";
    if(store){cache_xml="<cache mode=\"automatic_read_write\" backend=\"local_files\" directory=\""+store->directory+"\" matching=\"exact_full_scope\" trust=\"same_build_local_writer\" integrity=\"sha256\" writer=\"single_process_lock\" corruption=\"fail_preparation\" capacity=\"bypass_new_entries\" cleanup=\"explicit_only\" max_entries=\"64\" max_store_bytes=\"8388608\" max_entry_bytes=\"1048576\" max_decoded_bytes=\"1048576\"/>";}
    return "<composition version=\"1\"><run id=\"integration-probe\" clock=\"c\" driver=\""+std::string{driver==Driver::Hosted?"hosted":"standalone"}+"\" execution=\"synchronous\" sharing=\""+(sharing?"exact":"disabled")+"\" failure=\"stop\" trace=\"steps\" retention_warning_steps=\"128\" retention_log_path=\""+retention+"\" retention_log_format=\"key_value_lines_v1\" retention_log_open=\"create_new\" retention_log_flush=\"on_event\" retention_log_failure=\"stop\" retention_log_overflow=\"stop\" retention_log_max_bytes=\"1048576\" retention_log_max_events=\"8192\"/>"
        "<limits xml_bytes=\"1048576\" xml_depth=\"16\" xml_elements=\"1024\" string_bytes=\"1024\" nodes=\"64\" edges=\"128\" requests=\"16\" source_updates=\"120\" managed_buffer_bytes=\"134217728\" trace_events=\"4096\" trace_message_bytes=\"256\"/><plugins/>"+cache_xml+
        "<sources><source id=\"s\" operation=\""+source+"\" semantic_version=\"1\" implementation_id=\"builtin-v1\" source_version=\""+version+"\" clock=\"c\" begin_step=\"0\" end_step=\""+std::to_string(end)+"\" completeness=\"complete_only\" empty=\"reject\" missing=\"fail\" missing_intervals=\""+missing+"\" availability=\"explicit_probe_timeline\"/></sources><nodes>"+nodes+"</nodes><observations>"+observations+"</observations></composition>";
}
inline PrepareOptions options(const SourceRequest& request,std::shared_ptr<ResourceAccount> account,MissingIntervalPolicy missing){return {"integration-probe",request,{account,{{120,120,120,120},1024,32768,131072,32768},missing,{1048576,1048576,1048576,1048576,1048576,1048576,1048576}},
    {{0,1048576,1048576,2097152,1048576,1048576,4194304},2048,1024,8192,128},{1048576,1048576,1048576,1048576,1048576,16384,1048576},131072,1048576,memory_detail::digest,{}};}
inline Result<PrepareResult> prepare_text(const std::string& text,Registry& registry,SourceResolver& resolver,const PrepareOptions& options,std::shared_ptr<ResultStore> store={}){auto parsed=parse_xml(text,{1048576,32,16384,1024,options.source_services.account});if(auto* e=std::get_if<Diagnostic>(&parsed)){return *e;}auto composition=resolve_composition(std::get<ParsedComposition>(parsed),registry);if(auto* e=std::get_if<Diagnostic>(&composition)){return *e;}return prepare(std::get<Composition>(composition),registry,resolver,std::move(store),options);}
struct BarInput {std::uint64_t end=4;std::vector<std::pair<std::uint64_t,std::int64_t>> values{{0,100},{2,110},{3,120}};std::vector<SourceGap> gaps{{{"c",1,2},{10,20},7}};bool corrupt=false;};
struct BarData {Reservation charge;SourceMetadata metadata;std::vector<TimingDescriptor> timeline;std::vector<Bar> rows;};
inline SourceRequest bar_request(std::uint64_t end){return {"test.bars","explicit-bars","v1",quant::lexongine::bar_contract(),{"c",0,end},{1,{}}};}
inline void register_bar_source(Registry& registry,SourceResolver& resolver,RegisteredType<Bar> type,std::shared_ptr<BarInput> input,const ImplementationIdentity& identity,MissingIntervalPolicy missing){
    TypedSourceProvider<Bar,std::shared_ptr<const BarInput>,BarData,std::size_t> provider{"test.bars",type,
        [input](const SourceRequest& request,SourceValidationServices&)->Result<std::shared_ptr<const BarInput>>{if(input->values.size()>16||input->gaps.size()>16||input->end>120){return diagnostic(FailureCode::ResourcePlanExceeded,"small Bar probe capacity exceeded");}if(request.required_scope.clock!="c"||request.required_scope.begin!=0||request.required_scope.end==0||request.required_scope.end>input->end||request.requested_version!="v1"){return diagnostic(FailureCode::SourceChanged,"test Bar binding changed");}return std::shared_ptr<const BarInput>{input};},
        [](const auto& data)->Result<SourceBounds>{return SourceBounds{{"c",0,data->end},{65536,1024,65536,8192,4096,65536,0}};},
        [identity](auto&& input,const SourceRequest& request,SourceValidationServices& services)->Result<BarData>{return memory_detail::protect([&]()->Result<BarData>{
            if(input->corrupt){return diagnostic(FailureCode::SourceInvalid,"invalid present Bar is not a declared gap");}
            auto charge=memory_detail::checked(services.account->reserve(ResourceKind::Source,65536));
            SourceCoverage coverage{request.required_scope,{},{},protocol("test.expected_intervals")};std::vector<Bar> rows;std::vector<TimingDescriptor> timeline;rows.reserve(input->values.size());timeline.reserve(input->values.size()+input->gaps.size());std::string fingerprint="explicit-bars-v1";
            for(const auto& [key,raw]:input->values){auto bar=memory_detail::checked(fixture::complete_bar(key));bar.interval_start=quant::data::Timestamp::from_unix_nanos(static_cast<std::int64_t>(key*10));bar.interval_end=quant::data::Timestamp::from_unix_nanos(static_cast<std::int64_t>((key+1)*10));bar.header.event_time=bar.interval_end;bar.open=bar.high=bar.low=bar.close=Price::from_raw(raw);
                if(key<request.required_scope.end){coverage.complete_keys.push_back({"c",key});timeline.push_back({{"c",key},Interval{static_cast<std::int64_t>(key*10),static_cast<std::int64_t>((key+1)*10)},static_cast<std::int64_t>((key+1)*10),CompleteRecord{rows.size()}});}rows.push_back(bar);fingerprint+="|"+std::to_string(key)+":"+std::to_string(raw);}
            for(const auto& gap:input->gaps){fingerprint+="|gap:"+std::to_string(gap.missing_scope.begin)+":"+std::to_string(gap.missing_scope.end)+":"+std::to_string(gap.declared_reason);if(gap.missing_scope.begin<request.required_scope.end){auto selected=gap;if(selected.missing_scope.end>request.required_scope.end){selected.missing_scope.end=request.required_scope.end;selected.interval.end_ns=static_cast<std::int64_t>(request.required_scope.end*10);}coverage.gaps.push_back(selected);timeline.push_back({{selected.missing_scope.clock,selected.missing_scope.begin},selected.interval,selected.interval.end_ns,DeclaredGap{selected}});}}
            std::sort(timeline.begin(),timeline.end(),[](const auto& a,const auto& b){return a.key.sequence<b.key.sequence;}); // Explicit synthetic keys, never provider timestamp sorting.
            const auto digest=memory_detail::digest_text(fingerprint);
            SourceMetadata metadata{"explicit-bars","v1",digest,quant::lexongine::bar_contract(),{{},typeid(Bar)},request.required_scope,std::move(coverage),protocol("interval_v1"),protocol("test.original_keys"),protocol("test.interval_end"),protocol("test.validated_bars"),digest,identity};
            return BarData{std::move(charge),std::move(metadata),std::move(timeline),std::move(rows)};
        });},
        [](const auto& data)->Result<SourceMetadata>{return data.metadata;},[](const auto& data)->Result<std::span<const TimingDescriptor>>{return std::span<const TimingDescriptor>{data.timeline};},
        [](const auto&,SourceCursorServices&)->Result<std::size_t>{return std::size_t{0};},
        [](std::size_t& position,const BarData& data,const UpdateContext&,PublicationAllocator& allocator)->Result<SourceDelivery<Bar>>{const auto& timing=data.timeline.at(position++);if(const auto* gap=std::get_if<DeclaredGap>(&timing.delivery)){return SourceDelivery<Bar>{gap->gap};}auto value=allocator.copy_output(0,data.rows.at(std::get<CompleteRecord>(timing.delivery).payload_index));if(auto* e=std::get_if<Diagnostic>(&value)){return *e;}return SourceDelivery<Bar>{SourceRecord<Bar>{timing,std::get<Immutable<Bar>>(std::move(value))}};},
        [](std::size_t& position,const BarData& data)->Result<SourceEnd>{if(position!=data.timeline.size()){return diagnostic(FailureCode::SourceCoverage,"test source incomplete");}return SourceEnd{data.metadata.validated_scope};},{}};
    success(resolver.register_provider(std::move(provider)));
    OperationContract description{{"test","bars",1,"builtin-v1"},identity,{1,{}},{},{{"bar",type.policy.contract,StorageKind::ImmutableOwned,protocol("unsupported"),protocol("test.validated_bars"),false}},
        {SharingClass::StatefulDeclared,protocol("source_cursor"),protocol("same_step"),protocol("none"),true,true,false,{"integration-source-probe"}},{"interval_v1"},{Alignment::SameStep},{missing},{"count"},"Explicit test-only complete/gap Bar source"};
    success(registry.register_definition(std::move(description),{},[](const ParamSet&,const PreparationFacts&)->Result<PreparedKernel>{return diagnostic(FailureCode::UnsupportedContract,"source service only");}));
}
struct SeenValue {std::string request;UpdateKey key;std::int64_t raw,available;std::vector<ParentReference> parents;friend bool operator==(const SeenValue&,const SeenValue&)=default;};
struct Evidence {std::vector<SeenValue> values;std::vector<std::pair<std::uint32_t,OutcomeKind>> outcomes;std::uint64_t gaps=0;bool closed=false;};
inline RunServices services(const PreparedPlan& plan,const RegisteredType<Price>& type,std::shared_ptr<Evidence> evidence,Sha256 host=Sha256{std::byte{9}}){
    evidence->values.reserve(512);evidence->outcomes.reserve(2048);std::vector<ObservationAdapter> observers;
    for(const auto& request:plan.observations()){auto observer=make_observer<Price>(request.request.id,type,[evidence](const auto& value,const RequestContext& request)->Result<void>{evidence->values.push_back({request.request,request.key,value.value.raw(),value.parent().metadata().available_at_ns,{}});return ok();});
        auto callback=std::move(observer.deliver);observer.deliver=[callback=std::move(callback),evidence](const auto& request,const auto& slot,auto port,const auto& context)->Result<void>{auto result=callback(request,slot,port,context);if(auto* e=std::get_if<Diagnostic>(&result)){return *e;}for(const auto& parent:slot.current()->parents.entries()){evidence->values.back().parents.push_back({*parent.input_port(),parent.metadata()});}return ok();};observers.push_back(std::move(observer));}
    ExecutionEvidence retention{{},plan.evidence_specification(),[]()->Result<void>{return ok();},[](const auto&)->Result<void>{return ok();},[evidence](const auto&)->Result<void>{evidence->closed=true;return ok();}};
    return {std::move(observers),std::move(retention),{IntegrationOutcome::Completed,{},"test.toy_host",1,host},{}};
}
inline IntegrationCompletion completion(Sha256 host=Sha256{std::byte{9}}){return {IntegrationOutcome::Completed,{},"test.toy_host",1,host};}
}
