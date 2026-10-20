#pragma once
#include "lexongine/model.hpp"
namespace lexongine {
struct RequiredEvidenceSpec {
    std::string name, format, absolute_path, open_policy;
    std::uint64_t max_bytes, max_records;
    std::string flush_policy, failure;
    friend bool operator==(const RequiredEvidenceSpec &, const RequiredEvidenceSpec &) = default;
};
struct EvidenceReference {
    std::string name, absolute_path;
    std::uint64_t byte_count;
    Sha256 digest;
    bool finalized;
    friend bool operator==(const EvidenceReference &, const EvidenceReference &) = default;
};
struct EntryReference {
    std::string artifact_id;
    Sha256 identity_digest, manifest_digest;
    std::uint64_t manifest_bytes;
    Sha256 payload_digest;
    std::uint64_t payload_bytes;
    friend bool operator==(const EntryReference &, const EntryReference &) = default;
};
} // namespace lexongine
