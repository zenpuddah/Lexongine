#pragma once

#include <array>
#include <string_view>

namespace lexongine {
struct SchemaDescriptor {
    std::string_view name, fields;
};
// Ordered wire fields, not C++ object layout. Shared by all later XML projections.
inline constexpr auto schema_descriptors = std::to_array<SchemaDescriptor>(
    {{"ComputationIdentityV1",
      "operation,parameters,inputs,outputs,sources,initialization,processing_scope,history,timing_"
      "policy,numerical_policy,missing_policy,sharing_contract,codec_contract,validation_contract"},
     {"OperationIdentityV1",
      "namespace,name,semantic_version,implementation_selector,binary_digest,dependencies_digest,"
      "source_build_digest,sdk_build_contract_id"},
     {"CanonicalParamsV1", "schema_version,values"},
     {"NamedParamV1", "name,value"},
     {"InputIdentityV1",
      "input_name,producer_identity,output_port,prior_outputs,alignment_contract,missing_policy"},
     {"OutputIdentityV1",
      "output_name,type_contract,storage_kind,codec_contract,validation_contract"},
     {"TypeContractV1", "nominal_name,semantic_version,meaning"},
     {"ContractV1", "id,version,parameters"},
     {"SequenceScopeV1", "clock,begin,end"},
     {"UpdateKeyV1", "clock,sequence"},
     {"InitializationV1", "contract,actual_prefix,explicit_values"},
     {"SharingV1",
      "class,init_contract,update_contract,effect_contract,effect_free,replay_safe,cacheable"},
     {"SourceIdentityV1",
      "snapshot_name,source_version,content_digest,provider_operation,interpretation,query_"
      "selection_contract,reference_digest,validated_scope,coverage,order_contract,availability_"
      "contract,validation_contract"},
     {"CoverageV1", "expected_scope,complete_keys,gaps,expected_timeline_contract"},
     {"GapV1", "missing_scope,interval,declared_reason"},
     {"IntervalV1", "begin_ns,end_ns"},
     {"PointTimeV1", "event_time_ns"},
     {"CountHistoryV1", "preceding"},
     {"TimingPolicyV1", "update_contract,availability_contract"},
     {"ArtifactIdentityV1",
      "computation,processing_scope,ordered_output_types,codec_contract,validation_contract"},
     {"PlanIdentityV1", "effective_composition,logical_order,aliases,admitted_requests,rejected_"
                        "requests,resource_plan,required_evidence"},
     {"StartupV1", "initialization,history"},
     {"EvidenceReferenceV1", "name,absolute_path,byte_count,digest,finalized"},
     {"EntryReferenceV1",
      "artifact_id,identity_digest,manifest_digest,manifest_bytes,payload_digest,payload_bytes"},
     {"SnapshotReferenceV1", "absolute_path,byte_count,digest"},
     {"RequiredEvidenceSpecV1",
      "name,format,absolute_path,open_policy,max_bytes,max_records,flush_policy,failure"},
     {"EffectiveParametersV1", "schema_version,values"},
     {"EffectiveCompositionV1",
      "run,limits,cache,plugins,sources,nodes,effects,observations,overrides"},
     {"ConfigurationOverrideV1", "target,value"},
     {"PlanRequestsV1", "admitted,rejected"},
     {"InspectionResourcesV1", "admission,tool_profile"},
     {"PlanCacheDecisionV1", "logical,eligible,decision,artifact_identity,origin"},
     {"RegistryInspectionV1", "operations,schemas,tool_profile"},
     {"SchemaInspectionV1", "name,fields,value_shapes"},
     {"ToolProfileV1",
      "id,authoring_xml_bytes,authoring_depth,authoring_elements,authoring_string_bytes,metadata_"
      "xml_bytes,metadata_depth,metadata_elements,metadata_string_bytes,metadata_workspace_bytes,"
      "decoded_metadata_bytes,snapshot_capture_bytes,max_parents"},
     {"TerminalReceiptV1",
      "run_id,numerical,integration,acceptance,cache,commit_id,record_digest,published_entries,"
      "bypassed_entries,requests,producers,complete_updates,gap_events,missing_intervals,current,"
      "peak,store_current,store_peak,cause,secondary_cause,evidence"},
     {"PreparedNodeV1", "spec,canonical_identity,needed,retention,preceding,outputs"},
     {"EffectScopeV1", "node,evaluation"},
     {"AliasV1", "logical,representative,physical"},
     {"ResourcePlanV1", "required,runtime_limits,metadata_bytes,evidence_resources"},
     {"AccountLimitsV1", "bytes,max_allocations,max_holders,max_events,retention_warning_steps"},
     {"RequestRejectionV1", "request,cause,dependencies"},
     {"SnapshotOutcomeV1",
      "logical,key,extent,available_at_ns,kind,remaining_preceding,declared_reason,parents,values"},
     {"SnapshotValueV1", "port,type,encoding,raw,source_reference"},
     {"SnapshotEventV1", "kind,outcome,gap"},
     {"RequestEvidenceV1", "request,admitted,delivered,status,cause"},
     {"ProducerStatisticsV1",
      "id,physical,advances,publications,aliases,waiting,cached_deliveries"},
     {"RunCountersV1", "complete_updates,gap_events,missing_intervals,producers,last_reach"},
     {"LogicalReachV1", "logical,reached,advanced,published,alias,outcome"},
     {"RunCacheV1", "status,commit_id,record_digest,staged_entries,published_entries,bypassed_"
                    "entries,entries,origins"},
     {"ArtifactOriginV1", "producer,run_id,commit_id,record_digest"},
     {"RunResourcesV1", "current,peak,store_current,store_peak,required_evidence,plan"},
     {"RetentionEventV1",
      "kind,allocation,bytes,age_steps,holder,reason,resource_kind,step,producer,publication"},
     {"UpdateContext", "key,extent,decision_time_ns,activation_available_at_ns"},
     {"RequestContext", "request,evaluation,phase,key"},
     {"NamedParam", "name,value,origin,provenance"},
     {"NamedLiteral", "name,text,source_location"},
     {"SourceLocation", "path,line"},
     {"ParamSet", "values"},
     {"ParameterSchema", "version,fields"},
     {"ParameterField", "name,kind,required,constraints"},
     {"ParameterConstraints", "signed_min,signed_max,unsigned_min,unsigned_max,allowed_text"},
     {"OperationKey", "namespace,name,semantic_version,implementation_selector"},
     {"ImplementationIdentity",
      "binary_digest,dependencies_digest,source_build_digest,sdk_build_contract_id"},
     {"SourceRuleContractV1", "operation,implementation,validation"},
     {"SourceProjectionRuleV1", "contract,input_type,output_type"},
     {"SourceConstraintRuleV1", "contract,input_port,input_type"},
     {"OperationContract", "key,implementation,parameters,inputs,outputs,sharing,timing_protocols,"
                           "alignment_protocols,missing_protocols,history_protocols,description"},
     {"InputContract", "name,type,alignment,missing_intervals,absence"},
     {"OutputContract", "name,type,storage,codec,validation,cacheable"},
     {"SharingContract", "classification,initialization,update,effect,effect_free,replay_safe,"
                         "cacheable,correctness_evidence"},
     {"ResourceBounds", "source,state,scratch,staging,retention,graph,evidence"},
     {"PreparationFacts",
      "processing_scope,initialization,missing_intervals,alignment,admitted_resources"},
     {"ParentPublication",
      "computation_digest,key,port,extent,available_at_ns,source_reference_digest"},
     {"ParentReference", "input_port,publication"},
     {"Published", "outputs,parents"},
     {"PublicationMetadata", "key,extent,available_at_ns,parents,output_types"},
     {"ObservationRequest",
      "missing_intervals,id,producer,output_name,evaluation,absence,prior_outputs,label"},
     {"InputBindingSpec",
      "input_name,producer,output_name,prior_outputs,alignment,missing_intervals"},
     {"NodeSpec", "id,label,ordinal,operation,parameters,inputs,initialization,update_policy,"
                  "availability_policy,effect_policy"},
     {"SourceRequest", "provider_name,snapshot_name,requested_version,interpretation,required_"
                       "scope,provider_policies"},
     {"SourceBounds", "available_scope,resources"},
     {"SourceMetadata",
      "snapshot_name,version,content_digest,interpretation,witness,validated_scope,coverage,timing,"
      "order,availability,validation,source_reference_digest,provider_implementation"},
     {"TimingDescriptor", "key,extent,available_at_ns,delivery"},
     {"CompleteRecord", "payload_index"},
     {"DeclaredGap", "gap"},
     {"SourceEnd", "completed_scope"},
     {"Warming", "remaining_preceding"},
     {"ValidAbsence", "declared_reason"},
     {"Diagnostic", "code,message,length,truncated,logical_ordinal,request_ordinal,key"},
     {"DiagnosticKey", "clock_ordinal,sequence"},
     {"IntegrationCompletion",
      "kind,cause,host_contract_id,host_contract_version,host_experiment_identity"},
     {"StepReceipt",
      "status,key,extent,available_at_ns,gap,completed_scope,sources,observations,reach,cause"},
     {"ParseLimitsV1", "xml_bytes,xml_depth,xml_elements,string_bytes"},
     {"CompositionLimitsV1",
      "xml_bytes,xml_depth,xml_elements,string_bytes,nodes,edges,requests,source_updates,managed_"
      "buffer_bytes,trace_events,trace_message_bytes"},
     {"RetentionLogConfigurationV1",
      "path,format,open,flush,failure,overflow,max_bytes,max_events"},
     {"RunConfigurationV1",
      "id,clock,driver,execution,sharing,failure,trace,retention_warning_steps,retention_log"},
     {"PluginDeclarationV1", "path,build_contract_id"},
     {"DisabledCacheV1", ""},
     {"LocalCacheConfigurationV1",
      "mode,backend,directory,matching,trust,integrity,writer,corruption,capacity,cleanup,max_"
      "entries,max_store_bytes,max_entry_bytes,max_decoded_bytes"},
     {"SourceDeclarationV1", "id,ordinal,operation,source_version,scope,completeness,empty,missing,"
                             "availability,missing_intervals,parameters,location"},
     {"NodeDeclarationV1",
      "id,ordinal,operation,alignment,initialization,parameters,inputs,location"},
     {"CompositionDeclarationV1", "run,limits,plugins,cache,sources,nodes,observations"},
     {"ResolvedParametersV1", "definition,parameters"},
     {"CompositionV1", "declaration,sources,nodes"},
     {"SourceValidationLimitsV1",
      "max_records,max_timeline_events,max_gap_entries,max_expected_updates"},
     {"SourceServiceLimitsV1", "coverage,string_bytes,candidate_bytes,snapshot_bytes,cursor_bytes"},
     {"SourceValidationServicesV1", "limits,missing_intervals,admitted_resources"},
     {"CoverageSummaryV1", "complete_updates,missing_intervals,timeline_events"},
     {"CountHistoryPlanV1", "preceding"}});
[[nodiscard]] constexpr const SchemaDescriptor *schema_descriptor(std::string_view name) noexcept {
    for (const auto &record : schema_descriptors) {
        if (record.name == name) {
            return &record;
        }
    }
    return nullptr;
}
[[nodiscard]] constexpr std::string_view schema_fields(std::string_view name) noexcept {
    const auto *descriptor = schema_descriptor(name);
    return descriptor ? descriptor->fields : std::string_view{};
}
// Exact value shapes for the implemented metadata projections. [] and ? mean a
// homogeneous list and optional; | selects a registered alternative. A record
// without a shape is not yet supported by the metadata reader.
inline constexpr auto schema_value_shapes = std::to_array<SchemaDescriptor>(
    {{"CanonicalParamsV1", "u32,[]NamedParamV1"},
     {"NamedParamV1", "text,param"},
     {"ContractV1", "text,u32,CanonicalParamsV1"},
     {"TypeContractV1", "text,u32,CanonicalParamsV1"},
     {"SequenceScopeV1", "text,u64,u64"},
     {"UpdateKeyV1", "text,u64"},
     {"IntervalV1", "i64,i64"},
     {"PointTimeV1", "i64"},
     {"CountHistoryV1", "u64"},
     {"GapV1", "SequenceScopeV1,IntervalV1,u32"},
     {"CoverageV1", "SequenceScopeV1,[]UpdateKeyV1,[]GapV1,ContractV1"},
     {"InitializationV1", "ContractV1,SequenceScopeV1,CanonicalParamsV1"},
     {"StartupV1", "InitializationV1,ContractV1"},
     {"OperationIdentityV1", "text,text,u32,text,digest,digest,digest,text"},
     {"SourceIdentityV1", "text,text,digest,OperationIdentityV1,TypeContractV1,ContractV1,digest,"
                          "SequenceScopeV1,CoverageV1,ContractV1,ContractV1,ContractV1"},
     {"EvidenceReferenceV1", "text,text,u64,digest,bool"},
     {"EntryReferenceV1", "text,digest,digest,u64,digest,u64"},
     {"SnapshotReferenceV1", "text,u64,digest"},
     {"RequiredEvidenceSpecV1", "text,text,text,text,u64,u64,text,text"},
     {"EffectiveParametersV1", "u32,[]NamedParam"},
     {"NamedParam", "text,param,text,text"},
     {"OperationKey", "text,text,u32,text"},
     {"ImplementationIdentity", "digest,digest,digest,text"},
     {"RetentionLogConfigurationV1", "text,text,text,text,text,text,u64,u64"},
     {"RunConfigurationV1", "text,text,text,text,text,text,text,u64,RetentionLogConfigurationV1"},
     {"CompositionLimitsV1", "u64,u64,u64,u64,u64,u64,u64,u64,u64,u64,u64"},
     {"DisabledCacheV1", ""},
     {"LocalCacheConfigurationV1",
      "text,text,text,text,text,text,text,text,text,text,u64,u64,u64,u64"},
     {"InputBindingSpec", "text,text,text,u64,text,text"},
     {"NodeSpec", "text,text,u32,OperationKey,EffectiveParametersV1,[]InputBindingSpec,"
                  "InitializationV1,ContractV1,ContractV1,ContractV1"},
     {"OutputContract", "text,TypeContractV1,text,ContractV1,ContractV1,bool"},
     {"PreparedNodeV1", "NodeSpec,text,bool,u64,u64,[]OutputContract"},
     {"PluginDeclarationV1", "text,text"},
     {"ObservationRequest", "text,text,text,text,SequenceScopeV1,text,u64,text"},
     {"EffectScopeV1", "text,SequenceScopeV1"},
     {"AliasV1", "u32,u32,u32"},
     {"EffectiveCompositionV1",
      "RunConfigurationV1,CompositionLimitsV1,DisabledCacheV1|LocalCacheConfigurationV1,[]"
      "PluginDeclarationV1,[]SourceIdentityV1,[]PreparedNodeV1,[]EffectScopeV1,[]"
      "ObservationRequest,[]ConfigurationOverrideV1"},
     {"ConfigurationOverrideV1", "text,text"},
     {"PlanRequestsV1", "[]ObservationRequest,[]RequestRejectionV1"},
     {"InspectionResourcesV1", "ResourcePlanV1,ToolProfileV1"},
     {"PlanCacheDecisionV1", "u32,bool,text,?digest,?ArtifactOriginV1"},
     {"RegistryInspectionV1", "[]OperationContract,[]SchemaInspectionV1,ToolProfileV1"},
     {"SchemaInspectionV1", "text,text,?text"},
     {"ToolProfileV1", "text,u64,u64,u64,u64,u64,u64,u64,u64,u64,u64,u64,u64"},
     {"OperationContract", "OperationKey,ImplementationIdentity,ParameterSchema,[]InputContract,[]"
                           "OutputContract,SharingContract,[]text,[]text,[]text,[]text,text"},
     {"ParameterSchema", "u32,[]ParameterField"},
     {"ParameterField", "text,text,bool,ParameterConstraints"},
     {"ParameterConstraints", "?i64,?i64,?u64,?u64,[]text"},
     {"InputContract", "text,TypeContractV1,text,text,text"},
     {"SharingContract", "text,ContractV1,ContractV1,ContractV1,bool,bool,bool,[]text"},
     {"TerminalReceiptV1",
      "text,text,text,text,text,text,digest,u64,u64,[]RequestEvidenceV1,[]ProducerStatisticsV1,u64,"
      "u64,u64,ResourceBounds,ResourceBounds,ResourceBounds,ResourceBounds,?Diagnostic,?Diagnostic,"
      "[]EvidenceReferenceV1"},
     {"DiagnosticKey", "u32,u64"},
     {"Diagnostic", "text,text,u64,bool,?u32,?u32,?DiagnosticKey"},
     {"RequestRejectionV1", "text,Diagnostic,[]text"},
     {"ResourceBounds", "u64,u64,u64,u64,u64,u64,u64"},
     {"AccountLimitsV1", "ResourceBounds,u64,u64,u64,u64"},
     {"ResourcePlanV1", "ResourceBounds,AccountLimitsV1,u64,ResourceBounds"},
     {"PlanIdentityV1", "EffectiveCompositionV1,[]u32,[]AliasV1,[]ObservationRequest,[]"
                        "RequestRejectionV1,ResourcePlanV1,[]RequiredEvidenceSpecV1"},
     {"ParentPublication", "digest,UpdateKeyV1,u32,extent,i64,?digest"},
     {"ParentReference", "u32,ParentPublication"},
     {"SnapshotOutcomeV1",
      "u32,UpdateKeyV1,extent,i64,text,u64,u32,[]ParentReference,[]SnapshotValueV1"},
     {"SnapshotValueV1", "u32,TypeContractV1,text,?i64,?digest"},
     {"SnapshotEventV1", "text,?SnapshotOutcomeV1,?GapV1"},
     {"RequestEvidenceV1", "ObservationRequest,bool,u64,text,?Diagnostic"},
     {"ProducerStatisticsV1", "text,u32,u64,u64,u64,u64,u64"},
     {"RunCountersV1", "u64,u64,u64,[]ProducerStatisticsV1,[]LogicalReachV1"},
     {"LogicalReachV1", "u32,bool,bool,bool,bool,text"},
     {"RunCacheV1", "text,text,digest,u64,u64,u64,[]EntryReferenceV1,[]ArtifactOriginV1"},
     {"ArtifactOriginV1", "text,text,text,digest"},
     {"RunResourcesV1", "ResourceBounds,ResourceBounds,ResourceBounds,ResourceBounds,[]"
                        "RequiredEvidenceSpecV1,PlanIdentityV1"},
     {"RetentionEventV1", "text,u64,u64,u64,text,text,u64,u64,?u32,?u64"}});
[[nodiscard]] constexpr const SchemaDescriptor *schema_value_shape(std::string_view name) noexcept {
    for (const auto &record : schema_value_shapes) {
        if (record.name == name) {
            return &record;
        }
    }
    return nullptr;
}
} // namespace lexongine
