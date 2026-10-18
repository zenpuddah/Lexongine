#pragma once
#include "lexongine/evidence.hpp"
#include "lexongine/results.hpp"
#include <filesystem>
namespace lexongine::files {
struct Bytes { Reservation charge; std::string value; };
[[nodiscard]] Result<Sha256> digest(std::span<const std::byte>) noexcept;
[[nodiscard]] Result<Sha256> digest(std::string_view) noexcept;
[[nodiscard]] Result<Bytes> read(const std::filesystem::path&, std::shared_ptr<ResourceAccount>, std::uint64_t maximum) noexcept;
[[nodiscard]] Result<EvidenceReference> write_new(std::string name, const std::filesystem::path&, std::string_view,
    std::shared_ptr<ResourceAccount>, std::uint64_t maximum, FailureCode failure) noexcept;
[[nodiscard]] Result<void> verify(const EvidenceReference&, std::shared_ptr<ResourceAccount>, std::uint64_t maximum) noexcept;
[[nodiscard]] Result<EvidenceReference> finish_open(int& descriptor, std::string name, const std::filesystem::path&, std::string_view,
    std::shared_ptr<ResourceAccount>, std::uint64_t maximum, FailureCode failure) noexcept;
[[nodiscard]] std::string unique_id();
class DirectoryLock {
public:
    static Result<std::shared_ptr<DirectoryLock>> acquire(const std::filesystem::path&, std::shared_ptr<ResourceAccount>, std::uint64_t metadata_bytes) noexcept;
    ~DirectoryLock() noexcept;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] Result<std::vector<std::pair<std::string,std::uint64_t>>> inventory(std::uint64_t maximum_files) const noexcept;
private:
    DirectoryLock(Reservation charge,std::filesystem::path path,int fd,std::uint64_t metadata_bytes):charge_(std::move(charge)),path_(std::move(path)),fd_(fd),metadata_bytes_(metadata_bytes){}
    Reservation charge_;
    std::filesystem::path path_;
    int fd_;
    std::uint64_t metadata_bytes_;
};
// Atomic no-replace rename. An error after an attempted rename is conservatively
// unresolved; the store reconciles the authoritative final path under its lock.
[[nodiscard]] Result<void> publish(const std::filesystem::path& temporary,const std::filesystem::path& final) noexcept;
}
