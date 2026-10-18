#pragma once
#include "snapshot_evidence.hpp"
#include "price_artifact.hpp"
#include "lexongine/plugins.hpp"
#include "quant/lexongine/components.hpp"
namespace lexongine {
enum class StoreFaultPoint { PayloadWrite, ManifestWrite, MarkerWrite, BeforePublish, AfterPublish, Reconcile };
struct FileStoreLimits {
    StoreServiceLimits service;
    MetadataLimits metadata;
    PricePayloadLimits payload;
    std::uint64_t directory_metadata_bytes, max_files, max_evidence_bytes;
    std::optional<MetadataLimits> snapshot_metadata{};
};
class FileResultStore {
public:
    static Result<FileResultStore> create(LocalCacheConfiguration, RegisteredType<quant::data::Price>, HostBuild,
        std::shared_ptr<ResourceAccount>, FileStoreLimits,
        std::function<Result<void>(StoreFaultPoint)> fault = {}) noexcept;
    [[nodiscard]] std::shared_ptr<ResultStore> service() const noexcept { return service_; }
    [[nodiscard]] Result<void> cleanup_unaccepted() noexcept;
private:
    struct State;
    FileResultStore(std::shared_ptr<State> state,std::shared_ptr<ResultStore> service):state_(std::move(state)),service_(std::move(service)){}
    std::shared_ptr<State> state_;
    std::shared_ptr<ResultStore> service_;
};
}
