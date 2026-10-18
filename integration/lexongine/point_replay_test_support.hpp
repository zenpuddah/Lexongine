#pragma once
#include "integration_test_support.hpp"
#include "qaedalus_replay_source.hpp"

namespace lexongine::integration_test {
using Trade=quant::data::Trade;
inline quant::data::Timestamp time(std::int64_t ns){return quant::data::Timestamp::from_unix_nanos(ns);}
inline Trade trade(std::int64_t event,std::int64_t raw){using namespace quant::data;return {{InstrumentId{42},VenueId{7},time(event),std::nullopt,std::uint64_t{7},std::nullopt,0,SourceId{9}},Price::from_raw(raw),Quantity::from_raw(1000000),std::nullopt};}
class CheckedReader final:public quant::storage::MarketDataReader {
public:
    std::vector<quant::integrity::BoundObservation> rows{{quant::data::StreamId{1},10,trade(100,7)},{quant::data::StreamId{1},12,trade(100,8)},{quant::data::StreamId{1},13,trade(95,9)}};
    std::size_t split=0;bool fail_after=false,unsupported=false;mutable std::size_t checked_calls=0,unchecked_calls=0;
    arrow::Result<quant::storage::DatasetLocalMetadata> read_metadata(const quant::storage::DataFragment& f) const override{return quant::storage::DatasetLocalMetadata{f.data_kind,f.stream,f.time_range,quant::ingestion::AcquisitionMode::Historical,f.schema_version,"test-adapter-v1","test-mapping-v1",{},{},std::nullopt};}
    arrow::Status read_batches(const quant::storage::DataFragment&,quant::storage::BatchProjection,const quant::storage::BatchConsumer&) const override{++unchecked_calls;return arrow::Status::Invalid("unchecked forbidden");}
    arrow::Status stream_events(const quant::storage::DataFragment&,const quant::storage::EventConsumer&) const override{++unchecked_calls;return arrow::Status::Invalid("unchecked forbidden");}
    arrow::Status stream_ordered_events(std::span<const quant::storage::DataFragment>,quant::integrity::ConsumerRequirements requirements,const quant::storage::SequencedEventConsumer& consume) const override{
        ++checked_calls;if(unsupported){return arrow::Status::NotImplemented("checked capability absent");}if(!requirements.ordering||!requirements.groups||requirements.initial_state||requirements.allow_degraded||requirements.explicit_partial){return arrow::Status::Invalid("required eligibility flags changed");}
        for(const auto bounds:{std::pair{std::size_t{0},split},std::pair{split,rows.size()}}){for(auto i=bounds.first;i<bounds.second;++i){auto ephemeral=rows[i];ARROW_RETURN_NOT_OK(consume(ephemeral));ephemeral.event=trade(199,999);}}
        return fail_after?arrow::Status::Invalid("unresolved required ordering at close"):arrow::Status::OK();
    }
};
inline qaedalus::ReplaySourceBinding trade_binding(){
    quant::ingestion::MarketDataQuery query{quant::data::InstrumentId{42},quant::data::VenueId{7},quant::ingestion::MarketDataLevel::L3,quant::ingestion::TimeRange{time(90),time(200)},std::nullopt,quant::ingestion::TimeBasis::EventTime,quant::ingestion::FetchPolicy::DeriveProviderRange,std::nullopt};
    quant::storage::DataFragment fragment{quant::storage::DataKind::Trades,{quant::data::InstrumentId{42},quant::data::VenueId{7},quant::data::SourceId{9}},{time(90),time(200)},"quant-observations-v2","/test/trades","v1","acquisition-1","/test/package",quant::data::StreamId{1},std::string(64,'a')};
    return {query,{fragment},{true,true,false,false,false},qaedalus::ReplayDeliveryMode::OrderedEvent,quant::storage::DataKind::Trades,"checked-trades",{"c",0,3},protocol("qaedalus.ordinal_availability_table"),{{10,110},{12,111},{13,112}},MissingIntervalPolicy::Strict,{16,65536,65536,65536},{"test","trades",1,"builtin-v1"},build(),{}};
}
inline Result<Price> trade_price(const Trade& value,const CanonicalParams& p,const UpdateContext& context){const auto* point=std::get_if<PointTime>(&context.extent);if(!point||point->event_time_ns!=value.header.event_time.unix_nanos()||!p.values.empty()||context.activation_available_at_ns<point->event_time_ns){return diagnostic(FailureCode::SourceInvalid,"Trade projection must preserve point timing and causal availability");}return value.price;}
inline void register_point_operations(Registry& registry,RegisteredType<Trade> trade_type,RegisteredType<Price> price,const ImplementationIdentity& identity){
    OperationContract source{{"test","trades",1,"builtin-v1"},identity,{1,{}},{},{{"trade",trade_type.policy.contract,StorageKind::ImmutableOwned,protocol("unsupported"),protocol("qaedalus.trade"),false}},
        {SharingClass::StatefulDeclared,protocol("source_cursor"),protocol("same_step"),protocol("none"),true,true,false,{"checked-replay-integration"}},{"point_v1"},{Alignment::SameStep},{MissingIntervalPolicy::Strict},{"count"},"Explicit test registration of the checked Trade source"};
    success(registry.register_definition(std::move(source),{},[](const ParamSet&,const PreparationFacts&)->Result<PreparedKernel>{return diagnostic(FailureCode::UnsupportedContract,"source service only");}));
    OperationContract description{{"test","trade_price",1,"builtin-v1"},identity,{1,{}},{{"trade",trade_type.policy.contract,Alignment::SameStep,MissingIntervalPolicy::Strict,AbsencePolicy::RequireValue}},{{"price",price.policy.contract,StorageKind::ScalarCopy,protocol("qaedalus.price9_xml"),protocol("qaedalus.price9"),true}},
        {SharingClass::StatelessDeterministic,protocol("stateless"),protocol("same_step"),protocol("none"),true,true,true,{"checked-trade-price-projection"}},{"point_v1"},{Alignment::SameStep},{MissingIntervalPolicy::Strict},{"count"},"Test-only canonical Trade price projection; no Bar construction"};
    TypedOperation<std::monostate,std::monostate,std::tuple<Trade>,std::tuple<Price>> operation{description,{{"trade",trade_type,0}},{{"price",price}},
        [](const ParamSet& p)->Result<std::monostate>{if(!p.values.empty()){return diagnostic(FailureCode::InvalidConfig,"no projection parameters");}return std::monostate{};},
        [](const std::monostate&)->Result<CanonicalParams>{return CanonicalParams{1,{}};},[](const std::monostate&,const PreparationFacts&)->Result<HistoryRequirement>{return HistoryRequirement{CountHistory{0}};},
        [](const std::monostate&,const PreparationFacts&)->Result<ResourceBounds>{return ResourceBounds{0,0,0,8192,4096,8192,0};},[](const std::monostate&,const InitializationSpec&,StateAllocator&)->Result<std::monostate>{return std::monostate{};},
        [](std::monostate&,const std::monostate&,const std::tuple<ReadValue<Trade>>& values,const UpdateContext& context,PublicationAllocator& allocator)->StepResult<std::tuple<Price>>{const auto& input=std::get<0>(values);auto price=trade_price(input.value,{1,{}},context);if(auto* e=std::get_if<Diagnostic>(&price)){return *e;}auto parents=allocator.parents({input.parent()});if(auto* e=std::get_if<Diagnostic>(&parents)){return *e;}return Published{std::tuple{std::get<Price>(price)},std::get<ParentSelections>(std::move(parents))};},
        [](const std::monostate&,const std::tuple<Price>&,const UpdateContext&)->Result<void>{return ok();},{}};
    success(registry.register_definition(std::move(operation)));success(registry.register_source_projection<Trade,Price>({description.key,identity,protocol("test.trade_price_projection")},trade_type,price,trade_price,{}));
}
struct DirectTradeData {Reservation charge;SourceMetadata metadata;std::vector<TimingDescriptor> timeline;std::vector<Trade> rows;};
inline void register_direct_trade_source(SourceResolver& resolver,RegisteredType<Trade> type,std::shared_ptr<const DirectTradeData> data){
    using Data=std::shared_ptr<const DirectTradeData>;
    TypedSourceProvider<Trade,Data,Data,std::size_t> provider{"direct-trades",type,[data](const SourceRequest&,SourceValidationServices&)->Result<Data>{return data;},
        [](const Data& data)->Result<SourceBounds>{return SourceBounds{data->metadata.validated_scope,{65536,1024,65536,8192,4096,65536,0}};},
        [](Data&& data,const SourceRequest&,SourceValidationServices&)->Result<Data>{for(const auto& row:data->rows){if(!quant::data::validate(row).empty()){return diagnostic(FailureCode::SourceInvalid,"invalid direct Trade");}}return std::move(data);},
        [](const Data& data)->Result<SourceMetadata>{return data->metadata;},[](const Data& data)->Result<std::span<const TimingDescriptor>>{return std::span<const TimingDescriptor>{data->timeline};},[](const Data&,SourceCursorServices&)->Result<std::size_t>{return std::size_t{0};},
        [](std::size_t& i,const Data& data,const UpdateContext&,PublicationAllocator& allocator)->Result<SourceDelivery<Trade>>{auto value=allocator.copy_output(0,data->rows.at(i));if(auto* e=std::get_if<Diagnostic>(&value)){return *e;}auto timing=data->timeline.at(i++);return SourceDelivery<Trade>{SourceRecord<Trade>{timing,std::get<Immutable<Trade>>(std::move(value))}};},
        [](std::size_t& i,const Data& data)->Result<SourceEnd>{if(i!=data->rows.size()){return diagnostic(FailureCode::SourceCoverage,"direct Trade cursor incomplete");}return SourceEnd{data->metadata.validated_scope};},{}};
    success(resolver.register_provider(std::move(provider)));
}
struct PointEnvironment {
    std::shared_ptr<ResourceAccount> account=resources();Registry registry=take(Registry::create());qaedalus::ReplaySourceBinding binding;
    quant::lexongine::ComponentTypes types;RegisteredType<Trade> trade_type;SourceResolver resolver;SourceRequest request;
    PointEnvironment(std::shared_ptr<const quant::storage::MarketDataReader> reader,qaedalus::ReplaySourceBinding value,std::shared_ptr<const DirectTradeData> direct={}):binding(std::move(value)),types(take(quant::lexongine::register_components(registry,{binding.implementation,{},MissingIntervalPolicy::Strict}))),trade_type(take(qaedalus::register_trade_type(registry,{}))),resolver(take(SourceResolver::create(account,65536,4))),request{direct?"direct-trades":"checked-trades",binding.snapshot_name,binding.selected_fragments[0].version_id,qaedalus::trade_interpretation(),binding.expected_scope,{1,{}}}{
        register_point_operations(registry,trade_type,types.price,binding.implementation);
        if(direct){register_direct_trade_source(resolver,trade_type,std::move(direct));}else{success(qaedalus::register_trade_replay_source(resolver,request.provider_name,trade_type,std::move(reader),binding,account));}
        success(registry.freeze());success(resolver.freeze());
    }
    Result<PrepareResult> prepare(std::uint64_t window=2,Driver driver=Driver::Hosted,const LocalCacheConfiguration* config=nullptr,std::shared_ptr<ResultStore> store={}){
        auto nodes=node("price","test.trade_price","stateless",input("trade","s","trade","strict"))+sma("mean","price",window,"strict");
        auto text=xml(nodes,observe("mean","mean","mean",window-1,binding.expected_scope.end,"strict"),binding.expected_scope.end,MissingIntervalPolicy::Strict,driver,true,config,"test.trades",binding.selected_fragments[0].version_id);
        return prepare_text(text,registry,resolver,options(request,account,MissingIntervalPolicy::Strict),std::move(store));
    }
};
inline std::pair<RunReport,std::shared_ptr<Evidence>> execute_point(std::shared_ptr<const PreparedPlan> plan,const RegisteredType<Price>& type){auto evidence=std::make_shared<Evidence>();auto run=take(instantiate(plan,services(*plan,type,evidence)));if(plan->configuration().run.driver==Driver::Standalone){return {take(execute(run)),evidence};}
    for(;;){auto receipt=take(advance_one(run));if(receipt.source){const auto* frame=receipt.source->frame();require(std::holds_alternative<PointTime>(frame->context.extent),"Trade source fabricated an interval");rejects(receipt.source->read<Trade>(0,frame->available_at_ns-1),FailureCode::FutureRead);static_cast<void>(take(receipt.source->read<Trade>(0,frame->available_at_ns)));}if(receipt.status==StepStatus::Exhausted||receipt.status==StepStatus::Stopped){break;}}
    return {take(finish(run,completion())),evidence};
}
}
