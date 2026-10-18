#include "file_result_store.hpp"
#include "native_files.hpp"
#include <set>
namespace lexongine {
namespace {
using namespace wire;
struct Rows {
    Reservation charge;
    ArtifactIdentity identity;
    SourceCoverage coverage;
    InitializationSpec initialization;
    CountHistory history;
    std::vector<OutputContract> outputs;
    std::vector<PriceOutcomeRecord> records;
    std::uint64_t parents;
    bool sealed=false;
};
struct Candidate {OwnedMetadata<ArtifactManifest> manifest;files::Bytes payload;ArtifactOrigin origin;};
std::uint64_t parent_bound(const PreparedPlan& plan,std::uint32_t logical){auto local=std::get<CountHistory>(plan.nodes()[logical].facts.history).preceding;std::uint64_t n=0;for(const auto& i:plan.nodes()[logical].spec.inputs){n=checked(checked_add(n,checked(checked_add(std::max(local,i.prior_outputs),1))));}return n;}
std::uint64_t row_bytes(const PreparedPlan& plan,std::uint32_t logical){auto record=checked(checked_add(sizeof(PriceOutcomeRecord)+512,checked(checked_add(checked(checked_multiply(plan.outputs(logical).size(),8)),checked(checked_multiply(parent_bound(plan,logical),sizeof(ParentReference)+plan.configuration().run.clock.size()+64))))));return checked(checked_add(65536,checked(checked_multiply(plan.source().metadata().coverage.complete_keys.size(),record))));}
bool suffix(std::string_view name,std::string_view ending){return name.size()==32+ending.size()&&name.ends_with(ending)&&safe_identifier(name.substr(0,32));}
}
struct FileResultStore::State {
    std::shared_ptr<files::DirectoryLock> lock;
    LocalCacheConfiguration config;
    RegisteredType<quant::data::Price> price;
    HostBuild build;
    std::shared_ptr<ResourceAccount> account;
    FileStoreLimits limits;
    Sha256 build_digest;
    std::function<Result<void>(StoreFaultPoint)> fault;
    std::vector<std::weak_ptr<Rows>> writers;
    std::optional<std::pair<std::string,Sha256>> publishing;
    Result<void> inject(StoreFaultPoint p){return fault?fault(p):ok();}
    std::filesystem::path path(std::string_view id,std::string_view end)const{return lock->path()/(std::string{id}+std::string{end});}
    std::pair<std::uint64_t,std::uint64_t> usage(){auto inventory=checked(lock->inventory(limits.max_files));std::uint64_t bytes=0;std::set<std::string> entries;
        for(const auto& [name,size]:inventory){bytes=checked(checked_add(bytes,size));if(suffix(name,".manifest.xml")||suffix(name,".price.xml")){entries.insert(name.substr(0,32));}}return {entries.size(),bytes};}
    void verify_origin(const AcceptedRunRecord& marker,const SnapshotSummary& snapshot){
        if(marker.build_identity_digest!=build_digest||snapshot.run_id!=marker.run_id||snapshot.numerical!="completed"||snapshot.integration!="completed"||snapshot.acceptance!="pending"||snapshot.cache_status!="staged"||snapshot.commit_id!=marker.commit_id||snapshot.entries!=marker.entries){fail("accepted origin and snapshot disagree");}
        if(checked(acceptance_digest(snapshot.required,account,limits.metadata))!=marker.acceptance_policy_digest){fail("accepted evidence policy digest mismatch");}
        std::vector<EvidenceReference> embedded;std::size_t snapshots=0;
        for(const auto& spec:snapshot.required){auto ref=std::find_if(marker.inventory.begin(),marker.inventory.end(),[&](const auto& e){return e.name==spec.name;});
            if(ref==marker.inventory.end()||ref->absolute_path!=spec.absolute_path||ref->byte_count>spec.max_bytes||spec.open_policy!="create_new"||spec.failure!="stop"){fail("origin is missing declared required evidence");}
            if(spec.format=="run_snapshot_v1"){++snapshots;if(ref->absolute_path!=marker.snapshot.absolute_path||ref->digest!=marker.snapshot.digest||ref->byte_count!=marker.snapshot.byte_count){fail("origin snapshot reference mismatch");}}
            else{embedded.push_back(*ref);}
            checked(files::verify(*ref,account,std::min(spec.max_bytes,limits.max_evidence_bytes)));
        }
        if(snapshots!=1||marker.inventory.size()!=snapshot.required.size()||embedded!=snapshot.inventory){fail("origin inventory contains omitted, extra or self-referencing evidence");}
    }
    std::shared_ptr<Candidate> candidate(const ArtifactIdentity& expected){
        auto inventory=checked(lock->inventory(limits.max_files));
        for(const auto& [filename,size]:inventory){static_cast<void>(size);if(!suffix(filename,".accepted.xml")){continue;}
            auto marker_bytes=checked(files::read(lock->path()/filename,account,limits.metadata.xml.bytes));auto marker=checked(decode_accepted_run(marker_bytes.value,account,limits.metadata));
            auto selected=std::find_if(marker.value.entries.begin(),marker.value.entries.end(),[&](const auto& e){return e.identity_digest==expected.digest();});if(selected==marker.value.entries.end()){continue;}
            if(marker.value.commit_id!=filename.substr(0,32)){fail("accepted marker filename does not match commit identity");}
            auto snapshot_bytes=checked(files::read(marker.value.snapshot.absolute_path,account,limits.max_evidence_bytes));
            if(snapshot_bytes.value.size()!=marker.value.snapshot.byte_count||checked(files::digest(snapshot_bytes.value))!=marker.value.snapshot.digest){fail("origin snapshot integrity mismatch");}
            auto snapshot=checked(decode_snapshot(snapshot_bytes.value,account,limits.snapshot_metadata.value_or(limits.metadata)));verify_origin(marker.value,snapshot.value);
            std::shared_ptr<Candidate> result;
            for(const auto& ref:marker.value.entries){auto manifest_bytes=checked(files::read(path(ref.artifact_id,".manifest.xml"),account,config.max_entry_bytes));
                if(manifest_bytes.value.size()!=ref.manifest_bytes||checked(files::digest(manifest_bytes.value))!=ref.manifest_digest){fail("accepted manifest integrity mismatch");}
                auto manifest=checked(decode_manifest(manifest_bytes.value,account,limits.metadata));const auto& m=manifest.value;
                if(m.artifact_id!=ref.artifact_id||m.origin_run_id!=marker.value.run_id||m.identity_digest!=ref.identity_digest||m.payload_bytes!=ref.payload_bytes||m.payload_digest!=ref.payload_digest){fail("accepted entry references do not match manifest");}
                const auto artifact_body=artifact_computation(m);
                bool member=false;for(const auto& computation:snapshot.value.computations){if(std::ranges::equal(computation_body(computation),artifact_body)){member=true;break;}}
                if(!member){fail("artifact computation does not belong to originating plan");}
                auto payload=checked(files::read(path(ref.artifact_id,".price.xml"),account,config.max_entry_bytes));if(payload.value.size()!=ref.payload_bytes||checked(files::digest(payload.value))!=ref.payload_digest){fail("accepted payload integrity mismatch");}
                if(ref.artifact_id==selected->artifact_id&&std::ranges::equal(m.canonical_identity,expected.canonical_bytes())){
                    result=std::make_shared<Candidate>(Candidate{std::move(manifest),std::move(payload),{marker.value.run_id,marker.value.commit_id,checked(files::digest(marker_bytes.value)),NumericalOutcome::Completed,IntegrationOutcome::Completed,AcceptanceStatus::Accepted}});
                }
            }
            if(result){return result;}
        }
        return {};
    }
};
Result<FileResultStore> FileResultStore::create(LocalCacheConfiguration config,RegisteredType<quant::data::Price> price,HostBuild build,
    std::shared_ptr<ResourceAccount> account,FileStoreLimits limits,std::function<Result<void>(StoreFaultPoint)> fault) noexcept{return wire::protect([&]()->Result<FileResultStore>{
    if(!account||limits.max_files==0||limits.directory_metadata_bytes<8192||limits.max_files>(limits.directory_metadata_bytes-8192)/1024||limits.metadata.xml.bytes>config.max_store_bytes||limits.payload.xml_bytes<config.max_entry_bytes||limits.payload.decoded_bytes<config.max_decoded_bytes){return diagnostic(FailureCode::InvalidConfig,"filesystem store limits do not cover declared contracts");}
    auto lock=checked(files::DirectoryLock::acquire(config.directory,account,limits.directory_metadata_bytes));auto build_hash=checked(implementation_digest(build.identity(),account,limits.metadata));
    auto state=std::make_shared<State>(State{std::move(lock),config,std::move(price),std::move(build),account,limits,build_hash,std::move(fault),{},{}});
    state->writers.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(4096,limits.service.metadata_bytes)/sizeof(std::weak_ptr<Rows>)));
    ResultStoreCallbacks cb;cb.codec={"qaedalus.price9_xml",1,{1,{}}};cb.validation={"lexongine.count_ready_price",1,{1,{}}};
    cb.supports=[state](const PreparedPlan& plan,std::uint32_t logical)->Result<bool>{
        const auto& outputs=plan.nodes()[logical].definition->contract().outputs;for(std::size_t i=0;i<outputs.size();++i){if(outputs[i].type!=state->price.policy.contract||outputs[i].codec!=Contract{"qaedalus.price9_xml",1,{1,{}}}||outputs[i].validation!=Contract{"qaedalus.price9",1,{1,{}}}||plan.outputs(logical)[i].witness!=typeid(quant::data::Price)){return false;}}return !outputs.empty();};
    cb.staging_resources=[state](const PreparedPlan& plan,std::uint32_t logical)->Result<ResourceBounds>{return protect([&]()->Result<ResourceBounds>{return ResourceBounds{0,0,checked(checked_add(state->limits.metadata.xml.workspace_bytes,checked(checked_multiply(state->config.max_entry_bytes,4)))),checked(checked_add(state->limits.metadata.xml.bytes,checked(checked_multiply(state->config.max_entry_bytes,2)))),0,checked(checked_add(row_bytes(plan,logical),state->limits.metadata.owned_bytes)),state->limits.service.metadata_bytes};});};
    cb.lookup=[state](const ArtifactIdentity& expected)->Result<std::variant<CacheMiss,ArtifactInspection>>{return protect([&]()->Result<std::variant<CacheMiss,ArtifactInspection>>{auto candidate=state->candidate(expected);if(!candidate){return std::variant<CacheMiss,ArtifactInspection>{CacheMiss{CacheMissReason::Absent}};}return std::variant<CacheMiss,ArtifactInspection>{ArtifactInspection{{},expected,std::move(candidate)}};});};
    cb.validate=[state](ArtifactInspection&& inspection,const PreparedPlan& plan,std::uint32_t logical)->Result<ArtifactPayload>{return protect([&]()->Result<ArtifactPayload>{
        auto candidate=std::static_pointer_cast<const Candidate>(inspection.storage);const auto& m=candidate->manifest.value;
        if(!std::ranges::equal(m.canonical_identity,inspection.identity.canonical_bytes())||m.processing_scope!=inspection.identity.processing_scope()||!std::ranges::equal(m.output_types,inspection.identity.output_types())||m.codec!=inspection.identity.codec()||m.validation!=inspection.identity.validation()){fail("manifest differs from requested complete artifact identity");}
        const PricePayloadContract contract{m.processing_scope,plan.nodes()[logical].definition->contract().outputs,m.coverage.complete_keys};const auto before=state->account->current(ResourceKind::Graph);
        auto decoded=checked(decode_price_payload(candidate->payload.value,contract,state->account,state->limits.payload));const auto bytes=state->account->current(ResourceKind::Graph)-before;
        auto frozen=std::make_shared<const DecodedPricePayload>(std::move(decoded));const auto price=state->price;const auto ports=m.output_types.size();
        ArtifactPayload result{{},frozen,m.coverage,m.initialization,HistoryRequirement{m.history},candidate->origin,frozen->outcomes().size(),bytes,{}, {}};
        result.record=[frozen](std::uint64_t i)->Result<ArtifactRecordView>{if(i>=frozen->outcomes().size()){return diagnostic(FailureCode::CacheCorrupt,"artifact record index out of range");}const auto& r=frozen->outcomes()[static_cast<std::size_t>(i)];return ArtifactRecordView{r.key,r.extent,r.available_at_ns,r.kind,r.remaining_preceding,r.declared_reason,r.parents};};
        result.materialize=[frozen,price,ports](std::uint64_t i,const UpdateContext& context,std::shared_ptr<ResourceAccount> resources)->Result<PendingOutcome>{return protect([&]()->Result<PendingOutcome>{if(i>=frozen->outcomes().size()){fail("artifact materialization index out of range");}auto pending=checked(PendingOutcome::create(resources,context,ports));PublicationAllocator allocator{resources,0};const auto& row=frozen->outcomes()[static_cast<std::size_t>(i)];for(std::size_t p=0;p<row.values.size();++p){auto value=checked(allocator.freeze(row.values[p],price));checked(pending.set_output(static_cast<PortIndex>(p),checked(ErasedValue::from(std::move(value),price.policy.contract))));}return pending;});};return result;
    });};
    cb.stage=[state](const ArtifactIdentity& identity,const PreparedPlan& plan,std::uint32_t logical)->Result<ArtifactWriter>{return protect([&]()->Result<ArtifactWriter>{
        if(std::none_of(plan.required_evidence().begin(),plan.required_evidence().end(),[](const auto& e){return e.format=="run_snapshot_v1";})){return diagnostic(FailureCode::InvalidConfig,"native publication requires a frozen run snapshot sink");}
        std::erase_if(state->writers,[](const auto& writer){return writer.expired();});
        if(state->writers.size()==state->writers.capacity()){return diagnostic(FailureCode::ResourcePlanExceeded,"live private writer tracking exceeds metadata bound");}
        auto charge=checked(state->account->reserve(ResourceKind::Graph,row_bytes(plan,logical)));auto rows=std::make_shared<Rows>(Rows{std::move(charge),identity,plan.source().metadata().coverage,plan.nodes()[logical].spec.initialization,std::get<CountHistory>(plan.nodes()[logical].facts.history),plan.nodes()[logical].definition->contract().outputs,{},parent_bound(plan,logical)});rows->records.reserve(rows->coverage.complete_keys.size());state->writers.push_back(rows);
        return ArtifactWriter{{},rows,[rows](const OutcomeFrame& frame)->Result<void>{return protect([&]()->Result<void>{if(rows->sealed||rows->records.size()==rows->coverage.complete_keys.size()||frame.parents.entries().size()>rows->parents){return diagnostic(FailureCode::CacheIO,"private file-store writer exceeded its admitted stream");}
            PriceOutcomeRecord record{frame.context.key,frame.context.extent,frame.available_at_ns,frame.kind,frame.remaining_preceding,frame.declared_reason,{},{}};record.parents.reserve(frame.parents.entries().size());record.values.reserve(rows->outputs.size());
            for(const auto& p:frame.parents.entries()){if(!p.input_port()){return diagnostic(FailureCode::OutputInvalid,"artifact parent input is unassigned");}record.parents.push_back({*p.input_port(),p.metadata()});}
            if(frame.kind==OutcomeKind::Published){for(std::size_t p=0;p<rows->outputs.size();++p){if(!frame.outputs[p]||frame.outputs[p]->semantic()!=rows->outputs[p].type){return diagnostic(FailureCode::TypeMismatch,"artifact output type mismatch");}auto value=checked(frame.outputs[p]->share<quant::data::Price>("file_store","stage_output"));record.values.push_back(value.get());}}
            rows->records.push_back(std::move(record));return ok();});}};
    });};
    cb.prepare_commit=[state](std::vector<ArtifactWriter>&& writers,const RunReport& report,const SnapshotFinalizer& finalize)->Result<StorePublication>{return protect([&]()->Result<StorePublication>{
        auto [count,used]=state->usage();auto file_count=checked(state->lock->inventory(state->limits.max_files)).size();std::vector<EntryReference> entries;entries.reserve(writers.size());std::uint64_t bypassed=0;const auto commit=files::unique_id();
        for(auto& writer:writers){auto rows=std::static_pointer_cast<Rows>(writer.storage);if(rows->sealed||rows->records.size()!=rows->coverage.complete_keys.size()){return diagnostic(FailureCode::CacheIO,"file artifact writer is incomplete or consumed");}rows->sealed=true;
            if(count>=state->config.max_entries||state->limits.max_files-file_count<3){++bypassed;continue;}const PricePayloadContract contract{rows->coverage.expected_scope,rows->outputs,rows->coverage.complete_keys};
            const auto measured=checked(measure_price_payload(rows->records,contract,state->account,state->limits.payload));if(measured>state->config.max_entry_bytes){++bypassed;continue;}
            auto payload=checked(encode_price_payload(rows->records,contract,state->account,state->limits.payload));const auto payload_hash=checked(files::digest(payload.bytes()));
            ArtifactManifest manifest{files::unique_id(),report.run_id,rows->identity.digest(),payload_hash,checked(coverage_digest(rows->coverage,state->account,state->limits.metadata)),rows->records.size(),payload.bytes().size(),rows->identity.codec(),rows->identity.validation(),{rows->identity.canonical_bytes().begin(),rows->identity.canonical_bytes().end()},rows->coverage.expected_scope,{},rows->coverage,rows->initialization,rows->history};
            manifest.output_types.assign(rows->identity.output_types().begin(),rows->identity.output_types().end());auto encoded=checked(encode_manifest(manifest,state->account,state->limits.metadata));
            const auto bytes=checked(checked_add(payload.bytes().size(),encoded.bytes().size()));const auto with_marker=checked(checked_add(bytes,state->limits.metadata.xml.bytes));
            if(bytes>state->config.max_entry_bytes||used>state->config.max_store_bytes||with_marker>state->config.max_store_bytes-used){++bypassed;continue;}
            checked(state->inject(StoreFaultPoint::PayloadWrite));auto payload_ref=checked(files::write_new("payload",state->path(manifest.artifact_id,".price.xml"),payload.bytes(),state->account,state->config.max_entry_bytes,FailureCode::CacheIO));
            checked(state->inject(StoreFaultPoint::ManifestWrite));auto manifest_ref=checked(files::write_new("manifest",state->path(manifest.artifact_id,".manifest.xml"),encoded.bytes(),state->account,state->config.max_entry_bytes,FailureCode::CacheIO));
            entries.push_back({manifest.artifact_id,manifest.identity_digest,manifest_ref.digest,manifest_ref.byte_count,payload_ref.digest,payload_ref.byte_count});used+=bytes;++count;file_count+=2;
        }
        StorePublication publication{{},entries.empty()?(bypassed?CachePublicationStatus::Bypassed:CachePublicationStatus::NoNewEntries):CachePublicationStatus::Published,entries.size(),bypassed,entries.empty()?std::string{}:commit,{},entries};
        checked(finalize(publication));if(entries.empty()){return publication;}
        auto snapshot=std::find_if(report.evidence.begin(),report.evidence.end(),[&](const auto& e){return std::any_of(report.required_evidence.begin(),report.required_evidence.end(),[&](const auto& s){return s.name==e.name&&s.format=="run_snapshot_v1";});});
        if(snapshot==report.evidence.end()){return diagnostic(FailureCode::EvidenceIO,"accepted publication requires a finalized snapshot");}
        for(const auto& e:report.evidence){checked(files::verify(e,state->account,state->limits.max_evidence_bytes));}
        AcceptedRunRecord marker{report.run_id,commit,CachePublicationStatus::Published,checked(acceptance_digest(report.required_evidence,state->account,state->limits.metadata)),state->build_digest,*snapshot,report.evidence,entries};
        auto encoded=checked(encode_accepted_run(marker,state->account,state->limits.metadata));auto temporary=state->lock->path()/("pending-"+commit+".xml");auto final=state->path(commit,".accepted.xml");
        checked(state->inject(StoreFaultPoint::MarkerWrite));auto reference=checked(files::write_new("accepted_marker",temporary,encoded.bytes(),state->account,state->limits.metadata.xml.bytes,FailureCode::CacheIO));const auto hash=reference.digest;
        publication.publish=[state,temporary=std::move(temporary),final=std::move(final),hash,commit]()->CommitOutcome{
            state->publishing=std::pair{commit,hash};
            auto before=guarded([&]{return state->inject(StoreFaultPoint::BeforePublish);});if(auto* e=std::get_if<Diagnostic>(&before)){return NotCommitted{*e};}
            auto result=files::publish(temporary,final);if(auto* e=std::get_if<Diagnostic>(&result)){return CommitUnconfirmed{*e};}
            auto after=guarded([&]{return state->inject(StoreFaultPoint::AfterPublish);});if(auto* e=std::get_if<Diagnostic>(&after)){return CommitUnconfirmed{diagnostic(FailureCode::CommitUnconfirmed,e->text())};}
            return Committed{hash};};return publication;
    });};
    cb.reconcile=[state](std::string_view commit)->Result<CommitOutcome>{return protect([&]()->Result<CommitOutcome>{checked(state->inject(StoreFaultPoint::Reconcile));if(!safe_identifier(commit)){return diagnostic(FailureCode::InvalidConfig,"invalid reconciliation identity");}
        const auto final=state->path(commit,".accepted.xml");std::error_code error;const bool exists=std::filesystem::exists(final,error);if(error){return diagnostic(FailureCode::CacheIO,"marker visibility cannot be determined");}if(!exists){return CommitOutcome{NotCommitted{diagnostic(FailureCode::CacheIO,"authoritative marker is absent")}};}
        auto bytes=checked(files::read(final,state->account,state->limits.metadata.xml.bytes));const auto marker_hash=checked(files::digest(bytes.value));
        if(!state->publishing||state->publishing->first!=commit||state->publishing->second!=marker_hash){fail("reconciled marker differs from the prepared publication");}
        auto marker=checked(decode_accepted_run(bytes.value,state->account,state->limits.metadata));if(marker.value.commit_id!=commit){fail("reconciled marker has another identity");}
        auto snapshot=checked(files::read(marker.value.snapshot.absolute_path,state->account,state->limits.max_evidence_bytes));if(checked(files::digest(snapshot.value))!=marker.value.snapshot.digest||snapshot.value.size()!=marker.value.snapshot.byte_count){fail("reconciled snapshot integrity mismatch");}
        auto summary=checked(decode_snapshot(snapshot.value,state->account,state->limits.snapshot_metadata.value_or(state->limits.metadata)));state->verify_origin(marker.value,summary.value);
        for(const auto& ref:marker.value.entries){checked(files::verify({"manifest",state->path(ref.artifact_id,".manifest.xml").string(),ref.manifest_bytes,ref.manifest_digest,true},state->account,state->config.max_entry_bytes));checked(files::verify({"payload",state->path(ref.artifact_id,".price.xml").string(),ref.payload_bytes,ref.payload_digest,true},state->account,state->config.max_entry_bytes));}
        return CommitOutcome{Committed{checked(files::digest(bytes.value))}};
    });};
    auto service=checked(ResultStore::create(std::move(config),limits.service,std::move(cb),account,static_cast<Result<Sha256>(*)(std::span<const std::byte>) noexcept>(files::digest)));
    return FileResultStore{std::move(state),std::move(service)};
});}
Result<void> FileResultStore::cleanup_unaccepted() noexcept{return wire::protect([&]()->Result<void>{
    for(const auto& writer:state_->writers){if(!writer.expired()){return diagnostic(FailureCode::CacheBusy,"cannot clean an active private writer");}}
    auto inventory=checked(state_->lock->inventory(state_->limits.max_files));std::set<std::string> accepted;
    for(const auto& [name,bytes]:inventory){static_cast<void>(bytes);if(suffix(name,".accepted.xml")){auto raw=checked(files::read(state_->lock->path()/name,state_->account,state_->limits.metadata.xml.bytes));auto marker=checked(decode_accepted_run(raw.value,state_->account,state_->limits.metadata));for(const auto& entry:marker.value.entries){accepted.insert(entry.artifact_id);}}}
    for(const auto& [name,bytes]:inventory){static_cast<void>(bytes);const bool artifact=suffix(name,".manifest.xml")||suffix(name,".price.xml");const bool provisional=name.starts_with("pending-")&&name.ends_with(".xml")&&name.size()==44&&safe_identifier(std::string_view{name}.substr(8,32));
        if((artifact&&!accepted.contains(name.substr(0,32)))||provisional){std::error_code error;if(!std::filesystem::remove(state_->lock->path()/name,error)||error){return diagnostic(FailureCode::CacheIO,"explicit provisional cleanup failed");}}}
    return ok();
});}
} // namespace lexongine
