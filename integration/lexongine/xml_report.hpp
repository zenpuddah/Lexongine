#pragma once
#include "plan_projection.hpp"

namespace lexongine {
enum class GraphView { Logical, Physical };
inline constexpr MetadataLimits cli_metadata_limits{{16777216,524288,64,4096,268435456},33554432};
inline constexpr std::uint64_t cli_capture_bytes=8388608, cli_max_parents=16384;
wire::Element* project_tool_profile(wire::Document&);
wire::Element* project_operation(wire::Document&,const OperationContract&);
[[nodiscard]] Result<wire::EncodedXml> export_registry_xml(const Registry&,const OperationKey*,std::shared_ptr<ResourceAccount>,const MetadataLimits&) noexcept;
[[nodiscard]] Result<wire::EncodedXml> export_plan_xml(const PreparedPlan&,std::shared_ptr<ResourceAccount>,const MetadataLimits&) noexcept;
[[nodiscard]] Result<wire::Document> decode_plan_xml(std::string_view,std::shared_ptr<ResourceAccount>,const MetadataLimits&) noexcept;
[[nodiscard]] Result<wire::EncodedXml> export_dot(const PreparedPlan&,GraphView,std::shared_ptr<ResourceAccount>,std::uint64_t maximum_bytes) noexcept;
[[nodiscard]] Result<wire::EncodedXml> export_report_xml(const RunReport&,const PreparedPlan&,std::shared_ptr<ResourceAccount>,const MetadataLimits&) noexcept;
[[nodiscard]] int report_exit_code(const RunReport&) noexcept;
}
