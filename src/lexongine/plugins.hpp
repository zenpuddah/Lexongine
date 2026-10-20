#pragma once

#include "lexongine/registry.hpp"

namespace lexongine {

struct PluginLimits {
    std::uint32_t probe_bytes;
    std::uint64_t file_bytes, metadata_bytes, registration_bytes, dependency_images;
};
enum class NativeDependencyKind { FileContent, AppleLoadedImage, AppleDeferredImage };
struct NativeDependency {
    std::string install_name, loaded_path;
    std::array<std::byte, 16> image_uuid;
    std::optional<Sha256> file_digest; // System shared-cache images use their actual dyld UUID.
    bool present;
    NativeDependencyKind kind;
    std::uint32_t load_flags;
};
struct HostBuildLimits {
    std::uint64_t file_bytes, metadata_bytes, dependency_images;
};
class HostBuild {
  public:
    [[nodiscard]] const ImplementationIdentity &identity() const noexcept;
    [[nodiscard]] std::span<const NativeDependency> dependencies() const noexcept;
    [[nodiscard]] std::string_view path() const noexcept;
    [[nodiscard]] std::string_view platform_identity() const noexcept;

  private:
    struct Data;
    explicit HostBuild(std::shared_ptr<const Data> data) noexcept : data_(std::move(data)) {}
    friend Result<HostBuild> inspect_host_build(std::string_view, std::string_view,
                                                std::shared_ptr<ResourceAccount>,
                                                const HostBuildLimits &) noexcept;
    std::shared_ptr<const Data> data_;
};
[[nodiscard]] Result<HostBuild> inspect_host_build(std::string_view expected_build_contract,
                                                   std::string_view expected_source_build,
                                                   std::shared_ptr<ResourceAccount>,
                                                   const HostBuildLimits &) noexcept;
class LoadedPlugin {
  public:
    [[nodiscard]] const ImplementationIdentity &identity() const noexcept;
    [[nodiscard]] const ModulePin &pin() const noexcept;
    [[nodiscard]] std::span<const NativeDependency> dependencies() const noexcept;
    [[nodiscard]] std::string_view path() const noexcept;
    [[nodiscard]] std::string_view platform_identity() const noexcept;

  private:
    struct Data;
    explicit LoadedPlugin(std::shared_ptr<const Data> data) noexcept : data_(std::move(data)) {}
    friend Result<LoadedPlugin> load_plugin(std::string_view, std::string_view, Registry &,
                                            std::shared_ptr<ResourceAccount>,
                                            const PluginLimits &) noexcept;
    std::shared_ptr<const Data> data_;
};
// The module file and same-build manifest must be immutable for its loaded lifetime.
// Loading is trusted native initialization, not sandboxing or a stable foreign ABI.
Result<LoadedPlugin> load_plugin(std::string_view absolute_path,
                                 std::string_view expected_build_contract, Registry &,
                                 std::shared_ptr<ResourceAccount>, const PluginLimits &) noexcept;

} // namespace lexongine
