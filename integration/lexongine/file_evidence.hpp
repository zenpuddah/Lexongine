#pragma once

#include "lexongine/execution.hpp"

namespace lexongine {
struct FileEvidenceLimits {
    std::uint64_t metadata_bytes, record_bytes, digest_scratch_bytes;
};
class FileEvidence {
  public:
    [[nodiscard]] static Result<FileEvidence> create(RetentionLogConfiguration, std::string run,
                                                     std::string clock,
                                                     std::vector<std::string> producers,
                                                     std::shared_ptr<ResourceAccount>,
                                                     FileEvidenceLimits) noexcept;
    [[nodiscard]] ExecutionEvidence callbacks() const;
    [[nodiscard]] Result<EvidenceReference> reference() const noexcept;

  private:
    struct Data;
    explicit FileEvidence(std::shared_ptr<Data> data) noexcept : data_(std::move(data)) {}
    std::shared_ptr<Data> data_;
};
} // namespace lexongine
